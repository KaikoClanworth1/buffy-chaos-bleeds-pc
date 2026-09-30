/*
 * gpu_vk_res.inc - the Vulkan backend's resources: buffers, textures,
 * samplers, shaders, layouts, state objects. Included by gpu_vulkan.c.
 */

/* ── buffers ───────────────────────────────────────────────────────────── */

static int make_vkbuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, int host, VkBuffer *buf, VmaAllocation *mem, uint8_t **map)
{
    VkBufferCreateInfo bi;
    VmaAllocationCreateInfo ai;
    VmaAllocationInfo inf;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes ? bytes : 16;
    bi.usage = usage;
    memset(&ai, 0, sizeof ai);
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (host)
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (vmaCreateBuffer(s_vma, &bi, &ai, buf, mem, &inf) != VK_SUCCESS)
        return 0;
    if (map)
        *map = host ? (uint8_t *)inf.pMappedData : NULL;
    return 1;
}

static VkBufferUsageFlags buf_usage(unsigned bind)
{
    return ((bind & GPU_BIND_VERTEX) ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT : 0)
         | ((bind & GPU_BIND_INDEX) ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT : 0)
         | ((bind & GPU_BIND_CONSTANT) ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : 0)
         | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
}

/* Data into a device-local buffer at `off` (a staged copy, outside rendering). */
static void buffer_upload(GpuBuffer *b, uint32_t off, const void *data, uint32_t bytes)
{
    VkBuffer sb;
    VkDeviceSize so;
    VkBufferCopy c;
    uint8_t *p;
    if (!on_render_thread()) {
        OneShot o;
        if (!oneshot_begin(&o, bytes))
            return;
        memcpy(o.stage_ptr, data, bytes);
        c.srcOffset = 0; c.dstOffset = off; c.size = bytes;
        vkCmdCopyBuffer(o.cb, o.stage, b->buf, 1, &c);
        oneshot_end(&o);
        return;
    }
    if (!(p = staging_alloc(bytes, &sb, &so)))
        return;
    memcpy(p, data, bytes);
    end_rendering();
    global_barrier(cmd());
    c.srcOffset = so; c.dstOffset = off; c.size = bytes;
    vkCmdCopyBuffer(cmd(), sb, b->buf, 1, &c);
    global_barrier(cmd());
    b->last_use = s_serial;
}

GpuBuffer *vk_buffer_create(uint32_t bytes, unsigned bind, GpuUsage usage, const void *init)
{
    GpuBuffer *b = (GpuBuffer *)calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->bind = bind;
    b->usage = usage;
    b->bytes = bytes;
    b->refs = 1;
    if (usage == GPU_USAGE_DYNAMIC && (bind & GPU_BIND_CONSTANT)) {
        if (!(b->shadow = (uint8_t *)calloc(1, bytes ? bytes : 16))) {
            free(b);
            return NULL;
        }
        if (init)
            memcpy(b->shadow, init, bytes);
        b->dirty = 1;
        return b;
    }
    if (usage == GPU_USAGE_DYNAMIC) {
        if (!make_vkbuffer(bytes, buf_usage(bind), 1, &b->ver[0].buf, &b->ver[0].mem, &b->ver[0].map)) {
            free(b);
            return NULL;
        }
        b->nver = 1;
        if (init)
            memcpy(b->ver[0].map, init, bytes);
        return b;
    }
    if (!make_vkbuffer(bytes, buf_usage(bind), 0, &b->buf, &b->mem, NULL)) {
        free(b);
        return NULL;
    }
    if (init)
        buffer_upload(b, 0, init, bytes);
    return b;
}

void vk_buffer_addref(GpuBuffer *b)
{
    if (b) atomic_inc(&b->refs);
}

