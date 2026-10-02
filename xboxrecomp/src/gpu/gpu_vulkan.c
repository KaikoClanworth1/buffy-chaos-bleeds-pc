/*
 * gpu_vulkan.c - the GPU layer's Vulkan backend (see gpu.h).
 *
 * Vulkan 1.3: dynamic rendering (no render pass objects), extended dynamic
 * state (culling, topology, depth and stencil set per draw, so a pipeline is
 * keyed by its shaders, vertex layout, blending and target formats only),
 * push descriptors (one set a draw: b0/b1, t0..t7, s0..s3 as gpu_spirv.h
 * maps them), VMA for memory, volk for the functions.
 *
 * Direct3D 11's immediate context is kept as the model: state is set, draws
 * are recorded into the current frame's command buffer, and a present (or a
 * readback) submits it. Rendering is begun lazily at the first draw or clear
 * into a target and ended when the targets change or a copy, upload or
 * transition needs to happen outside it. Images carry their current layout.
 *
 * Buffers: DEFAULT / IMMUTABLE are device-local, written through a staging
 * copy; DYNAMIC vertex / index buffers are host-visible with versions (a map
 * with discard takes a version the GPU has finished with); DYNAMIC constant
 * buffers live in a CPU shadow and are copied into the frame's upload ring
 * when bound (once per submission, or again after a map).
 *
 * Objects released while the GPU may still use them are destroyed once the
 * submission that last used them has finished.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sys/stat.h>
#endif
#if defined(__ANDROID__)
#include <android/native_window.h>
#include <dlfcn.h>
#endif

#include "gpu.h"
#include "gpu_spirv.h"
#include "gpu_vk.h"

#define FRAMES       3                        /* submissions in flight */
#define RING_BYTES   (64u << 20)              /* upload ring a submission */
#define MAX_VERSIONS 16                       /* versions of a dynamic vertex / index buffer */
#define PIPE_CACHE   4096

/* ── small helpers ─────────────────────────────────────────────────────── */

#if defined(_WIN32)
typedef CRITICAL_SECTION Lock;
static void lock_init(Lock *l) { InitializeCriticalSection(l); }
static void lock(Lock *l) { EnterCriticalSection(l); }
static void unlock(Lock *l) { LeaveCriticalSection(l); }
static unsigned long thread_id(void) { return GetCurrentThreadId(); }
static long atomic_inc(volatile long *v) { return InterlockedIncrement(v); }
static long atomic_dec(volatile long *v) { return InterlockedDecrement(v); }
#else
typedef pthread_mutex_t Lock;
static void lock_init(Lock *l)
{
    pthread_mutexattr_t a;                    /* recursive, as a critical section is */
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(l, &a);
    pthread_mutexattr_destroy(&a);
}
static void lock(Lock *l) { pthread_mutex_lock(l); }
static void unlock(Lock *l) { pthread_mutex_unlock(l); }
static unsigned long thread_id(void) { return (unsigned long)pthread_self(); }
static long atomic_inc(volatile long *v) { return __atomic_add_fetch(v, 1, __ATOMIC_SEQ_CST); }
static long atomic_dec(volatile long *v) { return __atomic_sub_fetch(v, 1, __ATOMIC_SEQ_CST); }
#endif

/* BUFFY_VK_PROF=1: where the CPU time goes, averaged a frame, every 300 presents */
enum { PR_SETUP, PR_CB, PR_DESC, PR_STATE, PR_FENCE, PR_SUBMIT, PR_ACQUIRE, PR_PRESENT, PR_TEXUP,
       PR_PIPE, PR_VB, PR_TEXLOOP, PR_PASS, PR_BARRIER, PR_MAPWAIT, PR_FLUSHWAIT, PR_N };
static const char *const k_prof_name[PR_N] = { "setup", "cbuf", "desc", "state", "fence", "submit", "acquire", "present", "texup",
                                                "pipe", "vb", "texloop", "pass", "barrier", "mapwait", "flushwait" };
static int s_prof;
static double s_prof_ms[PR_N];
static uint64_t s_prof_n[PR_N];
static double prof_now(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}
#define PROF_BEGIN(i) double prof_t##i = s_prof ? prof_now() : 0.0
#define PROF_END(t, i) do { if (s_prof) { s_prof_ms[i] += prof_now() - prof_t##t; s_prof_n[i]++; } } while (0)
static void prof_frame(void)
{
    static int frames;
    int i;
    if (!s_prof || ++frames < 300)
        return;
    fprintf(stderr, "  [VKPROF] ms/frame:");
    for (i = 0; i < PR_N; i++)
        fprintf(stderr, " %s %.2f (%.0f)", k_prof_name[i], s_prof_ms[i] / frames, (double)s_prof_n[i] / frames);
    fprintf(stderr, "\n");
    memset(s_prof_ms, 0, sizeof s_prof_ms);
    memset(s_prof_n, 0, sizeof s_prof_n);
    frames = 0;
}

