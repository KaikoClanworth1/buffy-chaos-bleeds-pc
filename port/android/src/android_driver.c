/* Custom GPU drivers on Android (Mesa Turnip and the like).
 *
 * The launcher keeps driver packages (a zip with meta.json and the driver's
 * .so) in the app's own storage, and GameActivity passes the chosen one's
 * path as BUFFY_GPU_DRIVER. When the GPU layer starts Vulkan it asks for a
 * loader here (gpu_vulkan.c, gpu_vk_open_loader): libadrenotools (BSD-2-
 * Clause, third_party/libadrenotools) opens the phone's libvulkan in a
 * namespace where it loads that driver instead of the phone's. Its hook
 * libraries are in the app's native library folder (BUFFY_NATIVE_LIB_DIR).
 * Any failure: the phone's own driver, and a note in the app's cache
 * (gpu_driver_failed.txt) that the launcher shows. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <adrenotools/driver.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

static void note_failure(const char *path, const char *why)
{
    const char *cache = getenv("BUFFY_CACHE_DIR");
    char p[1024];
    FILE *f;
    fprintf(stderr, "  [VK] custom driver %s: %s -- the phone's own driver\n", path, why);
    if (!cache)
        return;
    snprintf(p, sizeof p, "%s/gpu_driver_failed.txt", cache);
    if ((f = fopen(p, "w")) != NULL) {
        fprintf(f, "%s\n%s\n", path, why);
        fclose(f);
    }
}

/* Can the driver make a Vulkan 1.3 instance? (Some load and then fail there.) */
static VkResult probe(void *h)
{
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create;
    PFN_vkDestroyInstance destroy;
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r;
    if (!gipa || !(create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance")))
        return VK_ERROR_INITIALIZATION_FAILED;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    if ((r = create(&ci, NULL, &inst)) != VK_SUCCESS)
        return r;
    destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    if (destroy)
        destroy(inst, NULL);
    return VK_SUCCESS;
}

/* The driver loaded, then could not run the game (gpu_vulkan.c, vk_init):
 * the GPU layer goes back to the phone's own driver. */
void gpu_vk_custom_driver_failed(const char *why)
{
    const char *path = getenv("BUFFY_GPU_DRIVER");
    note_failure(path ? path : "?", why);
}

/* The Vulkan loader to use (a dlopen handle), or NULL for the phone's own. */
void *gpu_vk_open_loader(void)
{
    const char *path = getenv("BUFFY_GPU_DRIVER"), *hooks = getenv("BUFFY_NATIVE_LIB_DIR"),
               *tmp = getenv("BUFFY_CACHE_DIR"), *slash;
    char dir[1024], hookdir[1024];
    void *h;
    VkResult r;
    if (!path || !*path)
        return NULL;
    slash = strrchr(path, '/');
    if (!slash || !hooks || access(path, R_OK) != 0) {
        note_failure(path, "missing");
        return NULL;
    }
    snprintf(dir, sizeof dir, "%.*s/", (int)(slash - path), path);
    snprintf(hookdir, sizeof hookdir, "%s%s", hooks, hooks[strlen(hooks) - 1] == '/' ? "" : "/");
    h = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp, hookdir, dir, slash + 1, NULL, NULL);
    if (!h) {
        const char *e = dlerror();
        note_failure(path, e ? e : "didn't load");
        return NULL;
    }
    if ((r = probe(h)) != VK_SUCCESS) {
        char why[64];
        snprintf(why, sizeof why, "can't start Vulkan (VkResult %d)", (int)r);
        note_failure(path, why);              /* (left loaded: unloading a half-started driver isn't safe) */
        return NULL;
    }
    fprintf(stderr, "  [VK] custom driver: %s\n", path);
    return h;
}
