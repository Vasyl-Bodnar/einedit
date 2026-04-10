/* This Source Code Form is subject to the terms of the Mozilla Public
   License, v. 2.0. If a copy of the MPL was not distributed with this
   file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef RENDER_H_
#define RENDER_H_

#include <vulkan/vulkan_core.h>
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/stat.h>

#include "arena.h"

#define MAX_FRAME_NUM 2
#define DESTROYER_LIMIT 1000

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
    DestroyBuffer,
    FreeMemory,
    UnmapMemory,
    DestroySampler,
    DestroyDescriptorPool,
    DestroyDescriptorSetLayout,
    DestroyCommandPool,
    DestroySemaphore,
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
// Might want to align the 32bit numbers together
typedef struct Context {
    GLFWwindow *window;

    VkInstance inst;
    VkSurfaceKHR surf;
    VkPhysicalDevice phy_dev;
    VkDevice dev;

    uint32_t qf_idx;
    VkQueue queue;
    VkPresentModeKHR present_mode;
    VkSwapchainKHR swapchain;
    VkSwapchainKHR old_swapchain;
    uint32_t img_cnt;
    VkImage *img;          // img_cnt
    VkImageView *img_view; // img_cnt
    VkFormat img_format;
    VkExtent2D extent;
    VkCommandPool cmd_pool;
    VkCommandBuffer cmd_buf[MAX_FRAME_NUM];
    VkPipelineLayout pipeline_lay;
    VkPipeline pipeline[2]; // For different specialized constants

    VkDescriptorSetLayout desc_lay[MAX_FRAME_NUM];
    VkDescriptorPool desc_pool;
    VkDescriptorSet desc[MAX_FRAME_NUM];

    VkBuffer staging_buf;
    VkDeviceMemory staging_mem;

    VkBuffer storage_buf;
    VkDeviceMemory storage_mem;

    // We keep duplicate screen buffers so CPU can write
    // while GPU is reading the previous state
    uint32_t screen_width; // In tiles, not pixels
    uint32_t screen_height;
    uint32_t font_width; // standard size of the font
    uint32_t font_height;
    uint32_t *screen;
    VkBuffer screen_buf;
    VkDeviceMemory screen_mem;

    VkImage storage_img;
    VkImageView storage_img_view;
    VkDeviceMemory storage_img_mem;

    uint32_t frame_idx;
    uint64_t frame_cnt;
    VkFence fence[MAX_FRAME_NUM];
    VkSemaphore img_avail[MAX_FRAME_NUM];
    VkSemaphore *rend_done; // img_cnt

    bool resize_flag;
    VkDebugUtilsMessengerEXT dbg_msger;
    Destroyer *destroyer;
} Context;

enum font_type {
    FontMASK = 0x0FFFFFFF,
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
    enum font_type type; // ascii type (excluding null)
    uint32_t segment_cnt;
    size_t code_cnt;
    FontSegment *segment;
    char *data;
} FontLUT;

void init_ctx(Arena **arena, Context *ctx, uint32_t window_width,
              uint32_t window_height);
void empty_ctx(Context *ctx);

void setup_bufs(Arena **arena, Context *ctx, const char *font_path,
                uint32_t width, uint32_t height);

void draw(Arena **arena, Context *ctx, uint32_t *chars, uint32_t chars_len);
void resize(Context *ctx);

#endif // RENDER_H_