#define VKCHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { fprintf(stderr, "  [VK] %s failed: %d (%s:%d)\n", #x, (int)_r, __FILE__, __LINE__); } } while (0)

/* ── objects ───────────────────────────────────────────────────────────── */

typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *map; uint64_t last_use; } BufVersion;

struct GpuBuffer {
    unsigned bind;
    GpuUsage usage;
    uint32_t bytes;
    volatile long refs;
    uint64_t last_use;
    /* DEFAULT / IMMUTABLE: device-local */
    VkBuffer buf;
    VmaAllocation mem;
    /* DYNAMIC vertex / index: versions */
    BufVersion ver[MAX_VERSIONS];
    int nver, cur;
    /* DYNAMIC constant: CPU shadow, and its copy in the ring */
    uint8_t *shadow;
    int dirty;
    VkBuffer rbuf;
    VkDeviceSize roff;
    uint64_t rserial;                         /* the submission whose ring holds the copy */
    uint64_t copy_gen;                        /* a queued copy into it (s_copy_gen: not done yet) */
    uint64_t src_gen;                         /* a queued copy out of it */
};

struct GpuTexture {
    GpuTextureDesc desc;
    volatile long refs;
    uint64_t last_use;
    VkImage img;
    VmaAllocation mem;
    VkImageView view;                         /* sampling (all mips; cube when a cube) */
    VkImageView att;                          /* attachment (mip 0) */
    VkFormat fmt;
    VkImageAspectFlags aspect;
    VkImageLayout layout;
    int swapchain;                            /* a swap chain's image: not ours to free */
    /* READBACK: a host buffer */
    VkBuffer rb;
    VmaAllocation rb_mem;
    uint8_t *rb_map;
    uint64_t rb_serial;
    GpuFormat soft_bc;                        /* BC data decoded to RGBA8 here (no BC on the GPU): its format */
};

struct GpuSampler { VkSampler s; volatile long refs; uint64_t last_use; };
struct GpuShader { GpuStage stage; VkShaderModule mod; uint32_t id; char entry[64]; };
struct GpuLayout { GpuLayoutElement e[16]; uint32_t n; uint32_t id; };
struct GpuBlendState { GpuBlendDesc d; };
struct GpuDepthState { GpuDepthDesc d; };
struct GpuRasterState { GpuRasterDesc d; };

struct GpuSwapchain {
    void *window;
    VkSurfaceKHR surf;
    VkSwapchainKHR sc;
    VkFormat fmt;
    uint32_t w, h, nimg, cur;
    GpuTexture *tex[8];
    VkSemaphore acq[FRAMES];
    VkSemaphore done[8];
    int acquired;
    int vsync, want_vsync;
    int recreate;
};

/* ── the device and frames ─────────────────────────────────────────────── */

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    int submitted;
    uint64_t serial;
    VkBuffer ring;
    VmaAllocation ring_mem;
    uint8_t *ring_ptr;
    VkDeviceSize ring_pos;
} Frame;

typedef struct { int kind; void *a, *b; uint64_t serial; } Dead;
enum { DEAD_BUFFER, DEAD_IMAGE, DEAD_VIEW, DEAD_SAMPLER, DEAD_FREE };

static VkInstance s_inst;
static VkPhysicalDevice s_phys;
static VkDevice s_dev;
static VkQueue s_queue;
static uint32_t s_qfam;
static VmaAllocator s_vma;
static VkCommandPool s_pool, s_upool;         /* the render thread's, other threads' uploads */
static Lock s_qlock, s_ulock, s_deadlock;
static unsigned long s_render_tid;
static Frame s_frames[FRAMES];
static int s_fi;
static uint64_t s_serial, s_done_serial;
static Dead *s_dead;
static int s_ndead, s_capdead;
static VkSemaphore s_waits[8];
static int s_nwaits;
static char s_adapter[256];
static int s_custom_border;
static int s_soft_bc;                         /* decode BC1-3 on the CPU (the GPU has no BC, or BUFFY_VK_SOFT_BC) */
static VkFormat s_depth_fmt = VK_FORMAT_D24_UNORM_S8_UINT;
static VkDeviceSize s_ubo_align = 256;
static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout s_pl;
static GpuTexture *s_dummy2d, *s_dummycube;
static GpuSampler *s_dummysmp;
static GpuBuffer *s_dummyubo, *s_dummyvb;
static int s_up;
static volatile long s_next_id;               /* shaders and layouts: pipeline keys */

