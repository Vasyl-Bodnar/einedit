/* This Source Code Form is subject to the terms of the Mozilla Public
   License, v. 2.0. If a copy of the MPL was not distributed with this
   file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "arena.h"
#include "render.h"

#define INIT_SCREEN_WIDTH 80
#define INIT_SCREEN_HEIGHT 25

void glfw_err_cb(int error, const char *desc) {
    fprintf(stderr, "GLFW Error: %s\n", desc);
}

typedef struct Editor {
    char *file_ptr;
    size_t file_size;
    uint32_t chars[INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT];
} Editor;

void load_file(Arena **arena, Editor *edit, const char *path) {
    [[maybe_unused]] size_t res;

    FILE *file = fopen(path, "r+");
    assert(file && "Could not open a File");

    struct stat st;
    res = fstat(fileno(file), &st);
    assert(!res && "Could not stat the File");

    char *ptr = alloc_arr(arena, st.st_size, char);
    res = fread(ptr, 1, st.st_size, file);
    assert(res == st.st_size && "Could not read the entire File");

    edit->file_ptr = ptr;
    edit->file_size = st.st_size;
}

void map_to_chars(Editor *edit, size_t idx) {
    assert(idx < edit->file_size && "Could not index that far into the File");
    for (size_t i = 0, j = idx; i < INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT;
         j++) {
        switch (edit->file_ptr[j]) {
        case '\n':
            for (size_t tmp = i;
                 i < tmp + (INIT_SCREEN_WIDTH - (tmp % INIT_SCREEN_WIDTH));
                 i++) {
                edit->chars[i] = 0;
            }
            break;
        default:
            edit->chars[i++] = (uint32_t)edit->file_ptr[j];
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    assert(argc < 2 && "Need a file path argument");

    // A bit much, we might not need all of it, or might even need more
    // For now, just keep it for current testing/developing
    Arena *arena = new_arena(1024 * 1024);
    Editor edit;
    Context ctx;

    if (!glfwInit()) {
        assert(!"GLFW did not init");
    }
    glfwSetErrorCallback(glfw_err_cb);

    init_ctx(&arena, &ctx, 1600, 800);

    setup_bufs(&arena, &ctx, "unscii-8.bin", INIT_SCREEN_WIDTH,
               INIT_SCREEN_HEIGHT);

    load_file(&arena, &edit, "../build.scm");
    map_to_chars(&edit, 0);

    while (!glfwWindowShouldClose(ctx.window)) {
        draw(&arena, &ctx, edit.chars, sizeof(edit.chars));

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
