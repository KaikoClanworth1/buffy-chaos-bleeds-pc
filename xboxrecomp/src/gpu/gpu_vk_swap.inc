/*
 * gpu_vk_swap.inc - the Vulkan backend's swap chains (one per window).
 * Included by gpu_vulkan.c.
 *
 * An image is acquired at the first begin of a frame (the submission that
 * draws into it waits on the acquire), and presented after a submission that
 * signals the image's own semaphore. FIFO with vsync, IMMEDIATE (or MAILBOX)
 * without; a change of either, a resize or an out-of-date chain recreates it.
 */

static GpuSwapchain *s_prof_sc;                  /* (the window whose presents count frames) */

static void window_size(void *window, uint32_t *w, uint32_t *h)
{
#if defined(_WIN32)
    RECT r;
    GetClientRect((HWND)window, &r);
    *w = (uint32_t)(r.right - r.left);
    *h = (uint32_t)(r.bottom - r.top);
#elif defined(__ANDROID__)
    *w = (uint32_t)ANativeWindow_getWidth((ANativeWindow *)window);
    *h = (uint32_t)ANativeWindow_getHeight((ANativeWindow *)window);
#else
    (void)window;
    *w = *h = 0;
#endif
}

static void drop_images(GpuSwapchain *s)
{
    uint32_t i;
    for (i = 0; i < s->nimg; i++) {
        if (s->tex[i]) {
            s->tex[i]->refs = 1;
            vk_texture_release(s->tex[i]);
            s->tex[i] = NULL;
        }
        if (s->done[i]) {
            vkDestroySemaphore(s_dev, s->done[i], NULL);
            s->done[i] = VK_NULL_HANDLE;
        }
    }
    s->nimg = 0;
}

static VkPresentModeKHR pick_mode(GpuSwapchain *s, int vsync)
{
    VkPresentModeKHR modes[16];
    uint32_t n = 16, i;
    int immediate = 0, mailbox = 0;
    if (vsync)
        return VK_PRESENT_MODE_FIFO_KHR;
    vkGetPhysicalDeviceSurfacePresentModesKHR(s_phys, s->surf, &n, modes);
    for (i = 0; i < n; i++) {
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) immediate = 1;
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mailbox = 1;
    }
    return immediate ? VK_PRESENT_MODE_IMMEDIATE_KHR : mailbox ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
}

/* Android: an upright chain on a screen turned round is SUBOPTIMAL every
 * frame (the compositor turns it), and fine; size changes are caught by the
 * window's size, looked at every frame (vk_swapchain_begin). */
#if defined(__ANDROID__)
#define SUBOPTIMAL_IS_TURNED 1
#else
#define SUBOPTIMAL_IS_TURNED 0
#endif

/* (Re)creates the chain at the window's size. 0 when the window has no area
 * (minimised): the old chain, if any, stays. */
static int build_chain(GpuSwapchain *s)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[32];
    VkSwapchainCreateInfoKHR ci;
    VkSwapchainKHR old = s->sc, sc = VK_NULL_HANDLE;
    VkImage imgs[8];
    uint32_t nf = 32, i, w, h, n;

    window_size(s->window, &w, &h);
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_phys, s->surf, &caps);
    fprintf(stderr, "  [VK] swap chain: window %ux%u, surface %ux%u, transform %X\n", w, h,
            caps.currentExtent.width, caps.currentExtent.height, (unsigned)caps.currentTransform);
#if !defined(__ANDROID__)
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        w = caps.currentExtent.width;
        h = caps.currentExtent.height;
    }