static VkCommandBuffer cmd(void) { return s_frames[s_fi].cmd; }

/* ── deferred destruction ──────────────────────────────────────────────── */

static void destroy_now(const Dead *d)
{
    switch (d->kind) {
    case DEAD_BUFFER: vmaDestroyBuffer(s_vma, (VkBuffer)d->a, (VmaAllocation)d->b); break;
    case DEAD_IMAGE: vmaDestroyImage(s_vma, (VkImage)d->a, (VmaAllocation)d->b); break;
    case DEAD_VIEW: vkDestroyImageView(s_dev, (VkImageView)d->a, NULL); break;
    case DEAD_SAMPLER: vkDestroySampler(s_dev, (VkSampler)d->a, NULL); break;
    case DEAD_FREE: free(d->a); break;
    }
}

static void bury(int kind, void *a, void *b, uint64_t last_use)
{
    Dead d;
    d.kind = kind; d.a = a; d.b = b; d.serial = last_use;
    if (last_use <= s_done_serial) {
        destroy_now(&d);
        return;
    }
    lock(&s_deadlock);
    if (s_ndead == s_capdead) {
        int c = s_capdead ? s_capdead * 2 : 256;
        Dead *n = (Dead *)realloc(s_dead, (size_t)c * sizeof(Dead));
        if (!n) { unlock(&s_deadlock); return; }       /* (leaked rather than freed in use) */
        s_dead = n;
        s_capdead = c;
    }
    s_dead[s_ndead++] = d;
    unlock(&s_deadlock);
}

static void reap(void)
{
    int i, k = 0;
    lock(&s_deadlock);
    for (i = 0; i < s_ndead; i++) {
        if (s_dead[i].serial <= s_done_serial)
            destroy_now(&s_dead[i]);
        else
            s_dead[k++] = s_dead[i];
    }
    s_ndead = k;
    unlock(&s_deadlock);
}

/* ── barriers ──────────────────────────────────────────────────────────── */

static void global_barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier2 mb;
    VkDependencyInfo di;
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cb, &di);
}

