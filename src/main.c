#include <vulkan/vulkan_core.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <assert.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Context {
    VkInstance inst;
    VkSurfaceKHR surf;
    VkPhysicalDevice phy_dev;
    VkDevice dev;
    uint32_t qf_idx;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    uint32_t img_cnt;
    VkImage *img;
    VkImageView *img_view;
    VkFormat img_format;
    VkExtent2D extent;
} Context;

VKAPI_ATTR VkBool32 VKAPI_CALL vk_dbg_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageSeverityFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *cb_data, void *user_data) {
    const char *ssevere[] = {
        [VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT] = "Error",
        [VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT] = "Warning",
        [VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT] = "Noise"};
    const char *stype[] = {
        [VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT] = "General",
        [VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT] = "Validation",
        [VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT] = "Performance"};
    fprintf(stderr, "Vulkan %s %s: %s\n", stype[type], ssevere[severity],
            cb_data->pMessage);
    return VK_FALSE;
}

// TODO: Consider setting up vulkan allocator with this
typedef struct Arena {
    size_t size;
    size_t cur;
    size_t save; // for scratch arena
    char space[];
} Arena;

// No need to delete, you manage space yourself
Arena *create_from(void *space, size_t init_size) {
    Arena *arena = space;
    if (init_size < 8) {
        memset(arena, 0, sizeof(*arena) + 8);
        arena->size = 8;
    } else {
        memset(arena, 0, sizeof(*arena) + init_size);
        arena->size = init_size;
    }
    return arena;
}

// Requires correspanding delete
Arena *new(size_t init_size) {
    void *space = malloc(init_size);
    return create_from(space, init_size);
}

void *alloc_align(Arena **arena, size_t size, size_t align) {
    assert(!(align & (align - 1)) && "Expected power of two alignment");
    Arena *local = *arena;
    uintptr_t ptr = (uintptr_t)local->space + local->cur + size;
    if (ptr & (align - 1)) {
        ptr += align - (ptr & (align - 1));
    }

    if (ptr - (uintptr_t)local->space > local->size) {
        return 0;
    }

    local->cur = ptr - (uintptr_t)local->space;
    return (void *)ptr;
}

#define alloc(arena, type) alloc_align(&arena, sizeof(type), alignof(type))
#define alloc_arr(arena, n, type)                                              \
    alloc_align(&arena, sizeof(type) * n, alignof(type))

// To reset a number of allocations
void start_scratch(Arena *arena) { arena->save = arena->cur; }
void end_scratch(Arena *arena) { arena->cur = arena->save; }

void free_all(Arena *arena) { arena->cur = 0; }

void delete(Arena *arena) { free(arena); }

void glfw_err_cb(int error, const char *desc) {
    fprintf(stderr, "GLFW Error: %s\n", desc);
}

