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

// Current code is based on rafael-abreu-english.blogspot.com and vkguide.dev
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

    VkFence fence;
    VkSemaphore img_avail;
    VkSemaphore rend_done;
    VkDebugUtilsMessengerEXT dbg_msger;

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

const char *vertex_shader =
    "#version 450\n"
    "vec2 positions[3] ="
    "vec2[](vec2(0.0, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5)); "
    "void main() {gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);} ";

const char *fragment_shader =
    "#version 450\n"
    "layout(location=0) out vec4 out_color;"
    "void main() {out_color = vec4(1.0, 0.0, 0.0, 1.0);} ";

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

VkShaderModule load_shader(Arena *arena, Context *ctx, const char *path) {
    VkShaderModule mod;

    FILE *file = fopen(path, "rb");
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

    VkBool32 res = vkCreateShaderModule(ctx->dev, &mod_info, 0, &mod);
    assert(res == VK_SUCCESS && "Failed to create a Shader Module");

    fclose(file);
    return mod;
}

void create_graphics_pipeline(Arena *arena, Context *ctx) {
    VkBool32 ret = 0;
    VkShaderModule vert_shader = load_shader(arena, ctx, "src/shader/vert.spv");
    VkShaderModule frag_shader = load_shader(arena, ctx, "src/shader/frag.spv");

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
        VK_FALSE,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_OP_ADD,
        VK_BLEND_FACTOR_ZERO,
        VK_BLEND_FACTOR_ZERO,
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
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 0, 0, 0, 0};

    ret = vkCreatePipelineLayout(ctx->dev, &pipeline_lay_info, 0,
                                 &ctx->pipeline_lay);
    assert(ret == VK_SUCCESS && "Could not create the Pipeline Layout");

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

    vkDestroyShaderModule(ctx->dev, vert_shader, 0);
    vkDestroyShaderModule(ctx->dev, frag_shader, 0);
}

