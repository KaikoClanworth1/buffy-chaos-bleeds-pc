/*
 * gpu_vk_draw.inc - the Vulkan backend's context: state, pipelines, draws,
 * clears. Included by gpu_vulkan.c.
 */

/* ── state setters (recorded at the next draw) ─────────────────────────── */

void vk_set_targets(GpuTexture *target, GpuTexture *depth)
{
    st.rt = target;
    st.ds = depth;
}

void vk_set_viewport(const GpuViewport *vp)
{
    st.vp = *vp;
    st.have_vp = 1;
}

void vk_set_scissor(int x, int y, int w, int h)
{
    st.scissor.offset.x = x;
    st.scissor.offset.y = y;
    st.scissor.extent.width = (uint32_t)(w > 0 ? w : 0);
    st.scissor.extent.height = (uint32_t)(h > 0 ? h : 0);
}

void vk_set_blend(GpuBlendState *s) { st.bs = s; }
void vk_set_depth(GpuDepthState *s, uint32_t ref) { st.dss = s; st.sref = ref; }
void vk_set_raster(GpuRasterState *s) { st.rs = s; }
void vk_set_layout(GpuLayout *l) { st.il = l; }

void vk_set_vertex_buffers(uint32_t first, uint32_t n, GpuBuffer *const *b, const uint32_t *strides, const uint32_t *offsets)
{
    uint32_t i;
    for (i = 0; i < n && first + i < 16; i++) {
        st.vb[first + i] = b ? b[i] : NULL;
        st.stride[first + i] = strides ? strides[i] : 0;
        st.off[first + i] = offsets ? offsets[i] : 0;
    }
}

void vk_set_index_buffer(GpuBuffer *b) { st.ib = b; }
void vk_set_topology(GpuTopology t) { st.topo = t; }
void vk_set_shaders(GpuShader *vs, GpuShader *ps) { st.vs = vs; st.ps = ps; }

void vk_set_cbuffer(GpuStage stage, uint32_t slot, GpuBuffer *b, uint32_t first, uint32_t count)
{
    if (slot > 1)
        return;
    st.cb[stage][slot].b = b;
    st.cb[stage][slot].first = first;
    st.cb[stage][slot].count = count;
}

void vk_set_textures(uint32_t first, uint32_t n, GpuTexture *const *t)
{
    uint32_t i;
    for (i = 0; i < n && first + i < 8; i++)
        st.tex[first + i] = t ? t[i] : NULL;
}

void vk_set_samplers(uint32_t first, uint32_t n, GpuSampler *const *s)
{
    uint32_t i;
    for (i = 0; i < n && first + i < 4; i++)
        st.smp[first + i] = s ? s[i] : NULL;
}

/* ── pipelines ─────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t vs, ps, il;                      /* ids (a freed object's address can come back) */
    GpuBlendDesc blend;                       /* (by value: states made twice alike share one) */
    VkFormat color, depth;
    uint32_t topo_class;                      /* 0 points, 1 lines, 2 triangles */
    uint32_t depth_clamp;
} PipeKey;

typedef struct { PipeKey key; VkPipeline pipe; int used; } PipeEntry;
static PipeEntry s_pipes[PIPE_CACHE];
static VkPipelineCache s_pcache;

/* ── the pipeline cache, kept on disk ──────────────────────────────────────
 *
 * A pipeline is the driver's compiled form of a shader pair and its fixed
 * state, built the first time a draw needs that combination -- the hitch a
 * new effect or material costs. The driver's cache of them is saved as
 * ShaderCache\vk_pipelines.bin (beside the .spv shaders) and loaded at
 * start, so the next run builds each one from it instead. A file from
 * another GPU or driver is not used: its header names the device and the
 * driver's cache version (pipelineCacheUUID). Saved when pipelines were
 * added, at most every 10 s (a game closed with Alt+F4 keeps all but the
 * last few seconds' worth). BUFFY_NO_PIPELINE_CACHE=1 turns it off. */