void vk_buffer_release(GpuBuffer *b)
{
    int i;
    if (!b || atomic_dec(&b->refs) != 0)
        return;
    if (b->buf)
        bury(DEAD_BUFFER, b->buf, b->mem, b->last_use);
    for (i = 0; i < b->nver; i++)
        bury(DEAD_BUFFER, b->ver[i].buf, b->ver[i].mem, b->ver[i].last_use);
    if (b->shadow)
        bury(DEAD_FREE, b->shadow, NULL, 0);
    free(b);
}

void *vk_buffer_map(GpuBuffer *b, GpuMap how)
{
    int i;
    if (b->shadow) {
        b->dirty = 1;                               /* (copied into the ring when next bound) */
        return b->shadow;
    }
    if (b->nver) {
        if (how == GPU_MAP_DISCARD && b->ver[b->cur].last_use > s_done_serial) {
            /* the GPU may still read this version: another it has finished with */
            for (i = 0; i < b->nver; i++)
                if (b->ver[i].last_use <= s_done_serial)
                    break;
            if (i == b->nver && b->nver < MAX_VERSIONS
                    && make_vkbuffer(b->bytes, buf_usage(b->bind), 1, &b->ver[i].buf, &b->ver[i].mem, &b->ver[i].map)) {
                b->ver[i].last_use = 0;
                b->nver++;
            }
            if (i == b->nver) {
                /* all in use and no room: wait for the oldest submission */
                if (s_prof) s_prof_n[PR_MAPWAIT]++;
                flush_wait();
                i = b->cur;
            }
            b->cur = i;
        }
        return b->ver[b->cur].map;
    }
    return NULL;                                    /* (DEFAULT buffers are written with update) */
}

void vk_buffer_unmap(GpuBuffer *b)
{
    (void)b;                                        /* (persistently mapped, host-coherent) */
}

void vk_buffer_update(GpuBuffer *b, const void *data)
{
    if (b->shadow) {
        memcpy(b->shadow, data, b->bytes);
        b->dirty = 1;
    } else if (b->nver) {
        void *p = vk_buffer_map(b, GPU_MAP_DISCARD);
        if (p) memcpy(p, data, b->bytes);
    } else {
        buffer_upload(b, 0, data, b->bytes);
    }
}

void vk_buffer_copy(GpuBuffer *dst, uint32_t dst_off, GpuBuffer *src, uint32_t src_off, uint32_t bytes)
{
    VkBufferCopy c;
    VkBuffer sb = src->buf ? src->buf : src->nver ? src->ver[src->cur].buf : VK_NULL_HANDLE;
    if (!sb || !dst->buf || !bytes)
        return;
    c.srcOffset = src_off; c.dstOffset = dst_off; c.size = bytes;
    if (s_npc && (src->copy_gen == s_copy_gen || dst->src_gen == s_copy_gen))
        end_rendering();                              /* (the queue's copies are unordered: it goes first) */
    if (s_npc && s_pc[s_npc - 1].src == sb && s_pc[s_npc - 1].dst == dst->buf
            && s_pc[s_npc - 1].c.srcOffset + s_pc[s_npc - 1].c.size == src_off
            && s_pc[s_npc - 1].c.dstOffset + s_pc[s_npc - 1].c.size == dst_off) {
        s_pc[s_npc - 1].c.size += bytes;              /* (continues the last one) */
    } else {
        if (s_npc == s_cappc) {
            int cap = s_cappc ? s_cappc * 2 : 1024;
            PendingCopy *n = (PendingCopy *)realloc(s_pc, (size_t)cap * sizeof *n);
            if (!n) {
                end_rendering();                      /* (no room: done now) */
                global_barrier(cmd());
                vkCmdCopyBuffer(cmd(), sb, dst->buf, 1, &c);
                global_barrier(cmd());
                goto done;
            }
            s_pc = n;
            s_cappc = cap;
        }
        s_pc[s_npc].src = sb;
        s_pc[s_npc].dst = dst->buf;
        s_pc[s_npc].c = c;
        s_npc++;
    }
    dst->copy_gen = s_copy_gen;
    src->src_gen = s_copy_gen;
done:
    dst->last_use = s_serial;
    if (src->buf) src->last_use = s_serial;
    else src->ver[src->cur].last_use = s_serial;
}