Context create_ctx(Arena *arena) {
    Context ctx;
    VkBool32 vk_ret;
    int ret;

    uint32_t prop_cnt = 0;
    vkEnumerateInstanceLayerProperties(&prop_cnt, 0);
    VkLayerProperties *props = alloc_arr(arena, prop_cnt, typeof(*props));
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

    vk_ret =
        vkCreateDebugUtilsMessengerEXT(ctx.inst, &dbg_info, 0, &ctx.dbg_msger);
    assert(vk_ret == VK_SUCCESS && "Could not create a Debug Messenger");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    ctx.window = glfwCreateWindow(640, 480, "Deity", 0, 0);
    assert(ctx.window && "not create a GLFW Window");

    vk_ret = glfwCreateWindowSurface(ctx.inst, ctx.window, 0, &ctx.surf);
    assert(vk_ret == VK_SUCCESS && "Could not create a Vulkan Window Surface");

    uint32_t devs_cnt = 0;
    vkEnumeratePhysicalDevices(ctx.inst, &devs_cnt, 0);
    VkPhysicalDevice *devs = alloc_arr(arena, devs_cnt, typeof(*devs));
    vkEnumeratePhysicalDevices(ctx.inst, &devs_cnt, devs);

    // NOTE: We don't care too much about what device we have for my purposes
    // Still might look into some heuristics
    ctx.phy_dev = devs[0];
    assert(ctx.phy_dev && "Could not find a Physical Device");

    uint32_t qfprops_cnt = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(ctx.phy_dev, &qfprops_cnt, 0);
    VkQueueFamilyProperties *qfprops =
        alloc_arr(arena, qfprops_cnt, typeof(*qfprops));
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
        glfwGetFramebufferSize(ctx.window, (int *)&ctx.extent.width,
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

    // TODO: Swapchain will need to be recreated, thus this should be separate
    vk_ret = vkCreateSwapchainKHR(ctx.dev, &swapchain_info, 0, &ctx.swapchain);
    assert(vk_ret == VK_SUCCESS && "Could not create a Swapchain");

    ctx.img_cnt = 0;
    vk_ret = vkGetSwapchainImagesKHR(ctx.dev, ctx.swapchain, &ctx.img_cnt, 0);
    assert(vk_ret == VK_SUCCESS && "Could not get valid Images");
    ctx.img = alloc_arr(arena, ctx.img_cnt, typeof(*ctx.img));
    ctx.img_view = alloc_arr(arena, ctx.img_cnt, typeof(*ctx.img_view));
    vk_ret =
        vkGetSwapchainImagesKHR(ctx.dev, ctx.swapchain, &ctx.img_cnt, ctx.img);
    assert(vk_ret == VK_SUCCESS && "Could not get valid Images");

    for (uint32_t i = 0; i < ctx.img_cnt; i++) {
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
            ctx.img[i],
            VK_IMAGE_VIEW_TYPE_2D,
            ctx.img_format,
            swizzle,
            range,
        };

        vk_ret = vkCreateImageView(ctx.dev, &view_info, 0, ctx.img_view + i);
        assert(vk_ret == VK_SUCCESS && "Could not create an Image View");
    }

    VkAttachmentDescription color_atts[] = {{
        0,
        ctx.img_format,
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

    vk_ret = vkCreateRenderPass(ctx.dev, &rendpass_info, 0, &ctx.rendpass);
    assert(vk_ret == VK_SUCCESS && "Could not create a Render Pass");

    ctx.fb = alloc_arr(arena, ctx.img_cnt, typeof(*ctx.fb));
    for (uint32_t i = 0; i < ctx.img_cnt; i++) {
        VkImageView img_views[] = {ctx.img_view[i]};
        uint32_t img_views_cnt = sizeof(img_views) / sizeof(*img_views);

        VkFramebufferCreateInfo fb_info = {
            VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            0,
            0,
            ctx.rendpass,
            img_views_cnt,
            img_views,
            ctx.extent.width,
            ctx.extent.height,
            1};

        vk_ret = vkCreateFramebuffer(ctx.dev, &fb_info, 0, ctx.fb + i);
        assert(vk_ret == VK_SUCCESS && "Could not create a Framebuffer");
    }

    VkSemaphoreCreateInfo sem_info = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                      0, 0};

    vk_ret = vkCreateSemaphore(ctx.dev, &sem_info, 0, &ctx.img_avail);
    vk_ret |= vkCreateSemaphore(ctx.dev, &sem_info, 0, &ctx.rend_done);
    assert(vk_ret == VK_SUCCESS && "Could not create Semaphores");

    VkCommandPoolCreateInfo cmd_pool_info = {
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        0,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        ctx.qf_idx,
    };

    vk_ret = vkCreateCommandPool(ctx.dev, &cmd_pool_info, 0, &ctx.cmd_pool);
    assert(vk_ret == VK_SUCCESS && "Could not create a Command Pool");

    VkCommandBufferAllocateInfo cmd_buf_info = {
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        0,
        ctx.cmd_pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        1,
    };

    vk_ret = vkAllocateCommandBuffers(ctx.dev, &cmd_buf_info, &ctx.cmd_buf);
    assert(vk_ret == VK_SUCCESS && "Could not alloc a Command Buffer");

    create_graphics_pipeline(arena, &ctx);

    VkFenceCreateInfo fence_info = {
        VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        0,
        VK_FENCE_CREATE_SIGNALED_BIT,
    };

    vk_ret = vkCreateFence(ctx.dev, &fence_info, 0, &ctx.fence);
    assert(vk_ret == VK_SUCCESS && "Could not create a Fence");

    return ctx;
}

void destroy_ctx(Context *ctx) {
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            ctx->inst, "vkDestroyDebugUtilsMessengerEXT");
    assert(
        vkDestroyDebugUtilsMessengerEXT &&
        "Could not get Procedure Address for vkDestroyDebugUtilsMessengerEXT");

    vkDestroyFence(ctx->dev, ctx->fence, 0);
    vkDestroyPipeline(ctx->dev, ctx->pipeline, 0);
    vkDestroyPipelineLayout(ctx->dev, ctx->pipeline_lay, 0);
    vkDestroyCommandPool(ctx->dev, ctx->cmd_pool, 0);
    vkDestroySemaphore(ctx->dev, ctx->rend_done, 0);
    vkDestroySemaphore(ctx->dev, ctx->img_avail, 0);
    for (uint32_t i = 0; i < ctx->img_cnt; i++) {
        vkDestroyFramebuffer(ctx->dev, ctx->fb[i], 0);
    }
    vkDestroyRenderPass(ctx->dev, ctx->rendpass, 0);
    for (uint32_t i = 0; i < ctx->img_cnt; i++) {
        vkDestroyImageView(ctx->dev, ctx->img_view[i], 0);
    }
    vkDestroySwapchainKHR(ctx->dev, ctx->swapchain, 0);
    vkDestroyDevice(ctx->dev, 0);
    vkDestroySurfaceKHR(ctx->inst, ctx->surf, 0);
    vkDestroyDebugUtilsMessengerEXT(ctx->inst, ctx->dbg_msger, 0);
    vkDestroyInstance(ctx->inst, 0);
}

int main(void) {
    // A bit much, we might not need all of it, or might even need more
    // For now, just keep it for current testing/developing
    char space[1024 * 1024];
    Arena *arena = create_from(space, 1024 * 1024);

    if (!glfwInit()) {
        assert(!"GLFW did not init");
    }
    glfwSetErrorCallback(glfw_err_cb);

    Context ctx = create_ctx(arena);

    /*
    while (!glfwWindowShouldClose(ctx.window)) {
        glfwPollEvents();
    }
    */

    destroy_ctx(&ctx);
    glfwTerminate();
    return 0;
}
