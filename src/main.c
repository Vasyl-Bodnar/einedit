/* This Source Code Form is subject to the terms of the Mozilla Public
   License, v. 2.0. If a copy of the MPL was not distributed with this
   file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "render.h"

void glfw_err_cb(int error, const char *desc) {
    fprintf(stderr, "GLFW Error: %s\n", desc);
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

    init_ctx(&arena, &ctx);

    setup_bufs(&arena, &ctx);

    uint32_t chars[INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT] = {0};

    for (uint32_t i = 0; i < sizeof(chars) / sizeof(*chars); i++) {
        chars[i] = i;
    }

    while (!glfwWindowShouldClose(ctx.window)) {
        vkWaitForFences(ctx.dev, 1, ctx.fence + ctx.frame_idx, VK_TRUE,
                        UINT64_MAX);

        memcpy(ctx.screen +
                   (ctx.screen_width * ctx.screen_height * ctx.frame_idx),
               chars, sizeof(chars));

        draw(&arena, &ctx);

        if (ctx.resize_flag) {
            resize(&ctx);
        }

        glfwWaitEvents();
    }

    // Wait for all work to finish for cleanup
    vkDeviceWaitIdle(ctx.dev);

    empty_ctx(&ctx);

    glfwTerminate();

    delete_arena(arena);

    return 0;
}
