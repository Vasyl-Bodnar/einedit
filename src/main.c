/* This Source Code Form is subject to the terms of the Mozilla Public
   License, v. 2.0. If a copy of the MPL was not distributed with this
   file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "arena.h"
#include "render.h"
#include <GLFW/glfw3.h>

// Default sizes and limits
#define DEFAULT_LINE_SIZE (32 * 1024)
#define DEFAULT_BLOCK_SIZE (1024 * 1024)
#define DEFAULT_MAX_BLOCK_CNT 1024

#define INIT_SCREEN_WIDTH 100
#define INIT_SCREEN_HEIGHT 30

#define TAB_SIZE 4

typedef struct Block {
    size_t block_id;
    struct Block *next;
    char ptr[]; // From editor->block_size
} Block;

// NOTE: Some space potentially available here at a limit to id/idx
// Or just allocate double more
typedef struct Line {
    uint64_t block_id : 32;
    uint64_t idx : 31;
    uint64_t live : 1;
} Line;

typedef struct LineTable {
    size_t line_id;
    struct LineTable *next;
    Line ptr[]; // From editor->line_size
} LineTable;

typedef struct Cursor {
    size_t row;
    size_t col;
} Cursor;

typedef struct Editor {
    int dirty;
    Cursor cursor;
    size_t max_row;

    size_t file_size;
    FILE *file_ptr;

    const size_t block_size;
    const size_t max_block_cnt;
    size_t block_cnt;
    Block *block_table;

    const size_t line_size;
    size_t line_cnt;
    LineTable *line_table;

    uint32_t screen[INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT];
} Editor;

// NOTE: Assumes the block is not already loaded
Block *load_block(Arena **arena, Editor *edit, size_t block_id) {
    [[maybe_unused]] size_t res;

    assert(edit->max_block_cnt > edit->block_cnt &&
           "TODO: Setup a bit of garbage collection for blocks");

    Block *block_tab = edit->block_table;
    edit->block_table = alloc_align(
        arena,
        sizeof(Block) + sizeof(*edit->block_table->ptr) * edit->block_size,
        alignof(Block));
    edit->block_table->block_id = block_id;
    edit->block_table->next = block_tab;
    edit->block_cnt += 1;

    res = fseek(edit->file_ptr, block_id * edit->block_size, SEEK_SET);
    assert(!res && "Could not seek a File");
    res = fread(edit->block_table->ptr, 1, edit->block_size, edit->file_ptr);
    assert((res == edit->block_size ||
            res == (edit->file_size - (block_id * edit->block_size))) &&
           "Could not read a File");

    return edit->block_table;
}

Block *find_block(Arena **arena, Editor *edit, size_t block_id) {
    if (block_id > edit->file_size / edit->block_size) {
        return 0;
    }

    Block *block_tab = edit->block_table;
    while (block_tab) {
        if (block_tab->block_id == block_id) {
            return block_tab;
        }
        block_tab = block_tab->next;
    }

    return load_block(arena, edit, block_id);
}

// Gets a line table from the block(s)
// TODO: Loading the all blocks up to this line_id, not ideal
// TODO: Currently this practically assumes there is one line table
LineTable *load_line(Arena **arena, Editor *edit, size_t line_id) {
    LineTable *line_tab = edit->line_table;
    edit->line_table = alloc_align(
        arena,
        sizeof(LineTable) + sizeof(*edit->line_table->ptr) * edit->line_size,
        alignof(LineTable));
    edit->line_table->line_id = line_id;
    edit->line_table->next = line_tab;

    // First line is assumed and does not have a physical \n
    edit->line_table->ptr[0].block_id = 0;
    edit->line_table->ptr[0].idx = 0;
    edit->line_table->ptr[0].live = 1;

    size_t line_idx = 1;
    for (size_t i = 0; i < edit->file_size / edit->block_size; i++) {
        Block *block = find_block(arena, edit, i);
        for (size_t j = 0; j < edit->block_size; j++) {
            if (block->ptr[j] == '\n') {
                edit->line_table->ptr[line_idx].block_id = i;
                edit->line_table->ptr[line_idx].idx = j + 1;
                edit->line_table->ptr[line_idx].live = 1;
                line_idx += 1;
                if (line_idx == edit->line_size) {
                    return edit->line_table;
                }
            }
        }
    }

    Block *block = find_block(arena, edit, edit->file_size / edit->block_size);
    for (size_t j = 0; j < edit->file_size % edit->block_size; j++) {
        if (block->ptr[j] == '\n') {
            edit->line_table->ptr[line_idx].block_id =
                edit->file_size / edit->block_size;
            edit->line_table->ptr[line_idx].idx = j + 1;
            edit->line_table->ptr[line_idx].live = 1;
            line_idx += 1;
            if (line_idx == edit->line_size) {
                return edit->line_table;
            }
        }
    }

    edit->max_row = line_id * edit->line_size + line_idx;

    return edit->line_table;
}

Line find_line(Arena **arena, Editor *edit, size_t line) {
    if (line >= edit->file_size) {
        return (Line){0};
    }

    if (!line) {
        return (Line){.block_id = 0, .idx = 0, .live = 1};
    }

    size_t target_id = line / edit->line_size;
    size_t target_idx = line % edit->line_size;
    LineTable *line_tab = edit->line_table;
    while (line_tab) {
        if (line_tab->line_id == target_id) {
            return line_tab->ptr[target_idx];
        }
        line_tab = line_tab->next;
    }

    return load_line(arena, edit, target_id)->ptr[target_idx];
}

void open_editor(Arena **arena, Editor *edit, const char *path) {
    [[maybe_unused]] size_t res;

    FILE *file = fopen(path, "r+");
    assert(file && "Could not open a File");

    struct stat st;
    res = fstat(fileno(file), &st);
    assert(!res && "Could not stat the File");

    edit->file_ptr = file;
    edit->file_size = st.st_size;
    edit->cursor = (Cursor){0};
    edit->dirty = 1;

    load_line(arena, edit, 0);
}

void close_editor(Editor *edit) { fclose(edit->file_ptr); }

// Convert a cursor position to a screen view
void update_screen(Arena **arena, Editor *edit) {
    if (!edit->dirty) {
        return;
    }

    Line line_idx;
    if (edit->cursor.row > (INIT_SCREEN_HEIGHT / 2) - 1) {
        line_idx =
            find_line(arena, edit, edit->cursor.row - (INIT_SCREEN_HEIGHT / 2));
    } else {
        line_idx = find_line(arena, edit, 0);
    }

    if (!line_idx.live) {
        return;
    }

    edit->dirty = 0;

    size_t block_id = line_idx.block_id;
    size_t init_idx = line_idx.idx;
    char *block = find_block(arena, edit, block_id)->ptr;
    for (size_t i = 0, j = init_idx; i < INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT;
         j++) {
        if (j * block_id > edit->file_size) {
            for (; i < INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT; i++) {
                edit->screen[i] = 0;
            }
            return;
        } else if (j >= edit->block_size) {
            j = 0;
            block_id += 1;
            Block *bl = find_block(arena, edit, block_id);
            if (!bl) {
                for (; i < INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT; i++) {
                    edit->screen[i] = 0;
                }
                return;
            }
            block = bl->ptr;
        }

        switch (block[j]) {
        case '\t':
            for (size_t tmp = i;
                 i < tmp + TAB_SIZE &&
                 i < tmp + (INIT_SCREEN_WIDTH - (tmp % INIT_SCREEN_WIDTH));
                 i++) {
                edit->screen[i] = 0;
            }
            break;
        case '\n':
            for (size_t tmp = i;
                 i < tmp + (INIT_SCREEN_WIDTH - (tmp % INIT_SCREEN_WIDTH));
                 i++) {
                edit->screen[i] = 0;
            }
            break;
        default:
            edit->screen[i++] = (uint32_t)block[j];
            break;
        }
    }

    const size_t half_screen = (INIT_SCREEN_HEIGHT / 2);
    size_t idx =
        ((edit->cursor.row > half_screen) ? half_screen : edit->cursor.row) *
            INIT_SCREEN_WIDTH +
        edit->cursor.col;

    // Full Block █ char
    // NOTE: These are indexed by their line in the hex instead of unicode.
    // My font format does have ranges but they are not supported yet
    edit->screen[idx] = 1415;
}

void glfw_err_cb(int error, const char *desc) {
    fprintf(stderr, "GLFW Error: %s\n", desc);
}

void glfw_scroll_cb(GLFWwindow *window, double xoffset, double yoffset) {
    Editor *edit = (Editor *)glfwGetWindowUserPointer(window);

    // Could work a bit more on this to make it smoother
    if (yoffset < 0) {
        if (edit->cursor.row < edit->max_row) {
            edit->cursor.row += 1;
            edit->dirty = 1;
        }
    }

    if (yoffset > 0) {
        if (edit->cursor.row) {
            edit->cursor.row -= 1;
            edit->dirty = 1;
        }
    }

    if (xoffset < 0) {
        if (edit->cursor.col < INIT_SCREEN_WIDTH - 1) {
            edit->cursor.col += 1;
            edit->dirty = 1;
        }
    }

    if (xoffset > 0) {
        if (edit->cursor.col) {
            edit->cursor.col -= 1;
            edit->dirty = 1;
        }
    }
}

void glfw_key_cb(GLFWwindow *window, int key, int scancode, int action,
                 int mods) {
    Editor *edit = (Editor *)glfwGetWindowUserPointer(window);

    if (key == GLFW_KEY_J && action != GLFW_RELEASE) {
        if (edit->cursor.row < edit->max_row) {
            edit->cursor.row += 1;
            edit->dirty = 1;
        }
    } else if (key == GLFW_KEY_K && action != GLFW_RELEASE) {
        if (edit->cursor.row) {
            edit->cursor.row -= 1;
            edit->dirty = 1;
        }
    } else if (key == GLFW_KEY_L && action != GLFW_RELEASE) {
        if (edit->cursor.col < INIT_SCREEN_WIDTH - 1) {
            edit->cursor.col += 1;
            edit->dirty = 1;
        }
    } else if (key == GLFW_KEY_H && action != GLFW_RELEASE) {
        if (edit->cursor.col) {
            edit->cursor.col -= 1;
            edit->dirty = 1;
        }
    }
}

int main(int argc, char *argv[]) {
    // TODO: Create a default file in memory
    char *file_path = argv[1];
    if (argc < 2) {
        fprintf(stdout, "Need a file to read in the arguments!\n");
        return -1;
    }

    Arena *arena = new_arena(1024 * 1024 * 4, 1024 * 1024 * 64);
    Editor edit = {
        .max_block_cnt = DEFAULT_MAX_BLOCK_CNT,
        .block_size = DEFAULT_BLOCK_SIZE,
        .line_size = DEFAULT_LINE_SIZE,
    };
    Context ctx = {0};

    if (!glfwInit()) {
        assert(!"GLFW did not init");
    }
    glfwSetErrorCallback(glfw_err_cb);

    init_ctx(&arena, &ctx, 1600, 800);

    glfwSetWindowUserPointer(ctx.window, &edit);

    glfwSetScrollCallback(ctx.window, glfw_scroll_cb);
    glfwSetKeyCallback(ctx.window, glfw_key_cb);

    setup_bufs(&arena, &ctx, "unscii-8.bin", INIT_SCREEN_WIDTH,
               INIT_SCREEN_HEIGHT);

    open_editor(&arena, &edit, file_path);
    update_screen(&arena, &edit);

    while (!glfwWindowShouldClose(ctx.window)) {
        draw(&arena, &ctx, edit.screen, sizeof(edit.screen));

        update_screen(&arena, &edit);

        if (ctx.resize_flag) {
            resize(&ctx);
        }

        glfwPollEvents();
    }

    // Wait for all work to finish for cleanup
    vkDeviceWaitIdle(ctx.dev);

    empty_ctx(&ctx);

    close_editor(&edit);

    glfwTerminate();

    delete_arena(arena);

    return 0;
}