#endif
    /* (Android: the surface's extent is the screen's own way up -- turned
     * round 90 or 270, it is the window's swapped. The chain is the window's
     * size, drawn upright, and the compositor turns it, as SVR11's
     * presenter does. Passing the screen's transform through without turning
     * the picture showed it squashed on a Fold held the other way.) */
    if (w == 0 || h == 0)
        return 0;
    if (w < caps.minImageExtent.width) w = caps.minImageExtent.width;
    if (h < caps.minImageExtent.height) h = caps.minImageExtent.height;
    if (caps.maxImageExtent.width && w > caps.maxImageExtent.width) w = caps.maxImageExtent.width;
    if (caps.maxImageExtent.height && h > caps.maxImageExtent.height) h = caps.maxImageExtent.height;

    /* the GPU is done with the old images before they go */
    end_rendering();
    flush_wait();
    vkDeviceWaitIdle(s_dev);

    if (!s->fmt) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s->surf, &nf, fmts);
        s->fmt = nf ? fmts[0].format : VK_FORMAT_B8G8R8A8_UNORM;
        for (i = 0; i < nf; i++)
            if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                s->fmt = VK_FORMAT_B8G8R8A8_UNORM;
                break;
            }
    }
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = s->surf;
    ci.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = s->fmt;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent.width = w;
    ci.imageExtent.height = h;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
#if defined(__ANDROID__)
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                    ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
#else
    ci.preTransform = caps.currentTransform;
#endif
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    ci.presentMode = pick_mode(s, s->want_vsync);
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    if (vkCreateSwapchainKHR(s_dev, &ci, NULL, &sc) != VK_SUCCESS) {
        fprintf(stderr, "  [VK] swap chain creation failed (%ux%u)\n", w, h);
        return 0;
    }
    drop_images(s);
    if (old)
        vkDestroySwapchainKHR(s_dev, old, NULL);
    s->sc = sc;
    s->w = w;
    s->h = h;
    s->vsync = s->want_vsync;
    s->acquired = 0;
    s->recreate = 0;
    n = 8;
    vkGetSwapchainImagesKHR(s_dev, sc, &n, NULL);
    if (n > 8) n = 8;
    vkGetSwapchainImagesKHR(s_dev, sc, &n, imgs);
    for (i = 0; i < n; i++) {
        GpuTexture *t = (GpuTexture *)calloc(1, sizeof *t);
        VkSemaphoreCreateInfo si;
        if (!t)
            break;
        t->desc.width = w;
        t->desc.height = h;
        t->desc.mips = 1;
        t->desc.format = GPU_FMT_BGRA8;
        t->desc.flags = GPU_TEX_TARGET;
        t->refs = 1;
        t->img = imgs[i];
        t->fmt = s->fmt;
        t->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        t->swapchain = 1;
        t->att = make_view(t, 1);
        s->tex[i] = t;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(s_dev, &si, NULL, &s->done[i]);
    }
    s->nimg = i;
    return 1;
}

GpuSwapchain *vk_swapchain_create(void *window)
{
    GpuSwapchain *s = (GpuSwapchain *)calloc(1, sizeof *s);
    VkBool32 ok = VK_FALSE;
    int i;
    if (!s)
        return NULL;
    s->window = window;
    s->want_vsync = 1;
#if defined(_WIN32)
    {
        VkWin32SurfaceCreateInfoKHR ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        ci.hinstance = GetModuleHandleA(NULL);
        ci.hwnd = (HWND)window;
        if (vkCreateWin32SurfaceKHR(s_inst, &ci, NULL, &s->surf) != VK_SUCCESS) {
            free(s);
            return NULL;
        }
    }
#elif defined(__ANDROID__)
    {
        /* window: the ANativeWindow * (from the activity's surface) */
        VkAndroidSurfaceCreateInfoKHR ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        ci.window = (ANativeWindow *)window;
        if (vkCreateAndroidSurfaceKHR(s_inst, &ci, NULL, &s->surf) != VK_SUCCESS) {
            free(s);
            return NULL;
        }
    }
#else
    free(s);
    return NULL;                                  /* (Linux runs the Windows build under Proton) */
#endif
    vkGetPhysicalDeviceSurfaceSupportKHR(s_phys, s_qfam, s->surf, &ok);
    if (!ok) {
        fprintf(stderr, "  [VK] the queue cannot present to this window\n");
        vkDestroySurfaceKHR(s_inst, s->surf, NULL);
        free(s);
        return NULL;
    }
    for (i = 0; i < FRAMES; i++) {
        VkSemaphoreCreateInfo si;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(s_dev, &si, NULL, &s->acq[i]);
    }
    if (!build_chain(s)) {
        /* (a minimised window: made at the first begin with an area) */
        s->recreate = 1;
    }
    return s;
}