static int s_pcache_dirty;
static uint64_t s_pcache_saved_ms;
static unsigned s_pipes_built;                /* this run, and their build time (logged with each save) */
static double s_pipes_ms;

static double pcache_now_ms(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static int pcache_path(char *path, size_t n)
{
#if defined(_WIN32)
    char *slash;
    GetModuleFileNameA(NULL, path, (DWORD)n);
    slash = strrchr(path, '\\');
    if (!slash)
        return 0;
    snprintf(slash + 1, n - (size_t)(slash + 1 - path), "ShaderCache");
    CreateDirectoryA(path, NULL);
    snprintf(slash + 1, n - (size_t)(slash + 1 - path), "ShaderCache\\vk_pipelines.bin");
    return 1;
#else
    const char *dir = getenv("GPU_SHADER_CACHE");
    if (!dir || !*dir)
        return 0;
    snprintf(path, n, "%s/vk_pipelines.bin", dir);
    return 1;
#endif
}

static void vk_pcache_init(void)
{
    VkPipelineCacheCreateInfo ci;
    VkPhysicalDeviceProperties p;
    char path[512];
    void *data = NULL;
    size_t size = 0;
    FILE *f;
    if (getenv("BUFFY_NO_PIPELINE_CACHE"))
        return;
    vkGetPhysicalDeviceProperties(s_phys, &p);
    if (pcache_path(path, sizeof path) && (f = fopen(path, "rb")) != NULL) {
        long n;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 32 && (data = malloc((size_t)n)) != NULL && fread(data, 1, (size_t)n, f) == (size_t)n) {
            /* VkPipelineCacheHeaderVersionOne: length, version, vendor, device, uuid[16] */
            const uint32_t *h = (const uint32_t *)data;
            if (h[0] >= 32 && h[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE && h[2] == p.vendorID
                    && h[3] == p.deviceID && !memcmp(h + 4, p.pipelineCacheUUID, VK_UUID_SIZE))
                size = (size_t)n;
        }
        fclose(f);
    }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = size;
    ci.pInitialData = size ? data : NULL;
    if (vkCreatePipelineCache(s_dev, &ci, NULL, &s_pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0;                  /* (a file the driver would not take) */
        ci.pInitialData = NULL;
        if (vkCreatePipelineCache(s_dev, &ci, NULL, &s_pcache) != VK_SUCCESS)
            s_pcache = VK_NULL_HANDLE;
    }
    fprintf(stderr, "  [VK] pipeline cache: %s (%zu KB)\n", size ? "loaded" : "new", size / 1024);
    free(data);
}

/* Write the cache if pipelines were added and the last write was over 10 s
 * ago (or now, with force). */
static void vk_pcache_save(int force)
{
    char path[512], tmp[520];
    size_t size = 0;
    void *data;
    FILE *f;
    uint64_t now;
#if defined(_WIN32)
    now = GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
    if (!s_pcache || !s_pcache_dirty || (!force && now - s_pcache_saved_ms < 10000))
        return;
    s_pcache_saved_ms = now;
    s_pcache_dirty = 0;
    if (!pcache_path(path, sizeof path) || vkGetPipelineCacheData(s_dev, s_pcache, &size, NULL) != VK_SUCCESS || !size)
        return;
    if (!(data = malloc(size)))
        return;
    if (vkGetPipelineCacheData(s_dev, s_pcache, &size, data) == VK_SUCCESS) {
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        if ((f = fopen(tmp, "wb")) != NULL) {
            int ok = fwrite(data, 1, size, f) == size;
            fclose(f);
#if defined(_WIN32)
            if (ok) MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
            if (ok) rename(tmp, path);
#endif
            fprintf(stderr, "  [VK] pipeline cache saved (%zu KB): %u pipelines built this run, %.0f ms\n",
                    size / 1024, s_pipes_built, s_pipes_ms);
        }
    }
    free(data);
}

static uint32_t pipe_hash(const PipeKey *k)
{
    const uint8_t *p = (const uint8_t *)k;
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < sizeof *k; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static VkBlendFactor vkblend(GpuBlend b)
{
    switch (b) {
    case GPU_BLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case GPU_BLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case GPU_BLEND_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case GPU_BLEND_INV_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GPU_BLEND_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case GPU_BLEND_INV_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GPU_BLEND_DEST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
    case GPU_BLEND_INV_DEST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GPU_BLEND_DEST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case GPU_BLEND_INV_DEST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GPU_BLEND_SRC_ALPHA_SAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case GPU_BLEND_FACTOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case GPU_BLEND_INV_FACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp vkblendop(GpuBlendOp o)
{
    switch (o) {
    case GPU_BLENDOP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case GPU_BLENDOP_REV_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case GPU_BLENDOP_MIN: return VK_BLEND_OP_MIN;
    case GPU_BLENDOP_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp vkcmp(GpuCmp c)
{
    switch (c) {
    case GPU_CMP_NEVER: return VK_COMPARE_OP_NEVER;
    case GPU_CMP_LESS: return VK_COMPARE_OP_LESS;
    case GPU_CMP_EQUAL: return VK_COMPARE_OP_EQUAL;
    case GPU_CMP_LESS_EQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case GPU_CMP_GREATER: return VK_COMPARE_OP_GREATER;
    case GPU_CMP_NOT_EQUAL: return VK_COMPARE_OP_NOT_EQUAL;
    case GPU_CMP_GREATER_EQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp vkstencil(GpuStencilOp o)
{
    switch (o) {
    case GPU_STENCIL_ZERO: return VK_STENCIL_OP_ZERO;
    case GPU_STENCIL_REPLACE: return VK_STENCIL_OP_REPLACE;
    case GPU_STENCIL_INCR_SAT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case GPU_STENCIL_DECR_SAT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case GPU_STENCIL_INVERT: return VK_STENCIL_OP_INVERT;
    case GPU_STENCIL_INCR: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case GPU_STENCIL_DECR: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static VkPipeline make_pipeline(const PipeKey *k, GpuShader *vs_, GpuShader *ps_, GpuLayout *il_)
{
    double t0;
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription binds[16];
    VkVertexInputAttributeDescription attrs[16];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vps;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineDynamicStateCreateInfo dy;
    VkPipelineRenderingCreateInfo ri;
    VkGraphicsPipelineCreateInfo pi;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
    };
    uint32_t nb = 0, na = 0, i, slots = 0, inst = 0;
    VkPipeline p = VK_NULL_HANDLE;

    memset(stages, 0, sizeof stages);
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_->mod;
    stages[0].pName = vs_->entry;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps_->mod;
    stages[1].pName = ps_->entry;

    if (il_) {
        for (i = 0; i < il_->n; i++) {
            const GpuLayoutElement *e = &il_->e[i];
            attrs[na].location = e->location;
            attrs[na].binding = e->slot;
            attrs[na].format = vkfmt(e->format);
            attrs[na].offset = e->offset;
            na++;
            slots |= 1u << e->slot;
            if (e->per_instance)
                inst |= 1u << e->slot;
        }
        for (i = 0; i < 16; i++)
            if (slots & (1u << i)) {
                binds[nb].binding = i;
                binds[nb].stride = 0;                    /* (dynamic) */
                binds[nb].inputRate = (inst & (1u << i)) ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
                nb++;
            }
    }
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = nb;
    vi.pVertexBindingDescriptions = binds;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = attrs;
    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = k->topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&vps, 0, sizeof vps);
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.depthClampEnable = k->depth_clamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    memset(&cba, 0, sizeof cba);
    cba.blendEnable = k->blend.enable ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = vkblend(k->blend.src);
    cba.dstColorBlendFactor = vkblend(k->blend.dst);
    cba.colorBlendOp = vkblendop(k->blend.op);
    cba.srcAlphaBlendFactor = vkblend(k->blend.src_alpha);
    cba.dstAlphaBlendFactor = vkblend(k->blend.dst_alpha);
    cba.alphaBlendOp = vkblendop(k->blend.op_alpha);
    cba.colorWriteMask = k->blend.write_mask & 15;
    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = k->color ? 1 : 0;
    cb.pAttachments = &cba;
    memset(&dy, 0, sizeof dy);
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ri.colorAttachmentCount = k->color ? 1 : 0;
    ri.pColorAttachmentFormats = &k->color;
    ri.depthAttachmentFormat = k->depth;
    ri.stencilAttachmentFormat = k->depth;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pi.pNext = &ri;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vps;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = s_pl;
    t0 = pcache_now_ms();
    if (vkCreateGraphicsPipelines(s_dev, s_pcache, 1, &pi, NULL, &p) != VK_SUCCESS) {
        static int shown;
        if (shown++ < 5)
            fprintf(stderr, "  [VK] pipeline creation failed\n");
        return VK_NULL_HANDLE;
    }
    s_pipes_ms += pcache_now_ms() - t0;
    s_pipes_built++;
    s_pcache_dirty++;                             /* (saved with the next present, vk_pcache_save) */
    return p;
}

static VkPipeline pipeline_for(const PipeKey *k, GpuShader *vs, GpuShader *ps, GpuLayout *il)
{
    uint32_t h = pipe_hash(k), i;
    for (i = 0; i < PIPE_CACHE; i++) {
        PipeEntry *e = &s_pipes[(h + i) & (PIPE_CACHE - 1)];
        if (!e->used) {
            e->key = *k;
            e->pipe = make_pipeline(k, vs, ps, il);
            e->used = 1;
            return e->pipe;
        }
        if (!memcmp(&e->key, k, sizeof *k))
            return e->pipe;
    }
    return VK_NULL_HANDLE;
}

/* ── rendering ─────────────────────────────────────────────────────────── */

static void begin_rendering(void)
{
    VkRenderingInfo ri;
    VkRenderingAttachmentInfo ca, da;
    GpuTexture *size = st.rt ? st.rt : st.ds;
    if (s_rendering && s_pass_rt == st.rt && s_pass_ds == st.ds)
        return;
    end_rendering();
    if (st.rt) image_barrier(cmd(), st.rt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (st.ds) image_barrier(cmd(), st.ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    ca.imageView = st.rt ? st.rt->att : VK_NULL_HANDLE;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da = ca;
    da.imageView = st.ds ? st.ds->att : VK_NULL_HANDLE;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    memset(&ri, 0, sizeof ri);
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.extent.width = size->desc.width;
    ri.renderArea.extent.height = size->desc.height;
    ri.layerCount = 1;
    ri.colorAttachmentCount = st.rt ? 1 : 0;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = st.ds ? &da : NULL;
    ri.pStencilAttachment = st.ds ? &da : NULL;
    vkCmdBeginRendering(cmd(), &ri);
    if (s_prof) s_prof_n[PR_PASS]++;
    s_rendering = 1;
    s_pass_rt = st.rt;
    s_pass_ds = st.ds;
    if (st.rt) st.rt->last_use = s_serial;
    if (st.ds) st.ds->last_use = s_serial;
}

/* 0: nothing to draw with (no pipeline) */
static int draw_setup(void)
{
    VkCommandBuffer cb;
    PipeKey k;
    VkPipeline p;
    VkWriteDescriptorSet w[14];
    VkDescriptorBufferInfo bi[2];
    VkDescriptorImageInfo ii[12];
    GpuTexture *size = st.rt ? st.rt : st.ds;
    uint32_t i;
    int cube;
    PROF_BEGIN(0);
    PROF_BEGIN(4);

    /* textures to be sampled: out of rendering, into the read layout */
    for (i = 0; i < 8; i++) {
        GpuTexture *t = st.tex[i];
        if (t && t->img && t->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && t != st.rt) {
            end_rendering();
            image_barrier(cmd(), t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    /* a queued copy into a buffer this draw reads: done first */
    if (s_npc) {
        int pend = st.ib && st.ib->copy_gen == s_copy_gen;
        for (i = 0; i < 16 && !pend; i++)
            pend = st.vb[i] && st.vb[i]->copy_gen == s_copy_gen;
        for (i = 0; i < 2 && !pend; i++)
            pend = (st.cb[0][i].b && st.cb[0][i].b->copy_gen == s_copy_gen)
                || (st.cb[1][i].b && st.cb[1][i].b->copy_gen == s_copy_gen);
        if (pend)
            end_rendering();
    }
    begin_rendering();
    cb = cmd();
    PROF_END(4, PR_TEXLOOP);
    PROF_BEGIN(5);

    {
        VkFormat color = st.rt ? st.rt->fmt : VK_FORMAT_UNDEFINED, depth = st.ds ? st.ds->fmt : VK_FORMAT_UNDEFINED;
        uint32_t tc = st.topo == GPU_TOPO_POINTS ? 0 : (st.topo == GPU_TOPO_LINES || st.topo == GPU_TOPO_LINE_STRIP) ? 1 : 2;
        if (s_last_pipe.pipe && s_last_pipe.vs == st.vs && s_last_pipe.ps == st.ps && s_last_pipe.il == st.il
                && s_last_pipe.bs == st.bs && s_last_pipe.rs == st.rs && s_last_pipe.color == color
                && s_last_pipe.depth == depth && s_last_pipe.topo_class == tc && s_last_pipe.epoch == s_state_epoch) {
            p = s_last_pipe.pipe;                     /* (the same inputs as the last draw) */
        } else {
            memset(&k, 0, sizeof k);
            k.vs = st.vs->id;
            k.ps = st.ps->id;
            k.il = st.il ? st.il->id : 0;
            if (st.bs) k.blend = st.bs->d;
            else { k.blend.src = k.blend.src_alpha = GPU_BLEND_ONE; k.blend.dst = k.blend.dst_alpha = GPU_BLEND_ZERO;
                   k.blend.op = k.blend.op_alpha = GPU_BLENDOP_ADD; k.blend.write_mask = GPU_WRITE_ALL; }
            k.color = color;
            k.depth = depth;
            k.topo_class = tc;
            k.depth_clamp = st.rs ? !st.rs->d.depth_clip : 0;
            p = pipeline_for(&k, st.vs, st.ps, st.il);
            s_last_pipe.vs = st.vs; s_last_pipe.ps = st.ps; s_last_pipe.il = st.il;
            s_last_pipe.bs = st.bs; s_last_pipe.rs = st.rs; s_last_pipe.color = color; s_last_pipe.depth = depth;
            s_last_pipe.topo_class = tc; s_last_pipe.epoch = s_state_epoch; s_last_pipe.pipe = p;
        }
    }
    if (!p)
        return 0;
    if (p != s_bound_pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        s_bound_pipe = p;
    }
    PROF_END(5, PR_PIPE);

    PROF_BEGIN(1);
    {
        /* D3D's viewport (y down) through a negative height */
        VkViewport vp;
        VkRect2D sc;
        GpuViewport v = st.vp;
        if (!st.have_vp) {
            v.x = v.y = 0; v.w = (float)size->desc.width; v.h = (float)size->desc.height; v.min_z = 0; v.max_z = 1;
        }
        vp.x = v.x;
        vp.y = v.y + v.h;
        vp.width = v.w;
        vp.height = -v.h;
        vp.minDepth = v.min_z;
        vp.maxDepth = v.max_z;
        if (st.rs && st.rs->d.scissor) {
            sc = st.scissor;
        } else {
            sc.offset.x = sc.offset.y = 0;
            sc.extent.width = size->desc.width;
            sc.extent.height = size->desc.height;
        }
        if (!s_dyn.valid) {
            static const float ones[4] = { 1, 1, 1, 1 };
            vkCmdSetBlendConstants(cb, ones);
        }
        if (!s_dyn.valid || memcmp(&vp, &s_dyn.vp, sizeof vp)) {
            vkCmdSetViewport(cb, 0, 1, &vp);
            s_dyn.vp = vp;
        }
        if (!s_dyn.valid || memcmp(&sc, &s_dyn.sc, sizeof sc)) {
            vkCmdSetScissor(cb, 0, 1, &sc);
            s_dyn.sc = sc;
        }
    }
    {
        const GpuDepthDesc *d = st.dss ? &st.dss->d : NULL;
        GpuCull cull = st.rs ? st.rs->d.cull : GPU_CULL_NONE;
        int v, all = !s_dyn.valid;
#define DYN(field, value, call) do { v = (int)(value); if (all || s_dyn.field != v) { call; s_dyn.field = v; } } while (0)
        DYN(cull, cull == GPU_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : cull == GPU_CULL_BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
            vkCmdSetCullMode(cb, (VkCullModeFlags)v));
        DYN(ff, (st.rs && st.rs->d.front_ccw) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
            vkCmdSetFrontFace(cb, (VkFrontFace)v));
        DYN(topo, st.topo == GPU_TOPO_POINTS ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                : st.topo == GPU_TOPO_LINES ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                : st.topo == GPU_TOPO_LINE_STRIP ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
                : st.topo == GPU_TOPO_TRIANGLE_STRIP ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            vkCmdSetPrimitiveTopology(cb, (VkPrimitiveTopology)v));
        DYN(dt, d && d->depth_enable && st.ds, vkCmdSetDepthTestEnable(cb, (VkBool32)v));
        DYN(dw, d && d->depth_write && st.ds, vkCmdSetDepthWriteEnable(cb, (VkBool32)v));
        DYN(dcmp, d ? vkcmp(d->depth_func) : VK_COMPARE_OP_ALWAYS, vkCmdSetDepthCompareOp(cb, (VkCompareOp)v));
        DYN(stt, d && d->stencil_enable && st.ds, vkCmdSetStencilTestEnable(cb, (VkBool32)v));
        {
            int sf = vkstencil(d ? d->stencil_fail : GPU_STENCIL_KEEP), sp = vkstencil(d ? d->stencil_pass : GPU_STENCIL_KEEP);
            int sd = vkstencil(d ? d->stencil_depth_fail : GPU_STENCIL_KEEP), sc = vkcmp(d ? d->stencil_func : GPU_CMP_ALWAYS);
            if (all || s_dyn.sfail != sf || s_dyn.spass != sp || s_dyn.sdfail != sd || s_dyn.scmp != sc) {
                vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, (VkStencilOp)sf, (VkStencilOp)sp, (VkStencilOp)sd, (VkCompareOp)sc);
                s_dyn.sfail = sf; s_dyn.spass = sp; s_dyn.sdfail = sd; s_dyn.scmp = sc;
            }
        }
        {
            uint32_t rm = d ? d->stencil_read_mask : 0xFF, wm = d ? d->stencil_write_mask : 0xFF;
            if (all || s_dyn.rmask != rm) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, rm); s_dyn.rmask = rm; }
            if (all || s_dyn.wmask != wm) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, wm); s_dyn.wmask = wm; }
            if (all || s_dyn.ref != st.sref) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, st.sref); s_dyn.ref = st.sref; }
        }
#undef DYN
        s_dyn.valid = 1;
    }
    PROF_END(1, PR_STATE);
    PROF_BEGIN(2);

    /* the descriptors: b0 (the pixel shader's), b1 (the vertex shader's), t0..t7, s0..s3 */
    for (i = 0; i < 2; i++) {
        GpuStage stage = i == 0 ? GPU_PS : GPU_VS;
        GpuBuffer *b = st.cb[stage][i].b ? st.cb[stage][i].b : s_dummyubo;
        VkDeviceSize off;
        VkBuffer vb = buffer_for_draw(b, &off);
        uint32_t first = st.cb[stage][i].b ? st.cb[stage][i].first : 0, count = st.cb[stage][i].b ? st.cb[stage][i].count : 0;
        if (!vb) { b = s_dummyubo; vb = buffer_for_draw(b, &off); first = count = 0; }
        bi[i].buffer = vb;
        bi[i].offset = off + (VkDeviceSize)first * 16;
        bi[i].range = count ? (VkDeviceSize)count * 16 : (VkDeviceSize)b->bytes - (VkDeviceSize)first * 16;
        if (bi[i].offset + bi[i].range > off + b->bytes)
            bi[i].range = off + b->bytes - bi[i].offset;
    }
    PROF_END(2, PR_CB);
    PROF_BEGIN(3);
    memset(ii, 0, sizeof ii);                            /* (padding too: compared with the last draw's) */
    for (i = 0; i < 8; i++) {
        GpuTexture *t = st.tex[i];
        cube = i >= 4;
        if (!t || !t->view || ((t->desc.flags & GPU_TEX_CUBE) != 0) != cube || t == st.rt)
            t = cube ? s_dummycube : s_dummy2d;
        ii[i].sampler = VK_NULL_HANDLE;
        ii[i].imageView = t->view;
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        t->last_use = s_serial;
    }
    for (i = 0; i < 4; i++) {
        GpuSampler *s = st.smp[i] ? st.smp[i] : s_dummysmp;
        ii[8 + i].sampler = s->s;
        ii[8 + i].imageView = VK_NULL_HANDLE;
        ii[8 + i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        s->last_use = s_serial;
    }
    {
        uint32_t nw = 0;
        for (i = 0; i < 14; i++) {
            if (s_bound.valid && (i < 2 ? !memcmp(&bi[i], &s_bound.bi[i], sizeof bi[i])
                                        : !memcmp(&ii[i - 2], &s_bound.ii[i - 2], sizeof ii[0])))
                continue;                                /* (the command buffer has it) */
            memset(&w[nw], 0, sizeof w[nw]);
            w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[nw].dstBinding = i;
            w[nw].descriptorCount = 1;
            w[nw].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                 : i < 10 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            if (i < 2) w[nw].pBufferInfo = &bi[i];
            else w[nw].pImageInfo = &ii[i - 2];
            nw++;
        }
        if (nw)
            vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pl, 0, nw, w);
        memcpy(s_bound.bi, bi, sizeof bi);
        memcpy(s_bound.ii, ii, sizeof ii);
        s_bound.valid = 1;
    }
    PROF_END(3, PR_DESC);

    /* vertex buffers: every slot the layout reads (an unbound one: zeros) */
    PROF_BEGIN(6);
    if (st.il) {
        VkBuffer bufs[16];
        VkDeviceSize offs[16], strides[16];
        uint32_t slots = 0;
        for (i = 0; i < st.il->n; i++)
            slots |= 1u << st.il->e[i].slot;
        for (i = 0; i < 16; i++) {
            VkDeviceSize o = 0;
            GpuBuffer *b = st.vb[i];
            if (!(slots & (1u << i)))
                continue;
            bufs[i] = b ? buffer_for_draw(b, &o) : buffer_for_draw(s_dummyvb, &o);
            offs[i] = o + (b ? st.off[i] : 0);
            strides[i] = b ? st.stride[i] : 0;
            if (s_bound.vb[i] == bufs[i] && s_bound.voff[i] == offs[i] && s_bound.vstride[i] == strides[i])
                continue;                                /* (bound already) */
            vkCmdBindVertexBuffers2(cb, i, 1, &bufs[i], &offs[i], NULL, &strides[i]);
            s_bound.vb[i] = bufs[i];
            s_bound.voff[i] = offs[i];
            s_bound.vstride[i] = strides[i];
        }
    }
    PROF_END(6, PR_VB);
    PROF_END(0, PR_SETUP);
    return 1;
}

void vk_draw(uint32_t vertices, uint32_t first)
{
    if ((!st.rt && !st.ds) || !st.vs || !st.ps)
        return;
    if (draw_setup())
        vkCmdDraw(cmd(), vertices, 1, first, 0);
}

static void bind_index(void)
{
    VkDeviceSize off;
    VkBuffer b = buffer_for_draw(st.ib, &off);
    if (b == s_bound.ib && off == s_bound.ioff)
        return;                                          /* (bound already) */
    vkCmdBindIndexBuffer(cmd(), b, off, VK_INDEX_TYPE_UINT32);
    s_bound.ib = b;
    s_bound.ioff = off;
}

void vk_draw_indexed(uint32_t indices, uint32_t first_index, int32_t base_vertex)
{
    if ((!st.rt && !st.ds) || !st.vs || !st.ps || !st.ib)
        return;
    if (!draw_setup())
        return;
    bind_index();
    vkCmdDrawIndexed(cmd(), indices, 1, first_index, base_vertex, 0);
}

void vk_draw_indexed_instanced(uint32_t indices, uint32_t instances, uint32_t first_index, int32_t base_vertex)
{
    if ((!st.rt && !st.ds) || !st.vs || !st.ps || !st.ib)
        return;
    if (!draw_setup())
        return;
    bind_index();
    vkCmdDrawIndexed(cmd(), indices, instances, first_index, base_vertex, 0);
}

/* ── clears ────────────────────────────────────────────────────────────── */

void vk_clear_target(GpuTexture *t, const float rgba[4])
{
    VkClearColorValue c;
    VkImageSubresourceRange r;
    if (!t || !t->img)
        return;
    memcpy(c.float32, rgba, sizeof c.float32);
    if (s_rendering && s_pass_rt == t) {
        VkClearAttachment a;
        VkClearRect rect;
        memset(&a, 0, sizeof a);
        a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        a.clearValue.color = c;
        memset(&rect, 0, sizeof rect);
        rect.rect.extent.width = t->desc.width;
        rect.rect.extent.height = t->desc.height;
        rect.layerCount = 1;
        vkCmdClearAttachments(cmd(), 1, &a, 1, &rect);
        return;
    }
    end_rendering();
    image_barrier(cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&r, 0, sizeof r);
    r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.levelCount = 1;
    r.layerCount = 1;
    vkCmdClearColorImage(cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
    t->last_use = s_serial;
}

void vk_clear_depth(GpuTexture *t, unsigned flags, float z, uint8_t stencil)
{
    VkClearDepthStencilValue v;
    VkImageSubresourceRange r;
    VkImageAspectFlags asp = ((flags & GPU_CLEAR_DEPTH) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0)
                           | ((flags & GPU_CLEAR_STENCIL) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (!t || !t->img || !asp)
        return;
    v.depth = z;
    v.stencil = stencil;
    if (s_rendering && s_pass_ds == t) {
        VkClearAttachment a;
        VkClearRect rect;
        memset(&a, 0, sizeof a);
        a.aspectMask = asp;
        a.clearValue.depthStencil = v;
        memset(&rect, 0, sizeof rect);
        rect.rect.extent.width = t->desc.width;
        rect.rect.extent.height = t->desc.height;
        rect.layerCount = 1;
        vkCmdClearAttachments(cmd(), 1, &a, 1, &rect);
        return;
    }
    end_rendering();
    image_barrier(cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&r, 0, sizeof r);
    r.aspectMask = asp;
    r.levelCount = 1;
    r.layerCount = 1;
    vkCmdClearDepthStencilImage(cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    t->last_use = s_serial;
}
