/* This Source Code Form is subject to the terms of the Mozilla Public
   License, v. 2.0. If a copy of the MPL was not distributed with this
   file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include <vulkan/vulkan_core.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <assert.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/stat.h>

#define DESC_LAY_CNT 1

PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessenger;
PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessenger;

enum destroy_type {
    DestroyGLFWWindow,
    DestroyInstance, // with instance
    DestroySurfaceKHR,
    DestroyDebugUtilsMessenger,
    DestroyDevice, // with device
    DestroyFence,
    DestroyPipeline,
    DestroyPipelineLayout,
    DestroyImage,
    FreeMemory,
    DestroySampler,
    DestroyDescriptorPool,
    DestroyDescriptorSetLayout,
    DestroyCommandPool,
    DestroySemaphore,
    DestroyRenderPass,
    DestroyFramebuffer,
    DestroyImageView,
    DestroySwapchainKHR,
};

// Use PointerToHandle for things that get updated like the swapchain
enum obj_type {
    Handle,
    PointerToHandle,
};

typedef struct Destroyer {
    size_t cap;
    size_t len;
    struct {
        enum destroy_type dtype;
        enum obj_type otype;
        void *obj;
    } destroys[];
} Destroyer;

// Current Vulkan code is based mostly on:
// rafael-abreu-english.blogspot.com - Vulkan series
typedef struct Context {
    GLFWwindow *window;

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
    VkRenderPass rendpass;
    VkFramebuffer *fb;
    VkCommandPool cmd_pool;
    VkCommandBuffer cmd_buf;
    VkPipelineLayout pipeline_lay;
    VkPipeline pipeline;

    VkDescriptorSetLayout desc_lay[DESC_LAY_CNT];
    VkDescriptorPool desc_pool;
    VkDescriptorSet desc;

    VkImage tex_img;
    VkDeviceMemory tex_img_mem;
    VkImageView tex_img_view;
    VkSampler tex_sampler;

    uint64_t frame_cnt;
    VkFence fence;
    VkSemaphore img_avail;
    VkSemaphore rend_done;
    VkDebugUtilsMessengerEXT dbg_msger;

    Destroyer *destroyer;
} Context;

enum font_type {
    FontEnd = 0x30000000,
    FontShort = 0x20000000,
    FontNormal = 0x00000000,
    FontWide = 0x10000000,
};

typedef struct FontSegment {
    uint32_t start; // font_type included in the highest nibble
    uint32_t end;
    uint32_t off;
} FontSegment;

// segments and data are directly mapped to the binary format
// ascii and type are extracted for the fast path
typedef struct FontLUT {
    enum font_type type; // ascii type
    char *ascii;
    size_t segment_cnt;
    FontSegment *segment;
    char *data;
} FontLUT;

typedef struct Arena {
    size_t size;
    size_t cur;
    size_t save; // for scratch arena
    char space[];
} Arena;

// No need to delete, you manage space yourself
Arena *create_from(void *space, size_t init_size) {
    Arena *arena = space;
    assert(arena && init_size >= sizeof(*arena) && "Could not create an Arena");
    memset(arena, 0, init_size);
    arena->size = init_size - sizeof(*arena);
    return arena;
}

// Requires correspanding delete
Arena *new_arena(size_t init_size) {
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

#define alloc(arena, type) alloc_align(arena, sizeof(type), alignof(type))
#define alloc_arr(arena, n, type)                                              \
    alloc_align(arena, sizeof(type) * n, alignof(type))

// To reset a number of allocations
void start_scratch(Arena *arena) { arena->save = arena->cur; }
void end_scratch(Arena *arena) { arena->cur = arena->save; }

void free_all(Arena *arena) { arena->cur = 0; }

void delete_arena(Arena *arena) { free(arena); }

VKAPI_ATTR VkBool32 VKAPI_CALL vk_dbg_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageSeverityFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *cb_data, void *user_data) {
    const char *ssevere;
    switch (severity) {
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
        ssevere = "Error";
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
        ssevere = "Warning";
        break;
    case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
        ssevere = "Noise";
        break;
    default:
        ssevere = "Unknown";
        break;
    }
    const char *stype;
    switch (type) {
    case VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT:
        stype = "General";
        break;
    case VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT:
        stype = "Validation";
        break;
    case VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT:
        stype = "Performance";
        break;
    default:
        stype = "Unknown";
        break;
    }
    fprintf(stderr, "Vulkan %s %s: %s\n", stype, ssevere, cb_data->pMessage);
    return VK_FALSE;
}

void glfw_err_cb(int error, const char *desc) {
    fprintf(stderr, "GLFW Error: %s\n", desc);
}

void alloc_destroyer(Arena **arena, Context *ctx, size_t cap) {
    ctx->destroyer = alloc_align(
        arena,
        sizeof(*ctx->destroyer) + sizeof(*ctx->destroyer->destroys) * cap,
        alignof(typeof(*ctx->destroyer)) &
            alignof(typeof(*ctx->destroyer->destroys)));
    ctx->destroyer->cap = cap;
}

void push_destroyer(Context *ctx, enum destroy_type dtype, enum obj_type otype,
                    void *obj) {
    assert(ctx->destroyer->len < ctx->destroyer->cap &&
           "Cannot push to a full Destroyer");

    ctx->destroyer->destroys[ctx->destroyer->len].dtype = dtype;
    ctx->destroyer->destroys[ctx->destroyer->len].otype = otype;
    ctx->destroyer->destroys[ctx->destroyer->len].obj = obj;

    ctx->destroyer->len += 1;
}

void pop_destroyer(Context *ctx) {
    assert(ctx->destroyer->len && "Cannot pop from an empty Destroyer");

    ctx->destroyer->len -= 1;

    enum destroy_type dtype =
        ctx->destroyer->destroys[ctx->destroyer->len].dtype;
    enum obj_type otype = ctx->destroyer->destroys[ctx->destroyer->len].otype;

    void *obj = ctx->destroyer->destroys[ctx->destroyer->len].obj;
    if (otype == PointerToHandle) {
        obj = *(void **)obj;
    }

    switch (dtype) {
    case DestroyGLFWWindow:
        glfwDestroyWindow(ctx->window);
        break;
    case DestroyInstance:
        vkDestroyInstance(ctx->inst, 0);
        break;
    case DestroySurfaceKHR:
        vkDestroySurfaceKHR(ctx->inst, obj, 0);
        break;
    case DestroyDebugUtilsMessenger:
        vkDestroyDebugUtilsMessenger(ctx->inst, obj, 0);
        break;
    case DestroyDevice:
        vkDestroyDevice(ctx->dev, 0);
        break;
    case DestroyFence:
        vkDestroyFence(ctx->dev, obj, 0);
        break;
    case DestroyPipeline:
        vkDestroyPipeline(ctx->dev, obj, 0);
        break;
    case DestroyPipelineLayout:
        vkDestroyPipelineLayout(ctx->dev, obj, 0);
        break;
    case DestroyImage:
        vkDestroyImage(ctx->dev, obj, 0);
        break;
    case FreeMemory:
        vkFreeMemory(ctx->dev, obj, 0);
        break;
    case DestroySampler:
        vkDestroySampler(ctx->dev, obj, 0);
        break;
    case DestroyDescriptorPool:
        vkDestroyDescriptorPool(ctx->dev, obj, 0);
        break;
    case DestroyDescriptorSetLayout:
        vkDestroyDescriptorSetLayout(ctx->dev, obj, 0);
        break;
    case DestroyCommandPool:
        vkDestroyCommandPool(ctx->dev, obj, 0);
        break;
    case DestroySemaphore:
        vkDestroySemaphore(ctx->dev, obj, 0);
        break;
    case DestroyRenderPass:
        vkDestroyRenderPass(ctx->dev, obj, 0);
        break;
    case DestroyFramebuffer:
        vkDestroyFramebuffer(ctx->dev, obj, 0);
        break;
    case DestroyImageView:
        vkDestroyImageView(ctx->dev, obj, 0);
        break;
    case DestroySwapchainKHR:
        vkDestroySwapchainKHR(ctx->dev, obj, 0);
        break;
    }
}

void empty_destroyer(Context *ctx) {
    while (ctx->destroyer->len) {
        pop_destroyer(ctx);
    }
}

size_t font_type_to_bytes(enum font_type font_type) {
    switch (font_type) {
    case FontEnd:
        return 0;
    case FontShort:
        return 8;
    case FontNormal:
        return 16;
    case FontWide:
        return 32;
    }
    return 0;
}

FontLUT load_font(Arena **arena, const char *path) {
    FontLUT font;
    size_t res;

    FILE *file = fopen(path, "rb");
    assert(file && "Could not open a Font File");

    struct stat st;
    res = fstat(fileno(file), &st);
    assert(!res && "Could not stat the Font File");

    char *bin = alloc_arr(arena, st.st_size, char);
    res = fread(bin, 1, st.st_size, file);
    assert(res == st.st_size && "Could not read the entire Font File");
    assert(!memcmp(bin, "HEXFONT0", 8) && "Could not recognize a Font File");

    bin += 8;
    font.segment = (void *)bin;

    size_t segments_count = 0;
    while (font.segment[segments_count].start != FontEnd) {
        segments_count += 1;
    }

    font.segment_cnt = segments_count;
    font.data = (void *)(font.segment + segments_count + 1);

    // Assume ascii is fully represented
    font.type = font.segment->start & 0xFF000000;
    font.ascii = font.data;

    return font;
}

VkShaderModule load_shader(Arena **arena, Context *ctx, const char *path) {
    VkShaderModule mod;
    [[maybe_unused]] VkBool32 res;

    FILE *file = fopen(path, "rb");
    assert(file && "Could not open a Shader File");

    struct stat st;
    fstat(fileno(file), &st);

    void *code = alloc_arr(arena, st.st_size, char);
    fread(code, 1, st.st_size, file);

    VkShaderModuleCreateInfo mod_info = {
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        0,
        0,
        st.st_size,
        (uint32_t *)code,

    };

    res = vkCreateShaderModule(ctx->dev, &mod_info, 0, &mod);
    assert(res == VK_SUCCESS && "Could not create a Shader Module");

    fclose(file);
    return mod;
}

void create_graphics_pipeline(Arena **arena, Context *ctx) {
    [[maybe_unused]] VkBool32 ret = 0;
    VkShaderModule vert_shader = load_shader(arena, ctx, "vert.spv");
    VkShaderModule frag_shader = load_shader(arena, ctx, "frag.spv");

    VkPipelineShaderStageCreateInfo shader_info[] = {
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            0,
            0,
            VK_SHADER_STAGE_VERTEX_BIT,
            vert_shader,
            "main",
            0,
        },
        {
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            0,
            0,
            VK_SHADER_STAGE_FRAGMENT_BIT,
            frag_shader,
            "main",
            0,
        }};
    uint32_t shader_info_cnt = sizeof(shader_info) / sizeof(*shader_info);

    // We do not create verteces yet
    VkPipelineVertexInputStateCreateInfo vert_input_info = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        0,
        0,
        0,
        0,
        0,
        0,
    };

    VkPipelineInputAssemblyStateCreateInfo input_asm_info = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        0,
        0,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        VK_FALSE,
    };

    VkPipelineDynamicStateCreateInfo dyn_st_info = {
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 0, 0,
    };

    VkViewport view_ports[] = {
        {0, 0, (float)ctx->extent.width, (float)ctx->extent.height, 0, 0}};
    uint32_t view_ports_cnt = sizeof(view_ports) / sizeof(*view_ports);

    VkRect2D scissors[] = {{{0, 0}, ctx->extent}};
    uint32_t scissors_cnt = sizeof(scissors) / sizeof(*scissors);

    VkPipelineViewportStateCreateInfo view_port_info = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        0,
        0,
        view_ports_cnt,
        view_ports,
        scissors_cnt,
        scissors};

    VkPipelineRasterizationStateCreateInfo raster_info = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        0,
        0,
        VK_FALSE,
        VK_FALSE,
        VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_BACK_BIT,
        VK_FRONT_FACE_CLOCKWISE,
        VK_FALSE,
        0,
        0,
        0,
        1.f};

    VkPipelineMultisampleStateCreateInfo multsample_info = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        0,
        0,
        VK_SAMPLE_COUNT_1_BIT,
        VK_FALSE,
        0,
        0,
        VK_FALSE,
        VK_FALSE,
    };

    VkFlags color_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendAttachmentState color_blends[] = {{
        VK_TRUE,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_OP_ADD,
        color_mask,
    }};
    uint32_t color_blends_cnt = sizeof(color_blends) / sizeof(*color_blends);

    VkPipelineColorBlendStateCreateInfo color_blend_info = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        0,
        0,
        VK_FALSE,
        VK_LOGIC_OP_CLEAR,
        color_blends_cnt,
        color_blends,
        {0, 0, 0, 0}};

    VkPipelineLayoutCreateInfo pipeline_lay_info = {
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        0,
        0,
        DESC_LAY_CNT,
        ctx->desc_lay,
        0,
        0};

    ret = vkCreatePipelineLayout(ctx->dev, &pipeline_lay_info, 0,
                                 &ctx->pipeline_lay);
    assert(ret == VK_SUCCESS && "Could not create the Pipeline Layout");
    push_destroyer(ctx, DestroyPipelineLayout, Handle, ctx->pipeline_lay);

    VkGraphicsPipelineCreateInfo pipeline_info = {
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        0,
        0,
        shader_info_cnt,
        shader_info,
        &vert_input_info,
        &input_asm_info,
        0,
        &view_port_info,
        &raster_info,
        &multsample_info,
        0,
        &color_blend_info,
        &dyn_st_info,
        ctx->pipeline_lay,
        ctx->rendpass,
        0, // subpass idx
        0,
        0};

    ret = vkCreateGraphicsPipelines(ctx->dev, VK_NULL_HANDLE, 1, &pipeline_info,
                                    0, &ctx->pipeline);
    assert(ret == VK_SUCCESS && "Could not create the Graphics Pipeline");
    push_destroyer(ctx, DestroyPipeline, Handle, ctx->pipeline);

    vkDestroyShaderModule(ctx->dev, vert_shader, 0);
    vkDestroyShaderModule(ctx->dev, frag_shader, 0);
}

uint32_t find_vkmem_type(Context *ctx, uint32_t filter,
                         VkMemoryPropertyFlags mem_flags) {
    VkPhysicalDeviceMemoryProperties props = {0};
    vkGetPhysicalDeviceMemoryProperties(ctx->phy_dev, &props);

    uint32_t mem_idx = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((filter & (1 << i)) &&
            ((mem_flags & props.memoryTypes[i].propertyFlags) == mem_flags)) {
            mem_idx = i;
            break;
        }
    }
    assert(mem_idx != UINT32_MAX && "Could not find a Vulkan Memory Type");

    return mem_idx;
}

VkBool32 begin_cmds_once(Context *ctx, VkCommandBuffer *cmd_buf) {
    [[maybe_unused]] VkBool32 vk_ret;
    VkCommandBufferAllocateInfo info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0,
        ctx->cmd_pool, // Need different pool
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vk_ret = vkAllocateCommandBuffers(ctx->dev, &info, cmd_buf);

    VkCommandBufferBeginInfo beg_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 0};
    vk_ret |= vkBeginCommandBuffer(*cmd_buf, &beg_info);

    return vk_ret;
}

void end_cmds_once(Context *ctx, VkCommandBuffer cmd_buf) {
    vkEndCommandBuffer(cmd_buf);
    VkSubmitInfo info = {
        VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cmd_buf, 0, 0};
    vkQueueSubmit(ctx->queue, 1, &info, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx->queue);
    vkFreeCommandBuffers(ctx->dev, ctx->cmd_pool, 1, &cmd_buf);
}

VkBool32 create_img_view(Context *ctx, VkImageView *img_view, VkImage img,
                         VkFormat format) {
    VkComponentMapping swizzle = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
    };

    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageViewCreateInfo view_info = {
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        0,
        0,
        img,
        VK_IMAGE_VIEW_TYPE_2D,
        format,
        swizzle,
        range,
    };

    return vkCreateImageView(ctx->dev, &view_info, 0, img_view);
}

VkBool32 create_rendpass(Arena **arena, Context *ctx) {
    VkAttachmentDescription color_atts[] = {{
        0,
        ctx->img_format,
        VK_SAMPLE_COUNT_1_BIT,
        VK_ATTACHMENT_LOAD_OP_CLEAR,
        VK_ATTACHMENT_STORE_OP_STORE,
        VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    }};
    uint32_t color_atts_cnt = sizeof(color_atts) / sizeof(*color_atts);

    VkAttachmentReference color_refs[] = {{
        0,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    }};
    uint32_t color_refs_cnt = sizeof(color_refs) / sizeof(*color_refs);

    VkSubpassDescription subpasses[] = {{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0,
                                         0, color_refs_cnt, color_refs, 0, 0, 0,
                                         0}};
    uint32_t subpasses_cnt = sizeof(subpasses) / sizeof(*subpasses);

    VkRenderPassCreateInfo rendpass_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        0,
        0,
        color_atts_cnt,
        color_atts,
        subpasses_cnt,
        subpasses,
        0,
        0};

    return vkCreateRenderPass(ctx->dev, &rendpass_info, 0, &ctx->rendpass);
}

void create_swapchain(Arena **arena, Context *ctx) {
    [[maybe_unused]] VkBool32 vk_ret;

    VkSurfaceCapabilitiesKHR surf_caps;
    vk_ret = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->phy_dev, ctx->surf,
                                                       &surf_caps);
    assert(vk_ret == VK_SUCCESS && "Could not find Surface Capabilities");

    ctx->img_format = VK_FORMAT_B8G8R8A8_SRGB;
    ctx->extent = surf_caps.currentExtent;
    ctx->img_cnt = surf_caps.minImageCount > 3 ? surf_caps.minImageCount : 3;

    if (ctx->extent.width == -1 && ctx->extent.height == -1) {
        glfwGetFramebufferSize(ctx->window, (int *)&ctx->extent.width,
                               (int *)&ctx->extent.height);
    }

    VkSwapchainCreateInfoKHR swapchain_info = {
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        0,
        0,
        ctx->surf,
        ctx->img_cnt,
        ctx->img_format,
        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        ctx->extent,
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

    vk_ret =
        vkCreateSwapchainKHR(ctx->dev, &swapchain_info, 0, &ctx->swapchain);
    assert(vk_ret == VK_SUCCESS && "Could not create a Swapchain");
    push_destroyer(ctx, DestroySwapchainKHR, PointerToHandle, &ctx->swapchain);

    ctx->img_cnt = 0;
    vk_ret =
        vkGetSwapchainImagesKHR(ctx->dev, ctx->swapchain, &ctx->img_cnt, 0);
    assert(vk_ret == VK_SUCCESS && "Could not get valid Images");
    ctx->img = alloc_arr(arena, ctx->img_cnt, typeof(*ctx->img));
    ctx->img_view = alloc_arr(arena, ctx->img_cnt, typeof(*ctx->img_view));
    vk_ret = vkGetSwapchainImagesKHR(ctx->dev, ctx->swapchain, &ctx->img_cnt,
                                     ctx->img);
    assert(vk_ret == VK_SUCCESS && "Could not get valid Images");

    for (uint32_t i = 0; i < ctx->img_cnt; i++) {
        vk_ret = create_img_view(ctx, ctx->img_view + i, ctx->img[i],
                                 ctx->img_format);
        assert(vk_ret == VK_SUCCESS && "Could not create an Image View");
        push_destroyer(ctx, DestroyImageView, PointerToHandle,
                       ctx->img_view + i);
    }
}

void update_swapchain(Context *ctx) {
    [[maybe_unused]] VkBool32 vk_ret;
    for (uint32_t i = 0; i < ctx->img_cnt; i++) {
        vkDestroyFramebuffer(ctx->dev, ctx->fb[i], 0);
        vkDestroyImageView(ctx->dev, ctx->img_view[i], 0);
    }
    vkDestroySwapchainKHR(ctx->dev, ctx->swapchain, 0);

    VkSurfaceCapabilitiesKHR surf_caps;
    vk_ret = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->phy_dev, ctx->surf,
                                                       &surf_caps);
    assert(vk_ret == VK_SUCCESS &&
           "Could not find updated Surface Capabilities");

    ctx->extent = surf_caps.currentExtent;
    if (ctx->extent.width == -1 && ctx->extent.height == -1) {
        glfwGetFramebufferSize(ctx->window, (int *)&ctx->extent.width,
                               (int *)&ctx->extent.height);
    }

    VkSwapchainCreateInfoKHR swapchain_info = {
        VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        0,
        0,
        ctx->surf,
        ctx->img_cnt,
        ctx->img_format,
        VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        ctx->extent,
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

    vk_ret =
        vkCreateSwapchainKHR(ctx->dev, &swapchain_info, 0, &ctx->swapchain);
    assert(vk_ret == VK_SUCCESS && "Could not update a Swapchain");
    vk_ret = vkGetSwapchainImagesKHR(ctx->dev, ctx->swapchain, &ctx->img_cnt,
                                     ctx->img);
    assert(vk_ret == VK_SUCCESS && "Could not get updated valid Images");

    for (uint32_t i = 0; i < ctx->img_cnt; i++) {
        vk_ret = create_img_view(ctx, ctx->img_view + i, ctx->img[i],
                                 ctx->img_format);
        assert(vk_ret == VK_SUCCESS && "Could not update an Image View");

        VkImageView img_views[] = {ctx->img_view[i]};
        uint32_t img_views_cnt = sizeof(img_views) / sizeof(*img_views);

        VkFramebufferCreateInfo fb_info = {
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            0,
            0,
            ctx->rendpass,
            img_views_cnt,
            img_views,
            ctx->extent.width,
            ctx->extent.height,
            1};

        vk_ret = vkCreateFramebuffer(ctx->dev, &fb_info, 0, ctx->fb + i);
        assert(vk_ret == VK_SUCCESS && "Could not update a Framebuffer");
    }
}

void create_desc(Context *ctx) {
    [[maybe_unused]] VkBool32 vk_ret = 0;
    VkDescriptorSetLayoutBinding desc_lay_bind = {
        0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
        VK_SHADER_STAGE_FRAGMENT_BIT, 0};

    VkDescriptorSetLayoutCreateInfo desc_lay_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1,
        &desc_lay_bind};

    vk_ret =
        vkCreateDescriptorSetLayout(ctx->dev, &desc_lay_info, 0, ctx->desc_lay);
    assert(vk_ret == VK_SUCCESS && "Could not create a Descriptor Layout");
    push_destroyer(ctx, DestroyDescriptorSetLayout, PointerToHandle,
                   ctx->desc_lay);

    VkDescriptorPoolSize desc_pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    uint32_t desc_pool_sizes_cnt =
        sizeof(desc_pool_sizes) / sizeof(*desc_pool_sizes);

    VkDescriptorPoolCreateInfo desc_pool_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        0,
        0,
        1,
        desc_pool_sizes_cnt,
        desc_pool_sizes};

    vk_ret =
        vkCreateDescriptorPool(ctx->dev, &desc_pool_info, 0, &ctx->desc_pool);
    assert(vk_ret == VK_SUCCESS && "Could not create a Descriptor Pool");
    push_destroyer(ctx, DestroyDescriptorPool, Handle, ctx->desc_pool);

    VkDescriptorSetAllocateInfo desc_info = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, ctx->desc_pool,
        DESC_LAY_CNT, ctx->desc_lay};
    vk_ret = vkAllocateDescriptorSets(ctx->dev, &desc_info, &ctx->desc);
    assert(vk_ret == VK_SUCCESS && "Could not allocate a Descriptor Set");
}

VkBool32 setup_dbg(Context *ctx) {
    vkCreateDebugUtilsMessenger =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            ctx->inst, "vkCreateDebugUtilsMessengerEXT");
    assert(vkCreateDebugUtilsMessenger && "Could not get Procedure Address for "
                                          "vkCreateDebugUtilsMessenger");

    vkDestroyDebugUtilsMessenger =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            ctx->inst, "vkDestroyDebugUtilsMessengerEXT");
    assert(vkDestroyDebugUtilsMessenger &&
           "Could not get Procedure Address for "
           "vkDestroyDebugUtilsMessenger");

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

    return vkCreateDebugUtilsMessenger(ctx->inst, &dbg_info, 0,
                                       &ctx->dbg_msger);
}

void init_ctx(Arena **arena, Context *ctx) {
    [[maybe_unused]] VkBool32 vk_ret = 0;
    [[maybe_unused]] int ret = 0;

    uint32_t prop_cnt = 0;
    vkEnumerateInstanceLayerProperties(&prop_cnt, 0);
    VkLayerProperties *props = alloc_arr(arena, prop_cnt, typeof(*props));
    vkEnumerateInstanceLayerProperties(&prop_cnt, props);

    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    uint32_t layers_cnt = sizeof(layers) / sizeof(*layers);

    [[maybe_unused]] int prop_flag = 0;
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
        // NOTE: glfw takes care of exts for us, so we can completely ignore
        // it and only copy the addresses to strings
        const char **tmp = alloc_arr(arena, exts_cnt + 1, typeof(*tmp));
        for (uint32_t i = 0; i < exts_cnt; i++) {
            tmp[i] = exts[i];
        }
        tmp[exts_cnt] = dbg_ext;
        exts_cnt += 1;
        exts = tmp;
    }

    VkApplicationInfo app_info = {VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                  0,
                                  "EinEdit",
                                  1,
                                  "EinEdit",
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

    vk_ret = vkCreateInstance(&create_info, 0, &ctx->inst);
    assert(vk_ret == VK_SUCCESS && "Could not create a Vulkan Instance");
    push_destroyer(ctx, DestroyInstance, Handle, ctx->inst);

    vk_ret = setup_dbg(ctx);
    assert(vk_ret == VK_SUCCESS && "Could not create a Debug Messenger");
    push_destroyer(ctx, DestroyDebugUtilsMessenger, Handle, ctx->dbg_msger);

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    ctx->window = glfwCreateWindow(640, 480, "EinEdit", 0, 0);
    assert(ctx->window && "not create a GLFW Window");
    push_destroyer(ctx, DestroyGLFWWindow, Handle, 0);

    vk_ret = glfwCreateWindowSurface(ctx->inst, ctx->window, 0, &ctx->surf);
    assert(vk_ret == VK_SUCCESS && "Could not create a Vulkan Window Surface");
    push_destroyer(ctx, DestroySurfaceKHR, Handle, ctx->surf);

    uint32_t devs_cnt = 0;
    vkEnumeratePhysicalDevices(ctx->inst, &devs_cnt, 0);
    VkPhysicalDevice *devs = alloc_arr(arena, devs_cnt, typeof(*devs));
    vkEnumeratePhysicalDevices(ctx->inst, &devs_cnt, devs);

    // NOTE: We don't care too much about what device we have for my
    // purposes Still might look into some heuristics
    ctx->phy_dev = devs[0];
    assert(ctx->phy_dev && "Could not find a Physical Device");

    uint32_t qfprops_cnt = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->phy_dev, &qfprops_cnt, 0);
    VkQueueFamilyProperties *qfprops =
        alloc_arr(arena, qfprops_cnt, typeof(*qfprops));
    vkGetPhysicalDeviceQueueFamilyProperties(ctx->phy_dev, &qfprops_cnt,
                                             qfprops);

    ctx->qf_idx = UINT32_MAX;
    for (size_t i = 0; i < qfprops_cnt; i++) {
        if (qfprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            ctx->qf_idx = i;
            break;
        }
    }
    assert(ctx->qf_idx != UINT32_MAX &&
           "Could not query Queue Family Properties for Graphics");

    ret = glfwGetPhysicalDevicePresentationSupport(ctx->inst, ctx->phy_dev,
                                                   ctx->qf_idx);
    assert(ret && "Could not query Queue Family Properties for Present");

    const float queue_priorites[] = {1.f};
    uint32_t queue_priorites_cnt =
        sizeof(queue_priorites) / sizeof(*queue_priorites);

    VkDeviceQueueCreateInfo queue_infos[] = {
        {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, ctx->qf_idx,
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

    vk_ret = vkCreateDevice(ctx->phy_dev, &dev_info, 0, &ctx->dev);
    assert(vk_ret == VK_SUCCESS && "Could not create a Logical Device");
    push_destroyer(ctx, DestroyDevice, Handle, ctx->dev);

    vkGetDeviceQueue(ctx->dev, ctx->qf_idx, 0, &ctx->queue);
    assert(ctx->queue && "Could not create a Queue");

    create_swapchain(arena, ctx);

    vk_ret = create_rendpass(arena, ctx);
    assert(vk_ret == VK_SUCCESS && "Could not create a Render Pass");
    push_destroyer(ctx, DestroyRenderPass, Handle, ctx->rendpass);

    ctx->fb = alloc_arr(arena, ctx->img_cnt, typeof(*ctx->fb));
    for (uint32_t i = 0; i < ctx->img_cnt; i++) {
        VkImageView img_views[] = {ctx->img_view[i]};
        uint32_t img_views_cnt = sizeof(img_views) / sizeof(*img_views);

        VkFramebufferCreateInfo fb_info = {
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            0,
            0,
            ctx->rendpass,
            img_views_cnt,
            img_views,
            ctx->extent.width,
            ctx->extent.height,
            1};

        vk_ret = vkCreateFramebuffer(ctx->dev, &fb_info, 0, ctx->fb + i);
        assert(vk_ret == VK_SUCCESS && "Could not create a Framebuffer");
        push_destroyer(ctx, DestroyFramebuffer, PointerToHandle, ctx->fb + i);
    }

    VkSemaphoreCreateInfo sem_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                      0, 0};

    vk_ret = vkCreateSemaphore(ctx->dev, &sem_info, 0, &ctx->img_avail);
    vk_ret |= vkCreateSemaphore(ctx->dev, &sem_info, 0, &ctx->rend_done);
    assert(vk_ret == VK_SUCCESS && "Could not create Semaphores");
    push_destroyer(ctx, DestroySemaphore, Handle, ctx->img_avail);
    push_destroyer(ctx, DestroySemaphore, Handle, ctx->rend_done);

    VkCommandPoolCreateInfo cmd_pool_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        0,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        ctx->qf_idx,
    };

    vk_ret = vkCreateCommandPool(ctx->dev, &cmd_pool_info, 0, &ctx->cmd_pool);
    assert(vk_ret == VK_SUCCESS && "Could not create a Command Pool");
    push_destroyer(ctx, DestroyCommandPool, Handle, ctx->cmd_pool);

    VkCommandBufferAllocateInfo cmd_buf_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        0,
        ctx->cmd_pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        1,
    };

    vk_ret = vkAllocateCommandBuffers(ctx->dev, &cmd_buf_info, &ctx->cmd_buf);
    assert(vk_ret == VK_SUCCESS && "Could not alloc a Command Buffer");

    create_desc(ctx);

    create_graphics_pipeline(arena, ctx);

    VkFenceCreateInfo fence_info = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        0,
        VK_FENCE_CREATE_SIGNALED_BIT,
    };

    vk_ret = vkCreateFence(ctx->dev, &fence_info, 0, &ctx->fence);
    assert(vk_ret == VK_SUCCESS && "Could not create a Fence");
    push_destroyer(ctx, DestroyFence, Handle, ctx->fence);

    ctx->frame_cnt = 0;
}

void destroy_ctx(Context *ctx) {
    // Must wait until GPU is free
    empty_destroyer(ctx);
}

void alloc_mem(Context *ctx, VkDeviceMemory *mem) {}

void copy_texture(Context *ctx, uint32_t *tex, uint32_t tex_size,
                  VkExtent3D extent) {
    [[maybe_unused]] VkBool32 vk_ret;
    VkBuffer staging_buf;
    VkDeviceMemory staging_buf_mem;

    VkBufferCreateInfo staging_buf_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                           0,
                                           0,
                                           tex_size,
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           VK_SHARING_MODE_EXCLUSIVE,
                                           0,
                                           0};

    vk_ret = vkCreateBuffer(ctx->dev, &staging_buf_info, 0, &staging_buf);
    assert(vk_ret == VK_SUCCESS && "Could not create Staging Buffer");

    VkMemoryRequirements req = {0};
    vkGetBufferMemoryRequirements(ctx->dev, staging_buf, &req);

    uint32_t mem_type_idx =
        find_vkmem_type(ctx, req.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkMemoryAllocateInfo alloc_info = {
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        0,
        req.size,
        mem_type_idx,
    };

    vk_ret = vkAllocateMemory(ctx->dev, &alloc_info, 0, &staging_buf_mem);
    assert(vk_ret == VK_SUCCESS && "Could not alloc Staging Buffer Memory");

    vk_ret = vkBindBufferMemory(ctx->dev, staging_buf, staging_buf_mem, 0);
    assert(vk_ret == VK_SUCCESS && "Could not bind Staging Buffer to Memory");

    void *data = 0;
    vk_ret = vkMapMemory(ctx->dev, staging_buf_mem, 0, tex_size, 0, &data);
    assert(vk_ret == VK_SUCCESS && "Could not map Staging Buffer Memory");
    memcpy(data, tex, tex_size);
    vkUnmapMemory(ctx->dev, staging_buf_mem);

    VkImageCreateInfo img_info = {
        VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        0,
        0,
        VK_IMAGE_TYPE_2D,
        VK_FORMAT_R8G8B8A8_SRGB,
        extent,
        1,
        1,
        VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0,
        0,
        VK_IMAGE_LAYOUT_UNDEFINED,
    };

    vk_ret = vkCreateImage(ctx->dev, &img_info, 0, &ctx->tex_img);
    assert(vk_ret == VK_SUCCESS && "Could not create an Image");
    push_destroyer(ctx, DestroyImage, Handle, ctx->tex_img);

    vkGetImageMemoryRequirements(ctx->dev, ctx->tex_img, &req);
    mem_type_idx = find_vkmem_type(ctx, req.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    alloc_info = (VkMemoryAllocateInfo){
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        0,
        req.size,
        mem_type_idx,
    };
    vk_ret = vkAllocateMemory(ctx->dev, &alloc_info, 0, &ctx->tex_img_mem);
    assert(vk_ret == VK_SUCCESS && "Could not alloc Texture Image Memory");
    push_destroyer(ctx, FreeMemory, Handle, ctx->tex_img_mem);

    vk_ret = vkBindImageMemory(ctx->dev, ctx->tex_img, ctx->tex_img_mem, 0);
    assert(vk_ret == VK_SUCCESS && "Could not bind Image to Memory");

    VkCommandBuffer cmd_buf;
    vk_ret = begin_cmds_once(ctx, &cmd_buf);
    assert(vk_ret == VK_SUCCESS && "Could not begin Command Buffer");
    VkImageSubresourceRange res_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkImageMemoryBarrier bars[] = {
        {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0,
         VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
         VK_QUEUE_FAMILY_IGNORED, ctx->tex_img, res_range}};
    uint32_t bars_cnt = sizeof(bars) / sizeof(*bars);

    vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0,
                         bars_cnt, bars);

    VkImageSubresourceLayers res_lay = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkBufferImageCopy img_copy = {0, 0, 0, res_lay, {0, 0, 0}, extent};

    vkCmdCopyBufferToImage(cmd_buf, staging_buf, ctx->tex_img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &img_copy);

    bars[0] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                     0,
                                     VK_ACCESS_TRANSFER_WRITE_BIT,
                                     VK_ACCESS_SHADER_READ_BIT,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     VK_QUEUE_FAMILY_IGNORED,
                                     VK_QUEUE_FAMILY_IGNORED,
                                     ctx->tex_img,
                                     res_range};

    vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, 0, 0, 0,
                         bars_cnt, bars);

    end_cmds_once(ctx, cmd_buf);

    vkDestroyBuffer(ctx->dev, staging_buf, 0);
    vkFreeMemory(ctx->dev, staging_buf_mem, 0);

    vk_ret = create_img_view(ctx, &ctx->tex_img_view, ctx->tex_img,
                             VK_FORMAT_R8G8B8A8_SRGB);
    assert(vk_ret == VK_SUCCESS && "Could not create an Image View");
    push_destroyer(ctx, DestroyImageView, Handle, ctx->tex_img_view);

    VkSamplerCreateInfo sampler_info = {
        VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        0,
        0,
        VK_FILTER_NEAREST,
        VK_FILTER_NEAREST,
        VK_SAMPLER_MIPMAP_MODE_LINEAR,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        0.f,
        VK_FALSE,
        16.f,
        VK_FALSE,
        VK_COMPARE_OP_ALWAYS,
        0.f,
        VK_LOD_CLAMP_NONE,
        VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        VK_FALSE,
    };

    vk_ret = vkCreateSampler(ctx->dev, &sampler_info, 0, &ctx->tex_sampler);
    assert(vk_ret == VK_SUCCESS && "Could not create the Sampler");
    push_destroyer(ctx, DestroySampler, Handle, ctx->tex_sampler);

    VkDescriptorImageInfo desc_info = {
        ctx->tex_sampler, ctx->tex_img_view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write_desc = {
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        0,
        ctx->desc,
        0,
        0,
        1,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        &desc_info,
        0,
        0};

    vkUpdateDescriptorSets(ctx->dev, 1, &write_desc, 0, 0);
}

void draw(Arena *arena, Context *ctx) {
    [[maybe_unused]] VkBool32 vk_ret;
    vkWaitForFences(ctx->dev, 1, &ctx->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx->dev, 1, &ctx->fence);

    uint32_t img_idx = UINT32_MAX;
    vk_ret = vkAcquireNextImageKHR(ctx->dev, ctx->swapchain, UINT64_MAX,
                                   ctx->img_avail, VK_NULL_HANDLE, &img_idx);
    switch (vk_ret) {
    case VK_ERROR_OUT_OF_DATE_KHR:
        vkDeviceWaitIdle(ctx->dev);
        update_swapchain(ctx);
        return;
    default:
        assert(vk_ret == VK_SUCCESS && "Could not acquire next Image");
        assert(img_idx != UINT32_MAX &&
               "Could not acquire proper next Image Index");
    }

    vk_ret = vkResetCommandBuffer(ctx->cmd_buf, 0);
    assert(vk_ret == VK_SUCCESS && "Could not reset Command Buffer");
    VkCommandBufferBeginInfo begin_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, 0, 0

    };

    vk_ret = vkBeginCommandBuffer(ctx->cmd_buf, &begin_info);
    assert(vk_ret == VK_SUCCESS && "Could not begin Command Buffer");

    VkOffset2D rend_off = {0, 0};
    VkRect2D rend = {rend_off, ctx->extent};

    // Simple variation of the background color
    VkClearValue clear_vals[] = {
        {{{0., 1., (float)(ctx->frame_cnt % 100) / 100, 1.}}}};
    uint32_t clear_vals_cnt = sizeof(clear_vals) / sizeof(*clear_vals);

    VkRenderPassBeginInfo rendpass_begin_info = {
        VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        0,
        ctx->rendpass,
        ctx->fb[img_idx],
        rend,
        clear_vals_cnt,
        clear_vals};

    vkCmdBeginRenderPass(ctx->cmd_buf, &rendpass_begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(ctx->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      ctx->pipeline);

    VkDescriptorSet descs[] = {ctx->desc};
    uint32_t descs_cnt = sizeof(descs) / sizeof(*descs);
    vkCmdBindDescriptorSets(ctx->cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ctx->pipeline_lay, 0, descs_cnt, descs, 0, 0);

    // TODO: Have vertices at runtime not hardcoded
    vkCmdDraw(ctx->cmd_buf, 6, 1, 0, 0);

    vkCmdEndRenderPass(ctx->cmd_buf);
    vkEndCommandBuffer(ctx->cmd_buf);

    VkCommandBuffer cmd_bufs[] = {ctx->cmd_buf};
    uint32_t cmd_bufs_cnt = sizeof(cmd_bufs) / sizeof(*cmd_bufs);

    VkSemaphore img_avails[] = {ctx->img_avail};
    uint32_t img_avails_cnt = sizeof(img_avails) / sizeof(*img_avails);

    VkSemaphore rend_dones[] = {ctx->rend_done};
    uint32_t rend_dones_cnt = sizeof(rend_dones) / sizeof(*rend_dones);

    VkPipelineStageFlags stage_flags[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

    VkSubmitInfo submit_info = {VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                0,
                                img_avails_cnt,
                                img_avails,
                                stage_flags,
                                cmd_bufs_cnt,
                                cmd_bufs,
                                rend_dones_cnt,
                                rend_dones

    };

    vk_ret = vkQueueSubmit(ctx->queue, 1, &submit_info, ctx->fence);
    assert(vk_ret == VK_SUCCESS && "Could not Submit in Queue");

    VkSwapchainKHR swapchains[] = {ctx->swapchain};
    uint32_t swapchains_cnt = sizeof(swapchains) / sizeof(*swapchains);

    VkPresentInfoKHR present_info = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                     0,
                                     rend_dones_cnt,
                                     rend_dones,
                                     swapchains_cnt,
                                     swapchains,
                                     &img_idx,
                                     0};

    vk_ret = vkQueuePresentKHR(ctx->queue, &present_info);
    switch (vk_ret) {
    case VK_ERROR_OUT_OF_DATE_KHR:
        vkDeviceWaitIdle(ctx->dev);
        update_swapchain(ctx);
        break;
    default:
        assert(vk_ret == VK_SUCCESS && "Could not Present in Queue");
    }

    ctx->frame_cnt += 1;
}

int main(void) {
    // A bit much, we might not need all of it, or might even need more
    // For now, just keep it for current testing/developing
    Arena *arena = new_arena(1024 * 1024);
    Context ctx;

    if (!glfwInit()) {
        assert(!"GLFW did not init");
    }
    glfwSetErrorCallback(glfw_err_cb);

    alloc_destroyer(&arena, &ctx, 1000);
    init_ctx(&arena, &ctx);

    FontLUT font = load_font(&arena, "unscii-8.bin");

    uint32_t *texture = alloc_arr(&arena, 8 * 8, uint32_t);
    VkDeviceSize texture_size = 8 * 8 * sizeof(*texture);

    size_t lett = font_type_to_bytes(font.type);
    for (size_t i = lett * 33, j = 0; j < lett; i++, j++) {
        for (size_t k = 0; k < 8; k++) {
            texture[k + j * 8] =
                (font.ascii[i] & (1 << (8 - k))) ? 0xFF000000 : 0x00000000;
        }
    }
    copy_texture(&ctx, texture, texture_size, (VkExtent3D){8, 8, 1});

    while (!glfwWindowShouldClose(ctx.window)) {
        draw(arena, &ctx);
        glfwWaitEvents();
    }

    // Wait for all work to finish
    vkDeviceWaitIdle(ctx.dev);

    empty_destroyer(&ctx);
    glfwTerminate();
    delete_arena(arena);
    return 0;
}
