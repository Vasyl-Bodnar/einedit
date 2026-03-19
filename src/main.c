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
} Context;

VKAPI_ATTR VkBool32 VKAPI_CALL vk_err_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageSeverityFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *cb_data, void *user_data) {
    printf("(Validation Layer) Vulkan Error: %s\n", cb_data->pMessage);
    return VK_FALSE;
}

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
    assert(glfwInit() && "GLFW did not init");
    glfwSetErrorCallback(glfw_err_cb);

    // A bit much, we might not need all of it
    char space[1024 * 1024];
    Arena *arena = create_from(space, 1024 * 1024);

    Context ctx;

    uint32_t prop_cnt = 0;
    vkEnumerateInstanceLayerProperties(&prop_cnt, 0);
    VkLayerProperties *props = alloc_arr(arena, prop_cnt, VkLayerProperties);
    vkEnumerateInstanceLayerProperties(&prop_cnt, props);

    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    uint32_t layers_cnt = sizeof(layers) / sizeof(*layers);

    int prop_flag = 0;
    for (uint32_t i = 0; i < prop_cnt; i++) {
        if (memcmp(layers[0], props[i].layerName, strlen(layers[0]))) {
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
        if (memcmp(dbg_ext, exts[i], strlen(dbg_ext))) {
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

    assert(vkCreateInstance(&create_info, 0, &ctx.inst) == VK_SUCCESS &&
           "Could not create a Vulkan Instance");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    GLFWwindow *window = glfwCreateWindow(640, 480, "Deity", 0, 0);

    VkSurfaceKHR surface;
    assert(glfwCreateWindowSurface(ctx.inst, window, 0, &surface) ==
               VK_SUCCESS &&
           "Could not create a Vulkan Window Surface");

    /* Nothing to loop so far
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
    }
    */

    glfwTerminate();
    return 0;
}