void vk_swapchain_release(GpuSwapchain *s)
{
    int i;
    if (!s)
        return;
    end_rendering();
    flush_wait();
    vkDeviceWaitIdle(s_dev);
    drop_images(s);
    if (s->sc) vkDestroySwapchainKHR(s_dev, s->sc, NULL);
    for (i = 0; i < FRAMES; i++)
        if (s->acq[i]) vkDestroySemaphore(s_dev, s->acq[i], NULL);
    vkDestroySurfaceKHR(s_inst, s->surf, NULL);
    free(s);
}

GpuTexture *vk_swapchain_begin(GpuSwapchain *s, int resize)
{
    uint32_t w, h;
    VkResult r;
    if (!s)
        return NULL;
#if defined(__ANDROID__)
    /* A window that changes shape (unfolded, turned round -- Android 16
     * ignores the landscape lock on big screens) keeps its surface, and a
     * chain of the old size is just scaled to it, stretched: no OUT_OF_DATE
     * says so. The size is looked at every frame. */
    resize = 1;
#endif
    if (resize && !s->acquired) {
        window_size(s->window, &w, &h);
        if (w && h && (w != s->w || h != s->h))
            s->recreate = 1;
    }
    if ((s->recreate || !s->sc) && !s->acquired)
        build_chain(s);
    if (!s->sc || !s->nimg)
        return NULL;
    if (!s->acquired) {
        VkSemaphore sem = s->acq[s_fi];
        PROF_BEGIN(0);
        r = vkAcquireNextImageKHR(s_dev, s->sc, UINT64_MAX, sem, VK_NULL_HANDLE, &s->cur);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!build_chain(s))
                return NULL;
            r = vkAcquireNextImageKHR(s_dev, s->sc, UINT64_MAX, sem, VK_NULL_HANDLE, &s->cur);
        }
        PROF_END(0, PR_ACQUIRE);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
            return NULL;
        if (r == VK_SUBOPTIMAL_KHR && !SUBOPTIMAL_IS_TURNED)
            s->recreate = 1;
        if (s_nwaits < 8)
            s_waits[s_nwaits++] = sem;
        s->acquired = 1;
    }
    return s->tex[s->cur];
}

void vk_swapchain_present(GpuSwapchain *s, int vsync)
{
    VkPresentInfoKHR pi;
    VkResult r;
    GpuTexture *t;
    if (!s)
        return;
    s->want_vsync = vsync ? 1 : 0;
    if (!s->acquired && !vk_swapchain_begin(s, 0))
        return;
    t = s->tex[s->cur];
    end_rendering();
    image_barrier(cmd(), t, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    submit(s->done[s->cur]);
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &s->done[s->cur];
    pi.swapchainCount = 1;
    pi.pSwapchains = &s->sc;
    pi.pImageIndices = &s->cur;
    {
        PROF_BEGIN(0);
        lock(&s_qlock);
        r = vkQueuePresentKHR(s_queue, &pi);
        unlock(&s_qlock);
        PROF_END(0, PR_PRESENT);
    }
    vk_pcache_save(0);                        /* new pipelines to disk, every 10 s at most */
    if (s == s_prof_sc || !s_prof_sc) {
        s_prof_sc = s;
        prof_frame();
    }
    s->acquired = 0;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !SUBOPTIMAL_IS_TURNED) || s->want_vsync != s->vsync)
        s->recreate = 1;
}