/* The buffer and offset a draw reads (a constant buffer's shadow into the ring). */
static VkBuffer buffer_for_draw(GpuBuffer *b, VkDeviceSize *off)
{
    *off = 0;
    if (b->shadow) {
        if (b->dirty || b->rserial != s_serial) {
            uint8_t *p = ring_alloc(b->bytes, s_ubo_align, &b->rbuf, &b->roff);
            if (!p) {
                flush_wait();                        /* (ring full: a new submission's ring) */
                p = ring_alloc(b->bytes, s_ubo_align, &b->rbuf, &b->roff);
                if (!p)
                    return VK_NULL_HANDLE;
            }
            memcpy(p, b->shadow, b->bytes);
            b->dirty = 0;
            b->rserial = s_serial;
        }
        *off = b->roff;
        return b->rbuf;
    }
    if (b->nver) {
        b->ver[b->cur].last_use = s_serial;
        return b->ver[b->cur].buf;
    }
    b->last_use = s_serial;
    return b->buf;
}

/* ── textures ──────────────────────────────────────────────────────────── */

static VkImageView make_view(GpuTexture *t, int attachment)
{
    VkImageViewCreateInfo vi;
    VkImageView v = VK_NULL_HANDLE;
    int cube = (t->desc.flags & GPU_TEX_CUBE) && !attachment;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->img;
    vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t->fmt;
    vi.subresourceRange.aspectMask = attachment ? t->aspect : (t->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : t->aspect);
    vi.subresourceRange.levelCount = attachment ? 1 : t->desc.mips;
    vi.subresourceRange.layerCount = cube ? 6 : 1;
    if (vkCreateImageView(s_dev, &vi, NULL, &v) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return v;
}

/* All the subresources' data into a texture (in `cb`), from staging memory
 * `stage` at `so`. */
static void record_texture_upload(VkCommandBuffer cb, GpuTexture *t, const GpuSubresource *init, uint8_t *sp, VkBuffer sb, VkDeviceSize so)
{
    uint32_t faces = (t->desc.flags & GPU_TEX_CUBE) ? 6 : 1, f, l;
    VkBufferImageCopy regions[6 * 16];
    uint32_t nreg = 0;
    VkDeviceSize at = 0;
    for (f = 0; f < faces; f++)
        for (l = 0; l < t->desc.mips; l++) {
            uint32_t w = t->desc.width >> l ? t->desc.width >> l : 1, h = t->desc.height >> l ? t->desc.height >> l : 1;
            uint32_t row, y, rows = is_bc(t->desc.format) ? (h + 3) / 4 : h;
            const GpuSubresource *s = &init[f * t->desc.mips + l];
            VkDeviceSize bytes = level_bytes(t->desc.format, w, h, &row);
            for (y = 0; y < rows; y++)
                memcpy(sp + at + (VkDeviceSize)y * row, (const uint8_t *)s->data + (size_t)y * s->pitch, row);
            memset(&regions[nreg], 0, sizeof regions[nreg]);
            regions[nreg].bufferOffset = so + at;
            regions[nreg].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            regions[nreg].imageSubresource.mipLevel = l;
            regions[nreg].imageSubresource.baseArrayLayer = f;
            regions[nreg].imageSubresource.layerCount = 1;
            regions[nreg].imageExtent.width = w;
            regions[nreg].imageExtent.height = h;
            regions[nreg].imageExtent.depth = 1;
            nreg++;
            at += (bytes + 15) & ~(VkDeviceSize)15;
        }
    image_barrier(cb, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cb, sb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nreg, regions);
    image_barrier(cb, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static VkDeviceSize texture_bytes(const GpuTexture *t)
{
    uint32_t faces = (t->desc.flags & GPU_TEX_CUBE) ? 6 : 1, f, l;
    VkDeviceSize n = 0;
    for (f = 0; f < faces; f++)
        for (l = 0; l < t->desc.mips; l++) {
            uint32_t w = t->desc.width >> l ? t->desc.width >> l : 1, h = t->desc.height >> l ? t->desc.height >> l : 1;
            n += (level_bytes(t->desc.format, w, h, NULL) + 15) & ~(VkDeviceSize)15;
        }
    return n;
}

GpuTexture *vk_texture_create(const GpuTextureDesc *desc, const GpuSubresource *init)
{
    GpuTexture *t = (GpuTexture *)calloc(1, sizeof *t);
    VkImageCreateInfo ii;
    VmaAllocationCreateInfo ai;
    if (!t)
        return NULL;
    t->desc = *desc;
    if (!t->desc.mips)
        t->desc.mips = 1;
    t->refs = 1;
    t->fmt = vkfmt(desc->format);
    t->aspect = desc->format == GPU_FMT_D24S8 ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc->flags & GPU_TEX_READBACK) {
        if (!make_vkbuffer((VkDeviceSize)desc->width * desc->height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, &t->rb, &t->rb_mem, NULL)) {
            free(t);
            return NULL;
        }
        {
            /* (host-readable: a mapped, random-access allocation) */
            VmaAllocationInfo inf;
            vmaDestroyBuffer(s_vma, t->rb, t->rb_mem);
            {
                VkBufferCreateInfo bi;
                VmaAllocationCreateInfo vai;
                memset(&bi, 0, sizeof bi);
                bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
                bi.size = (VkDeviceSize)desc->width * desc->height * 4;
                bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                memset(&vai, 0, sizeof vai);
                vai.usage = VMA_MEMORY_USAGE_AUTO;
                vai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                if (vmaCreateBuffer(s_vma, &bi, &vai, &t->rb, &t->rb_mem, &inf) != VK_SUCCESS) {
                    free(t);
                    return NULL;
                }
            }
            t->rb_map = (uint8_t *)inf.pMappedData;
        }
        return t;
    }
    memset(&ii, 0, sizeof ii);
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.flags = (desc->flags & GPU_TEX_CUBE) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = t->fmt;
    ii.extent.width = desc->width;
    ii.extent.height = desc->height;
    ii.extent.depth = 1;
    ii.mipLevels = t->desc.mips;
    ii.arrayLayers = (desc->flags & GPU_TEX_CUBE) ? 6 : 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
             | ((desc->flags & GPU_TEX_SAMPLED) ? VK_IMAGE_USAGE_SAMPLED_BIT : 0)
             | ((desc->flags & GPU_TEX_TARGET) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0)
             | ((desc->flags & GPU_TEX_DEPTH) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0);
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    memset(&ai, 0, sizeof ai);
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(s_vma, &ii, &ai, &t->img, &t->mem, NULL) != VK_SUCCESS) {
        free(t);
        return NULL;
    }
    if (desc->flags & GPU_TEX_SAMPLED)
        t->view = make_view(t, 0);
    if (desc->flags & (GPU_TEX_TARGET | GPU_TEX_DEPTH))
        t->att = make_view(t, 1);
    if (init) {
        VkDeviceSize bytes = texture_bytes(t);
        if (on_render_thread()) {
            VkBuffer sb;
            VkDeviceSize so;
            uint8_t *sp = staging_alloc(bytes, &sb, &so);
            if (sp) {
                end_rendering();
                record_texture_upload(cmd(), t, init, sp, sb, so);
                t->last_use = s_serial;
            }
        } else {
            OneShot o;
            if (oneshot_begin(&o, bytes)) {
                record_texture_upload(o.cb, t, init, o.stage_ptr, o.stage, 0);
                oneshot_end(&o);
            }
        }
    }
    return t;
}

static GpuTexture *make_dummy(int cube)
{
    static const uint32_t white[6] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    GpuTextureDesc d;
    GpuSubresource s[6];
    int i;
    memset(&d, 0, sizeof d);
    d.width = d.height = 1;
    d.mips = 1;
    d.format = GPU_FMT_BGRA8;
    d.flags = GPU_TEX_SAMPLED | (cube ? GPU_TEX_CUBE : 0);
    for (i = 0; i < 6; i++) {
        s[i].data = &white[i];
        s[i].pitch = 4;
    }
    return vk_texture_create(&d, s);
}

void vk_texture_addref(GpuTexture *t)
{
    if (t) atomic_inc(&t->refs);
}

void vk_texture_release(GpuTexture *t)
{
    if (!t || atomic_dec(&t->refs) != 0)
        return;
    if (t->view) bury(DEAD_VIEW, t->view, NULL, t->last_use);
    if (t->att) bury(DEAD_VIEW, t->att, NULL, t->last_use);
    if (t->img && !t->swapchain) bury(DEAD_IMAGE, t->img, t->mem, t->last_use);
    if (t->rb) bury(DEAD_BUFFER, t->rb, t->rb_mem, t->rb_serial);
    free(t);
}

void vk_texture_desc(const GpuTexture *t, GpuTextureDesc *out)
{
    *out = t->desc;
}

void vk_texture_update(GpuTexture *t, uint32_t face, uint32_t mip, const void *data, uint32_t pitch)
{
    uint32_t w = t->desc.width >> mip ? t->desc.width >> mip : 1, h = t->desc.height >> mip ? t->desc.height >> mip : 1;
    uint32_t row, y, rows = is_bc(t->desc.format) ? (h + 3) / 4 : h;
    VkDeviceSize bytes = level_bytes(t->desc.format, w, h, &row), so;
    VkBuffer sb;
    VkBufferImageCopy r;
    uint8_t *sp;
    PROF_BEGIN(0);
    sp = staging_alloc(bytes, &sb, &so);
    if (!sp)
        return;
    for (y = 0; y < rows; y++)
        memcpy(sp + (VkDeviceSize)y * row, (const uint8_t *)data + (size_t)y * pitch, row);
    end_rendering();
    image_barrier(cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&r, 0, sizeof r);
    r.bufferOffset = so;
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.mipLevel = mip;
    r.imageSubresource.baseArrayLayer = face;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = w; r.imageExtent.height = h; r.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd(), sb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    image_barrier(cmd(), t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->last_use = s_serial;
    PROF_END(0, PR_TEXUP);
}

void vk_texture_copy_region(GpuTexture *dst, uint32_t dx, uint32_t dy, GpuTexture *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h)
{
    end_rendering();
    image_barrier(cmd(), src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (dst->rb) {
        /* into a readback buffer: tightly packed rows of the destination's width */
        VkBufferImageCopy r;
        memset(&r, 0, sizeof r);
        r.bufferOffset = ((VkDeviceSize)dy * dst->desc.width + dx) * 4;
        r.bufferRowLength = dst->desc.width;
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        r.imageSubresource.layerCount = 1;
        r.imageOffset.x = (int32_t)sx; r.imageOffset.y = (int32_t)sy;
        r.imageExtent.width = w; r.imageExtent.height = h; r.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd(), src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->rb, 1, &r);
        global_barrier(cmd());
        dst->rb_serial = s_serial;
        dst->desc.format = src->desc.format;         /* (read converts RGBA) */
    } else {
        VkImageCopy c;
        image_barrier(cmd(), dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&c, 0, sizeof c);
        c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
        c.srcOffset.x = (int32_t)sx; c.srcOffset.y = (int32_t)sy;
        c.dstOffset.x = (int32_t)dx; c.dstOffset.y = (int32_t)dy;
        c.extent.width = w; c.extent.height = h; c.extent.depth = 1;
        vkCmdCopyImage(cmd(), src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        dst->last_use = s_serial;
    }
    src->last_use = s_serial;
}

void vk_texture_copy(GpuTexture *dst, GpuTexture *src)
{
    uint32_t w = src->desc.width < dst->desc.width ? src->desc.width : dst->desc.width;
    uint32_t h = src->desc.height < dst->desc.height ? src->desc.height : dst->desc.height;
    vk_texture_copy_region(dst, 0, 0, src, 0, 0, w, h);
}

int vk_texture_read(GpuTexture *t, void *out, int no_wait)
{
    uint32_t i, n;
    uint8_t *o = (uint8_t *)out;
    if (!t->rb_map)
        return 0;
    if (t->rb_serial > s_done_serial) {
        if (no_wait)
            return 0;
        flush_wait();
    }
    vmaInvalidateAllocation(s_vma, t->rb_mem, 0, VK_WHOLE_SIZE);
    n = t->desc.width * t->desc.height;
    memcpy(out, t->rb_map, (size_t)n * 4);
    if (t->desc.format == GPU_FMT_RGBA8)
        for (i = 0; i < n; i++) {
            uint8_t r = o[i * 4];
            o[i * 4] = o[i * 4 + 2];
            o[i * 4 + 2] = r;
        }
    return 1;
}

/* ── samplers ──────────────────────────────────────────────────────────── */

static VkSamplerAddressMode vkaddr(GpuAddress a)
{
    switch (a) {
    case GPU_ADDRESS_MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case GPU_ADDRESS_CLAMP: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case GPU_ADDRESS_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

GpuSampler *vk_sampler_create(const GpuSamplerDesc *d)
{
    VkSamplerCreateInfo si;
    VkSamplerCustomBorderColorCreateInfoEXT cb;
    GpuSampler *s = (GpuSampler *)calloc(1, sizeof *s);
    if (!s)
        return NULL;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = d->mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = d->min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = d->mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = vkaddr(d->u);
    si.addressModeV = vkaddr(d->v);
    si.addressModeW = vkaddr(d->w);
    si.maxLod = d->max_lod >= 1000.0f ? VK_LOD_CLAMP_NONE : d->max_lod;
    if (s_custom_border) {
        memset(&cb, 0, sizeof cb);
        cb.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT;
        memcpy(cb.customBorderColor.float32, d->border, sizeof d->border);
        cb.format = VK_FORMAT_UNDEFINED;
        si.pNext = &cb;
        si.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    } else {
        /* the nearest of Vulkan's three */
        int white = d->border[0] + d->border[1] + d->border[2] > 1.5f;
        si.borderColor = white ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                       : d->border[3] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    if (vkCreateSampler(s_dev, &si, NULL, &s->s) != VK_SUCCESS) {
        free(s);
        return NULL;
    }
    s->refs = 1;
    return s;
}

void vk_sampler_addref(GpuSampler *s)
{
    if (s) atomic_inc(&s->refs);
}

void vk_sampler_release(GpuSampler *s)
{
    if (!s || atomic_dec(&s->refs) != 0)
        return;
    bury(DEAD_SAMPLER, s->s, NULL, s->last_use);
    free(s);
}

/* ── shaders (HLSL -> SPIR-V, cached on disk as ShaderCache\<hash>.spv) ── */

static int spv_cache_path(const char *hlsl, const char *entry, GpuStage stage, char *path, size_t n)
{
#if defined(_WIN32)
    static char dir[MAX_PATH];
    uint64_t h = 1469598103934665603ull;
    const char *p;
    if (!dir[0]) {
        char *slash;
        GetModuleFileNameA(NULL, dir, MAX_PATH);
        slash = strrchr(dir, '\\');
        if (slash) strcpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - dir), "ShaderCache");
        CreateDirectoryA(dir, NULL);
    }
    for (p = hlsl; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    for (p = entry; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    h = (h ^ (uint64_t)(stage + 0x53507631u)) * 1099511628211ull;   /* ("SPv1") */
    snprintf(path, n, "%s\\%016llx.spv", dir, (unsigned long long)h);
    return 1;
#else
    /* elsewhere (Android): GPU_SHADER_CACHE=<dir>, the app's cache folder */
    const char *dir = getenv("GPU_SHADER_CACHE");
    uint64_t h = 1469598103934665603ull;
    const char *p;
    if (dir && *dir) {
        mkdir(dir, 0755);
        for (p = hlsl; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
        for (p = entry; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
        h = (h ^ (uint64_t)(stage + 0x53507631u)) * 1099511628211ull;
        snprintf(path, n, "%s/%016llx.spv", dir, (unsigned long long)h);
        return 1;
    }
    (void)hlsl; (void)entry; (void)stage; (void)path; (void)n;
    return 0;
#endif
}

GpuShader *vk_shader_create(GpuStage stage, const char *hlsl, const char *entry)
{
    GpuShader *s;
    VkShaderModuleCreateInfo mi;
    uint32_t *words = NULL;
    size_t count = 0;
    char path[512], err[4096];
    FILE *f;
    int cached = 0;
    if (spv_cache_path(hlsl, entry, stage, path, sizeof path) && (f = fopen(path, "rb")) != NULL) {
        long n;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && (n % 4) == 0 && (words = (uint32_t *)malloc((size_t)n)) != NULL && fread(words, 1, (size_t)n, f) == (size_t)n) {
            count = (size_t)n / 4;
            cached = 1;
        }
        fclose(f);
    }
    if (!cached) {
        free(words);
        words = NULL;
        if (!gpu_hlsl_to_spirv(hlsl, entry, stage == GPU_VS ? GPU_STAGE_VERTEX : GPU_STAGE_PIXEL, &words, &count, err, sizeof err)) {
            static int shown;
            if (shown++ < 3)
                fprintf(stderr, "  [VK] shader failed: %s\n%s\n", err, hlsl);
            return NULL;
        }
        if ((f = fopen(path, "wb")) != NULL) {
            fwrite(words, 4, count, f);
            fclose(f);
        }
    }
    s = (GpuShader *)calloc(1, sizeof *s);
    if (!s) {
        free(words);
        return NULL;
    }
    memset(&mi, 0, sizeof mi);
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = count * 4;
    mi.pCode = words;
    if (vkCreateShaderModule(s_dev, &mi, NULL, &s->mod) != VK_SUCCESS) {
        free(words);
        free(s);
        return NULL;
    }
    free(words);
    s->stage = stage;
    s->id = (uint32_t)atomic_inc(&s_next_id);
    snprintf(s->entry, sizeof s->entry, "%s", entry);
    return s;
}

void vk_shader_release(GpuShader *s)
{
    if (!s) return;
    s_state_epoch++;
    vkDestroyShaderModule(s_dev, s->mod, NULL);        /* (pipelines keep their own copy of the code) */
    free(s);
}

/* ── layouts and state objects (descriptions: pipelines are made from them) ── */

GpuLayout *vk_layout_create(const GpuLayoutElement *e, uint32_t n, GpuShader *vs)
{
    GpuLayout *l;
    (void)vs;
    if (n > 16 || !(l = (GpuLayout *)calloc(1, sizeof *l)))
        return NULL;
    memcpy(l->e, e, n * sizeof *e);
    l->n = n;
    l->id = (uint32_t)atomic_inc(&s_next_id);
    return l;
}

void vk_layout_release(GpuLayout *l) { s_state_epoch++; free(l); }

GpuBlendState *vk_blend_create(const GpuBlendDesc *d)
{
    GpuBlendState *s = (GpuBlendState *)calloc(1, sizeof *s);
    if (s) s->d = *d;
    return s;
}

GpuDepthState *vk_depth_create(const GpuDepthDesc *d)
{
    GpuDepthState *s = (GpuDepthState *)calloc(1, sizeof *s);
    if (s) s->d = *d;
    return s;
}

GpuRasterState *vk_raster_create(const GpuRasterDesc *d)
{
    GpuRasterState *s = (GpuRasterState *)calloc(1, sizeof *s);
    if (s) s->d = *d;
    return s;
}

void vk_blend_release(GpuBlendState *s) { s_state_epoch++; free(s); }
void vk_depth_release(GpuDepthState *s) { free(s); }
void vk_raster_release(GpuRasterState *s) { s_state_epoch++; free(s); }