// TODO: reorg
int main(void) {
    if (!glfwInit()) {
        assert(!"GLFW did not init");
    }
    glfwSetErrorCallback(glfw_err_cb);

    // A bit much, we might not need all of it
    // Just for current testing/developing
    char space[1024 * 1024];
    Arena *arena = create_from(space, 1024 * 1024);

    Context ctx;
    VkBool32 vk_ret;
    int ret;

    uint32_t prop_cnt = 0;
    vkEnumerateInstanceLayerProperties(&prop_cnt, 0);
    VkLayerProperties *props = alloc_arr(arena, prop_cnt, VkLayerProperties);
    vkEnumerateInstanceLayerProperties(&prop_cnt, props);

    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    uint32_t layers_cnt = sizeof(layers) / sizeof(*layers);

    int prop_flag = 0;
    for (uint32_t i = 0; i < prop_cnt; i++) {
        if (!memcmp(layers[0], props[i].layerName, strlen(layers[0]))) {
            prop_flag = 1;
            break;
        }
    }
    assert(prop_flag && "Could not find the validation layer");

    uint32_t exts_cnt = 0;
    const char **exts = glfwGetRequiredInstanceExtensions(&exts_cnt);

    const char *dbg_ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    int ext_flag = 1;
    for (uint32_t i = 0; i < exts_cnt; i++) {
        if (!memcmp(dbg_ext, exts[i], strlen(dbg_ext))) {
            ext_flag = 0;
            break;
        }
    }
    if (ext_flag) {
        // NOTE: glfw takes care of exts for us, so we can completely ignore it
        // and only copy the addresses to strings
        const char **tmp = alloc_arr(arena, exts_cnt + 1, const char *);
        for (uint32_t i = 0; i < exts_cnt; i++) {
            tmp[i] = exts[i];
        }
        tmp[exts_cnt] = dbg_ext;
        exts_cnt += 1;
        exts = tmp;
    }

    VkApplicationInfo app_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                  0,
                                  "Deity",
                                  1,
                                  "Deity",
                                  1,
                                  VK_API_VERSION_1_3};

    VkInstanceCreateInfo create_info = {
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        0,
        0,
        &app_info,
        layers_cnt,
        layers,
        exts_cnt,
        exts,
    };

    vk_ret = vkCreateInstance(&create_info, 0, &ctx.inst);
    assert(vk_ret == VK_SUCCESS && "Could not create a Vulkan Instance");

    VkDebugUtilsMessageSeverityFlagsEXT msg_severity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

    VkDebugUtilsMessageTypeFlagsEXT msg_type =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

    VkDebugUtilsMessengerCreateInfoEXT dbg_info = {
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        0,
        0,
        msg_severity,
        msg_type,
        vk_dbg_cb,
        0};

    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            ctx.inst, "vkCreateDebugUtilsMessengerEXT");
    assert(
        vkCreateDebugUtilsMessengerEXT &&
        "Could not get Procedure Address for vkCreateDebugUtilsMessengerEXT");

    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            ctx.inst, "vkDestroyDebugUtilsMessengerEXT");
    assert(
        vkDestroyDebugUtilsMessengerEXT &&
        "Could not get Procedure Address for vkDestroyDebugUtilsMessengerEXT");

    VkDebugUtilsMessengerEXT dbg_msger;
    vk_ret = vkCreateDebugUtilsMessengerEXT(ctx.inst, &dbg_info, 0, &dbg_msger);
    assert(vk_ret == VK_SUCCESS && "Could not create a Debug Messenger");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *window = glfwCreateWindow(640, 480, "Deity", 0, 0);
    assert(window && "Could not create a GLFW Window");

    vk_ret = glfwCreateWindowSurface(ctx.inst, window, 0, &ctx.surf);
    assert(vk_ret == VK_SUCCESS && "Could not create a Vulkan Window Surface");

    uint32_t devs_cnt = 0;
    vkEnumeratePhysicalDevices(ctx.inst, &devs_cnt, 0);
    VkPhysicalDevice *devs = alloc_arr(arena, devs_cnt, VkPhysicalDevice);
    vkEnumeratePhysicalDevices(ctx.inst, &devs_cnt, devs);

    // NOTE: We don't care too much about what device we have for my purposes
    // Still might look into some heuristics
    ctx.phy_dev = devs[0];
    assert(ctx.phy_dev && "Could not find a Physical Device");

    uint32_t qfprops_cnt = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.phy_dev, &qfprops_cnt, 0);
    VkQueueFamilyProperties *qfprops =
        alloc_arr(arena, qfprops_cnt, VkQueueFamilyProperties);
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.phy_dev, &qfprops_cnt,
                                             qfprops);

    ctx.qf_idx = UINT32_MAX;
    for (size_t i = 0; i < qfprops_cnt; i++) {
        if (qfprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            ctx.qf_idx = i;
            break;
        }
    }
    assert(ctx.qf_idx != UINT32_MAX &&
           "Could not query Queue Family Properties for Graphics");

    ret = glfwGetPhysicalDevicePresentationSupport(ctx.inst, ctx.phy_dev,
                                                   ctx.qf_idx);
    assert(ret && "Could not query Queue Family Properties for Present");

    const float queue_priorites[] = {1.f};
    uint32_t queue_priorites_cnt =
        sizeof(queue_priorites) / sizeof(*queue_priorites);

    VkDeviceQueueCreateInfo queue_infos[] = {
        {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, ctx.qf_idx,
         queue_priorites_cnt, queue_priorites}};
    uint32_t queue_infos_cnt = sizeof(queue_infos) / sizeof(*queue_infos);

    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t dev_exts_cnt = sizeof(dev_exts) / sizeof(*dev_exts);

    VkDeviceCreateInfo dev_info = {
        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        0,
        0,
        queue_infos_cnt,
        queue_infos,
        0,
        0,
        dev_exts_cnt,
        dev_exts,
        0,

    };

    vk_ret = vkCreateDevice(ctx.phy_dev, &dev_info, 0, &ctx.dev);
    assert(vk_ret == VK_SUCCESS && "Could not create a Logical Device");

    vkGetDeviceQueue(ctx.dev, ctx.qf_idx, 0, &ctx.queue);
    assert(ctx.queue && "Could not create a Queue");

    VkSurfaceCapabilitiesKHR surf_caps;
    vk_ret = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.phy_dev, ctx.surf,
                                                       &surf_caps);
    assert(vk_ret == VK_SUCCESS && "Could not find Surface Capabilities");

    ctx.img_format = VK_FORMAT_B8G8R8A8_SRGB;
    ctx.extent = surf_caps.currentExtent;
    // TODO: For now
    ctx.img_cnt = surf_caps.minImageCount > 3 ? surf_caps.minImageCount : 3;

    // TODO: Assuming this is what we want, have to change when we resize
    if (ctx.extent.width == -1 && ctx.extent.height == -1) {
        glfwGetFramebufferSize(window, (int *)&ctx.extent.width,
                               (int *)&ctx.extent.height);
    }

    VkSwapchainCreateInfoKHR swapchain_info = {
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        0,
        0,
        ctx.surf,
        ctx.img_cnt,
        ctx.img_format,
        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        ctx.extent,
        1,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0,
        0,
        surf_caps.currentTransform,
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_PRESENT_MODE_MAILBOX_KHR,
        VK_TRUE,
        0,
    };

    vk_ret = vkCreateSwapchainKHR(ctx.dev, &swapchain_info, 0, &ctx.swapchain);
    assert(vk_ret == VK_SUCCESS && "Could not create a Swapchain");

    /* Nothing to loop so far
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
    }
    */

    vkDestroySwapchainKHR(ctx.dev, ctx.swapchain, 0);
    vkDestroyDevice(ctx.dev, 0);
    vkDestroySurfaceKHR(ctx.inst, ctx.surf, 0);
    vkDestroyDebugUtilsMessengerEXT(ctx.inst, dbg_msger, 0);
    vkDestroyInstance(ctx.inst, 0);
    glfwTerminate();
    return 0;
}