static void image_barrier(VkCommandBuffer cb, GpuTexture *t, VkImageLayout to)
{
    VkImageMemoryBarrier2 ib;
    VkDependencyInfo di;
    if (t->layout == to && to != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        return;
    if (s_prof) s_prof_n[PR_BARRIER]++;
    memset(&ib, 0, sizeof ib);
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = t->layout;
    ib.newLayout = to;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = t->img;
    ib.subresourceRange.aspectMask = t->aspect;
    ib.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    ib.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(cb, &di);
    t->layout = to;
}

/* ── the state (the immediate context) ─────────────────────────────────── */

static struct {
    GpuTexture *rt, *ds;
    GpuViewport vp;
    int have_vp;
    VkRect2D scissor;
    GpuBlendState *bs;
    GpuDepthState *dss;
    uint32_t sref;
    GpuRasterState *rs;
    GpuLayout *il;
    GpuBuffer *vb[16];
    uint32_t stride[16], off[16];
    GpuBuffer *ib;
    GpuTopology topo;
    GpuShader *vs, *ps;
    struct { GpuBuffer *b; uint32_t first, count; } cb[2][2];     /* [stage][slot] */
    GpuTexture *tex[8];
    GpuSampler *smp[4];
} st;

static int s_rendering;
static GpuTexture *s_pass_rt, *s_pass_ds;
static VkPipeline s_bound_pipe;

/* What the command buffer already has: dynamic state and the pipeline's
 * inputs, so a draw records only what changed. valid = 0 at each new
 * command buffer; s_state_epoch moves when a state object, shader or layout
 * is released (its address may come back as another). */
static struct {
    int valid;
    VkViewport vp;
    VkRect2D sc;
    int cull, ff, topo, dt, dw, dcmp, stt, sfail, spass, sdfail, scmp;
    uint32_t rmask, wmask, ref;
} s_dyn;

/* Qualcomm's driver loses the dynamic state (cull mode, depth test...) set
 * in an earlier render pass, though Vulkan keeps it for the command buffer:
 * draws then ran with stale state -- walls culled away, the sky showing
 * through. There each pass sets it all again (begin_rendering;
 * BUFFY_VK_DYN_PER_PASS=0/1 overrides, for testing). */
static int s_dyn_per_pass;
static struct {
    GpuShader *vs, *ps;
    GpuLayout *il;
    GpuBlendState *bs;
    GpuRasterState *rs;
    VkFormat color, depth;
    uint32_t topo_class, epoch;
    VkPipeline pipe;
} s_last_pipe;
static uint32_t s_state_epoch = 1;

/* The bindings the command buffer already has: a draw pushes only the
 * descriptors that changed (push descriptors keep the rest) and binds only
 * the vertex / index buffers that moved. valid = 0 at each new one. */
static struct {
    int valid;
    VkDescriptorBufferInfo bi[2];
    VkDescriptorImageInfo ii[12];
    VkBuffer vb[16];
    VkDeviceSize voff[16], vstride[16];
    VkBuffer ib;
    VkDeviceSize ioff;
} s_bound;

/* Buffer copies (gpu_buffer_copy: frame interpolation records every draw's
 * vertices and indices with one) cannot be recorded inside rendering, and
 * ending it for each would reload the targets every draw: they are queued
 * and done together the next time rendering ends -- or first, when a draw
 * would read a buffer one of them writes. */
typedef struct { VkBuffer src, dst; VkBufferCopy c; } PendingCopy;
static PendingCopy *s_pc;
static int s_npc, s_cappc;
static uint64_t s_copy_gen = 1;

static void flush_copies(void)
{
    int i, j;
    VkBufferCopy regions[64];
    if (!s_npc)
        return;
    global_barrier(cmd());
    for (i = 0; i < s_npc; ) {
        int n = 0;
        for (j = i; j < s_npc && n < 64 && s_pc[j].src == s_pc[i].src && s_pc[j].dst == s_pc[i].dst; j++)
            regions[n++] = s_pc[j].c;
        vkCmdCopyBuffer(cmd(), s_pc[i].src, s_pc[i].dst, (uint32_t)n, regions);
        i = j;
    }
    global_barrier(cmd());
    s_npc = 0;
    s_copy_gen++;
}

static void end_rendering(void)
{
    if (s_rendering) {
        vkCmdEndRendering(cmd());
        s_rendering = 0;
        s_pass_rt = s_pass_ds = NULL;
    }
    flush_copies();
}

/* ── frames and submission ─────────────────────────────────────────────── */

static void begin_frame(void)
{
    Frame *f = &s_frames[s_fi];
    VkCommandBufferBeginInfo bi;
    if (f->submitted) {
        PROF_BEGIN(0);
        vkWaitForFences(s_dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
        PROF_END(0, PR_FENCE);
        if (f->serial > s_done_serial)
            s_done_serial = f->serial;
        f->submitted = 0;
    }
    vkResetFences(s_dev, 1, &f->fence);
    reap();
    f->ring_pos = 0;
    f->serial = ++s_serial;
    vkResetCommandBuffer(f->cmd, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f->cmd, &bi);
    s_bound_pipe = VK_NULL_HANDLE;
    s_dyn.valid = 0;
    memset(&s_bound, 0, sizeof s_bound);
}

/* Submits what is recorded (waiting on the acquired swap chain images,
 * signalling `signal`), then starts the next frame. */
static void submit(VkSemaphore signal)
{
    Frame *f = &s_frames[s_fi];
    VkSubmitInfo si;
    VkPipelineStageFlags ws[8];
    int i;
    end_rendering();
    vkEndCommandBuffer(f->cmd);
    for (i = 0; i < s_nwaits; i++)
        ws[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = (uint32_t)s_nwaits;
    si.pWaitSemaphores = s_waits;
    si.pWaitDstStageMask = ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f->cmd;
    si.signalSemaphoreCount = signal ? 1 : 0;
    si.pSignalSemaphores = &signal;
    {
        PROF_BEGIN(0);
        lock(&s_qlock);
        VKCHECK(vkQueueSubmit(s_queue, 1, &si, f->fence));
        unlock(&s_qlock);
        PROF_END(0, PR_SUBMIT);
    }
    s_nwaits = 0;
    f->submitted = 1;
    s_fi = (s_fi + 1) % FRAMES;
    begin_frame();
}

/* Everything recorded, finished by the GPU (readbacks). */
static void flush_wait(void)
{
    int prev = s_fi;
    if (s_prof) s_prof_n[PR_FLUSHWAIT]++;
    submit(VK_NULL_HANDLE);
    vkWaitForFences(s_dev, 1, &s_frames[prev].fence, VK_TRUE, UINT64_MAX);
    if (s_frames[prev].serial > s_done_serial)
        s_done_serial = s_frames[prev].serial;
}

/* The upload ring: `bytes` of this submission's, aligned; NULL if full. */
static uint8_t *ring_alloc(VkDeviceSize bytes, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off)
{
    Frame *f = &s_frames[s_fi];
    VkDeviceSize at = (f->ring_pos + align - 1) & ~(align - 1);
    if (at + bytes > RING_BYTES)
        return NULL;
    f->ring_pos = at + bytes;
    *buf = f->ring;
    *off = at;
    return f->ring_ptr + at;
}

/* A staging buffer of its own (large uploads), freed after this submission. */
static uint8_t *staging_alloc(VkDeviceSize bytes, VkBuffer *buf, VkDeviceSize *off)
{
    VkBufferCreateInfo bi;
    VmaAllocationCreateInfo ai;
    VmaAllocation mem;
    VmaAllocationInfo inf;
    uint8_t *p = ring_alloc(bytes, 16, buf, off);
    if (p)
        return p;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    memset(&ai, 0, sizeof ai);
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;   /* (mapped and never flushed: phones have non-coherent types) */
    if (vmaCreateBuffer(s_vma, &bi, &ai, buf, &mem, &inf) != VK_SUCCESS)
        return NULL;
    *off = 0;
    bury(DEAD_BUFFER, *buf, mem, s_serial);
    return (uint8_t *)inf.pMappedData;
}

/* ── one-shot uploads (textures made on other threads: texture packs) ──── */

typedef struct { VkCommandBuffer cb; VkBuffer stage; VmaAllocation stage_mem; uint8_t *stage_ptr; } OneShot;

static int oneshot_begin(OneShot *o, VkDeviceSize bytes)
{
    VkCommandBufferAllocateInfo ai;
    VkCommandBufferBeginInfo bi;
    VkBufferCreateInfo bci;
    VmaAllocationCreateInfo vai;
    VmaAllocationInfo inf;
    memset(o, 0, sizeof *o);
    lock(&s_ulock);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = s_upool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(s_dev, &ai, &o->cb) != VK_SUCCESS) {
        unlock(&s_ulock);
        return 0;
    }
    if (bytes) {
        memset(&bci, 0, sizeof bci);
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        memset(&vai, 0, sizeof vai);
        vai.usage = VMA_MEMORY_USAGE_AUTO;
        vai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        vai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;   /* (mapped and never flushed: phones have non-coherent types) */
        if (vmaCreateBuffer(s_vma, &bci, &vai, &o->stage, &o->stage_mem, &inf) != VK_SUCCESS) {
            vkFreeCommandBuffers(s_dev, s_upool, 1, &o->cb);
            unlock(&s_ulock);
            return 0;
        }
        o->stage_ptr = (uint8_t *)inf.pMappedData;
    }
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(o->cb, &bi);
    return 1;
}

static void oneshot_end(OneShot *o)
{
    VkSubmitInfo si;
    VkFenceCreateInfo fi;
    VkFence fence;
    vkEndCommandBuffer(o->cb);
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(s_dev, &fi, NULL, &fence);
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &o->cb;
    lock(&s_qlock);
    VKCHECK(vkQueueSubmit(s_queue, 1, &si, fence));
    unlock(&s_qlock);
    vkWaitForFences(s_dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(s_dev, fence, NULL);
    vkFreeCommandBuffers(s_dev, s_upool, 1, &o->cb);
    if (o->stage)
        vmaDestroyBuffer(s_vma, o->stage, o->stage_mem);
    unlock(&s_ulock);
}

static int on_render_thread(void)
{
    return thread_id() == s_render_tid;
}

/* ── formats ───────────────────────────────────────────────────────────── */

static VkFormat vkfmt(GpuFormat f)
{
    switch (f) {
    case GPU_FMT_BGRA8: case GPU_FMT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case GPU_FMT_RGBA8: case GPU_FMT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case GPU_FMT_BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case GPU_FMT_BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case GPU_FMT_BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case GPU_FMT_D24S8: return s_depth_fmt;
    case GPU_FMT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case GPU_FMT_R32G32B32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case GPU_FMT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case GPU_FMT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case GPU_FMT_R32_UINT: return VK_FORMAT_R32_UINT;
    case GPU_FMT_R16G16B16A16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case GPU_FMT_R16G16_SNORM: return VK_FORMAT_R16G16_SNORM;
    case GPU_FMT_R16_SNORM: return VK_FORMAT_R16_SNORM;
    case GPU_FMT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case GPU_FMT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static int is_bc(GpuFormat f) { return f == GPU_FMT_BC1 || f == GPU_FMT_BC2 || f == GPU_FMT_BC3; }

/* Bytes of a w x h level (and its row pitch in the tight layout). */
static VkDeviceSize level_bytes(GpuFormat f, uint32_t w, uint32_t h, uint32_t *row)
{
    if (is_bc(f)) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = f == GPU_FMT_BC1 ? 8 : 16;
        if (row) *row = bw * bb;
        return (VkDeviceSize)bw * bh * bb;
    }
    if (row) *row = w * 4;
    return (VkDeviceSize)w * h * 4;
}

/* ── init ──────────────────────────────────────────────────────────────── */

static GpuTexture *make_dummy(int cube);

static void vk_pcache_init(void);           /* (gpu_vk_draw.inc) */

#if defined(__ANDROID__)
/* A custom driver that loaded and then could not start the game (no Vulkan
 * 1.3 device, or no instance or device from it): the phone's own instead,
 * with the reason where the launcher shows it (android_driver.c). */
static int s_custom_driver;
#define CUSTOM_DRIVER_FALLBACK(why) do { \
        if (s_custom_driver) { \
            extern void gpu_vk_custom_driver_failed(const char *) __attribute__((weak)); \
            if (gpu_vk_custom_driver_failed) gpu_vk_custom_driver_failed(why); \
            if (s_dev) { vkDestroyDevice(s_dev, NULL); s_dev = VK_NULL_HANDLE; } \
            if (s_inst) { vkDestroyInstance(s_inst, NULL); s_inst = VK_NULL_HANDLE; } \
            s_phys = VK_NULL_HANDLE; \
            s_custom_driver = 0; \
            setenv("BUFFY_GPU_DRIVER", "", 1); \
            return vk_init(window); \
        } \
    } while (0)
#else
#define CUSTOM_DRIVER_FALLBACK(why) do { } while (0)
#endif

int vk_init(void *window)
{
    VkApplicationInfo app;
    VkInstanceCreateInfo ici;
    const char *iext[4];
    uint32_t niext = 0, n, i, best = 0;
    VkPhysicalDevice devs[16];
    int validate = getenv("BUFFY_VK_VALIDATE") != NULL;
    s_prof = getenv("BUFFY_VK_PROF") != NULL;
    const char *layers[1] = { "VK_LAYER_KHRONOS_validation" };
    (void)window;
    if (s_up)
        return 1;
#if defined(__ANDROID__)
    {
        /* A custom driver (the app's: Turnip and the like) when it gives a
         * loader; else the phone's (port/android/src/android_driver.c). */
        extern void *gpu_vk_open_loader(void) __attribute__((weak));
        void *h = gpu_vk_open_loader ? gpu_vk_open_loader() : NULL;
        PFN_vkGetInstanceProcAddr gipa = h ? (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr") : NULL;
        s_custom_driver = gipa != NULL;
        if (gipa)
            volkInitializeCustom(gipa);
        else if (volkInitialize() != VK_SUCCESS) {
            fprintf(stderr, "  [VK] no Vulkan loader\n");
            return 0;
        }
    }
#else
    if (volkInitialize() != VK_SUCCESS) {
        fprintf(stderr, "  [VK] no Vulkan loader\n");
        return 0;
    }
#endif
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Buffy the Vampire Slayer: Chaos Bleeds";
    app.pEngineName = "xboxrecomp";
    app.apiVersion = VK_API_VERSION_1_3;
    iext[niext++] = VK_KHR_SURFACE_EXTENSION_NAME;
#if defined(_WIN32)
    iext[niext++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif defined(__ANDROID__)
    iext[niext++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
#endif
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = niext;
    ici.ppEnabledExtensionNames = iext;
    if (validate) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = layers;
    }
    if (vkCreateInstance(&ici, NULL, &s_inst) != VK_SUCCESS && validate) {
        ici.enabledLayerCount = 0;                     /* (no validation layer installed) */
        if (vkCreateInstance(&ici, NULL, &s_inst) != VK_SUCCESS)
            s_inst = VK_NULL_HANDLE;
    }
    if (!s_inst)
        CUSTOM_DRIVER_FALLBACK("it can't start Vulkan here");
    if (!s_inst) {
        fprintf(stderr, "  [VK] instance creation failed\n");
        return 0;
    }
    volkLoadInstance(s_inst);

    /* the device: a discrete GPU first, Vulkan 1.3 */
    n = 16;
    vkEnumeratePhysicalDevices(s_inst, &n, devs);
    s_phys = VK_NULL_HANDLE;
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        uint32_t score;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3)
            continue;                             /* (Android: Vulkan 1.3 devices, Android 13 and later) */
        score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best) {
            best = score;
            s_phys = devs[i];
            snprintf(s_adapter, sizeof s_adapter, "%s", p.deviceName);   /* (the overlay names the renderer) */
            s_dyn_per_pass = p.vendorID == 0x5143;      /* Qualcomm (s_dyn) */
            s_ubo_align = p.limits.minUniformBufferOffsetAlignment ? p.limits.minUniformBufferOffsetAlignment : 256;
        }
    }
    if (getenv("BUFFY_VK_DYN_PER_PASS"))
        s_dyn_per_pass = getenv("BUFFY_VK_DYN_PER_PASS")[0] == '1';
    if (!s_phys)
        CUSTOM_DRIVER_FALLBACK("it has no Vulkan 1.3 GPU for this phone");
    if (!s_phys) {
        fprintf(stderr, "  [VK] no Vulkan 1.3 device\n");
        return 0;
    }
    {
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(s_phys, &nq, qf);
        for (i = 0; i < nq; i++)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                s_qfam = i;
                break;
            }
    }
    {
        /* D24S8 where the GPU has it (NVIDIA, Intel), else D32S8 (AMD) */
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s_phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) || getenv("BUFFY_VK_D32"))
            s_depth_fmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qi;
        VkDeviceCreateInfo dci;
        VkPhysicalDeviceFeatures2 f2;
        VkPhysicalDeviceVulkan13Features f13;
        VkPhysicalDeviceCustomBorderColorFeaturesEXT fcb;
        const char *dext[4];
        uint32_t ndext = 0, ne = 0;
        VkExtensionProperties *ex;
        memset(&qi, 0, sizeof qi);
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = s_qfam;
        qi.queueCount = 1;
        qi.pQueuePriorities = &prio;
        dext[ndext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        dext[ndext++] = VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME;
        vkEnumerateDeviceExtensionProperties(s_phys, NULL, &ne, NULL);
        ex = (VkExtensionProperties *)calloc(ne ? ne : 1, sizeof *ex);
        if (ex) {
            vkEnumerateDeviceExtensionProperties(s_phys, NULL, &ne, ex);
            for (i = 0; i < ne; i++)
                if (!strcmp(ex[i].extensionName, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME))
                    s_custom_border = 1;
            free(ex);
        }
        memset(&f2, 0, sizeof f2);
        memset(&f13, 0, sizeof f13);
        memset(&fcb, 0, sizeof fcb);
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        fcb.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT;
        f2.pNext = &f13;
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_TRUE;
        f2.features.depthClamp = VK_TRUE;
        {
            /* BC textures where the GPU has them; else (phones without them,
             * or BUFFY_VK_SOFT_BC=1 for testing) decoded on the CPU at upload */
            VkPhysicalDeviceFeatures have;
            vkGetPhysicalDeviceFeatures(s_phys, &have);
            s_soft_bc = !have.textureCompressionBC || (getenv("BUFFY_VK_SOFT_BC") && getenv("BUFFY_VK_SOFT_BC")[0] == '1');
            f2.features.textureCompressionBC = s_soft_bc ? VK_FALSE : VK_TRUE;
            if (s_soft_bc)
                fprintf(stderr, "  [VK] BC textures decoded on the CPU%s\n", have.textureCompressionBC ? " (BUFFY_VK_SOFT_BC)" : "");
        }
        if (s_custom_border) {
            dext[ndext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
            fcb.customBorderColors = VK_TRUE;
            fcb.customBorderColorWithoutFormat = VK_TRUE;
            f13.pNext = &fcb;
        }
        memset(&dci, 0, sizeof dci);
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pNext = &f2;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qi;
        dci.enabledExtensionCount = ndext;
        dci.ppEnabledExtensionNames = dext;
        if (vkCreateDevice(s_phys, &dci, NULL, &s_dev) != VK_SUCCESS) {
            s_dev = VK_NULL_HANDLE;
            CUSTOM_DRIVER_FALLBACK("it can't make a GPU device with what the game needs");
            fprintf(stderr, "  [VK] device creation failed\n");
            return 0;
        }
    }
    volkLoadDevice(s_dev);
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
    vk_pcache_init();                         /* pipelines built in earlier runs */
    {
        VmaVulkanFunctions vf;
        VmaAllocatorCreateInfo ai;
        memset(&vf, 0, sizeof vf);
        vf.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vf.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        memset(&ai, 0, sizeof ai);
        ai.vulkanApiVersion = VK_API_VERSION_1_3;
        ai.physicalDevice = s_phys;
        ai.device = s_dev;
        ai.instance = s_inst;
        ai.pVulkanFunctions = &vf;
        if (vmaCreateAllocator(&ai, &s_vma) != VK_SUCCESS) {
            fprintf(stderr, "  [VK] memory allocator failed\n");
            return 0;
        }
    }
    lock_init(&s_qlock);
    lock_init(&s_ulock);
    lock_init(&s_deadlock);
    s_render_tid = thread_id();
    {
        VkCommandPoolCreateInfo pi;
        VkCommandBufferAllocateInfo ai;
        VkCommandBuffer cbs[FRAMES];
        VkFenceCreateInfo fi;
        memset(&pi, 0, sizeof pi);
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = s_qfam;
        vkCreateCommandPool(s_dev, &pi, NULL, &s_pool);
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        vkCreateCommandPool(s_dev, &pi, NULL, &s_upool);
        memset(&ai, 0, sizeof ai);
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = s_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = FRAMES;
        vkAllocateCommandBuffers(s_dev, &ai, cbs);
        memset(&fi, 0, sizeof fi);
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        for (i = 0; i < FRAMES; i++) {
            VkBufferCreateInfo bi;
            VmaAllocationCreateInfo vai;
            VmaAllocationInfo inf;
            s_frames[i].cmd = cbs[i];
            vkCreateFence(s_dev, &fi, NULL, &s_frames[i].fence);
            memset(&bi, 0, sizeof bi);
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = RING_BYTES;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT
                     | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            memset(&vai, 0, sizeof vai);
            vai.usage = VMA_MEMORY_USAGE_AUTO;
            vai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            vai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;   /* (mapped and never flushed: phones have non-coherent types) */
            if (vmaCreateBuffer(s_vma, &bi, &vai, &s_frames[i].ring, &s_frames[i].ring_mem, &inf) != VK_SUCCESS)
                return 0;
            s_frames[i].ring_ptr = (uint8_t *)inf.pMappedData;
        }
    }
    {
        /* one push-descriptor set: b0 b1, t0..t7, s0..s3 */
        VkDescriptorSetLayoutBinding b[14];
        VkDescriptorSetLayoutCreateInfo li;
        VkPipelineLayoutCreateInfo pli;
        for (i = 0; i < 14; i++) {
            memset(&b[i], 0, sizeof b[i]);
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                : i < 10 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            b[i].stageFlags = i < 2 ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        memset(&li, 0, sizeof li);
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = 14;
        li.pBindings = b;
        vkCreateDescriptorSetLayout(s_dev, &li, NULL, &s_dsl);
        memset(&pli, 0, sizeof pli);
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &s_dsl;
        vkCreatePipelineLayout(s_dev, &pli, NULL, &s_pl);
    }
    s_up = 1;
    begin_frame();
    {
        /* what an unbound slot reads */
        static const uint8_t zero[256];
        GpuSamplerDesc sd;
        s_dummy2d = make_dummy(0);
        s_dummycube = make_dummy(1);
        memset(&sd, 0, sizeof sd);
        sd.u = sd.v = sd.w = GPU_ADDRESS_WRAP;
        s_dummysmp = gpu_sampler_create(&sd);
        s_dummyubo = gpu_buffer_create(256, GPU_BIND_CONSTANT, GPU_USAGE_IMMUTABLE, zero);
        s_dummyvb = gpu_buffer_create(256, GPU_BIND_VERTEX, GPU_USAGE_IMMUTABLE, zero);
    }
    fprintf(stderr, "  [VK] Vulkan on %s (depth %s%s)\n", s_adapter,
            s_depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", s_custom_border ? ", custom border colours" : "");
    return 1;
}

const char *vk_adapter_name(void) { return s_adapter; }

void vk_flush(void)
{
    /* (no-op: a submission happens at each present) */
}

#include "gpu_vk_res.inc"
#include "gpu_vk_draw.inc"
#include "gpu_vk_swap.inc"
