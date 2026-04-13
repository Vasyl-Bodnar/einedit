/* This Source Code Form is subject to the terms of the Mozilla Public
   License, v. 2.0. If a copy of the MPL was not distributed with this
   file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "arena.h"
#include "render.h"
#include <GLFW/glfw3.h>

#define LINE_SIZE (32 * 1024)
// NOTE: BLOCK_BITSIZE MUST be log2(BLOCK_SIZE) or more
#define BLOCK_SIZE (1024 * 1024)
#define BLOCK_BITSIZE 20

#define INIT_SCREEN_WIDTH 100
#define INIT_SCREEN_HEIGHT 30

#define TAB_SIZE 4

enum block_tag {
    Alive = 0,
    Dead,
};

typedef struct Block {
    enum block_tag tag;
    size_t block_id;
    struct Block *next;
    char ptr[BLOCK_SIZE];
} Block;

// NOTE: Some space available here
typedef struct Line {
    uint64_t block_id : 32;
    uint64_t idx : BLOCK_BITSIZE;
    uint64_t live : 1;
} Line;

typedef struct LineTable {
    size_t line_id;
    struct LineTable *next;
    Line line[LINE_SIZE];
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
    Block *block_table;
    LineTable *line_table;
    uint32_t screen[INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT];
} Editor;

// NOTE: Assumes the block is not already loaded
Block *load_block(Arena **arena, Editor *edit, size_t block_id) {
    [[maybe_unused]] size_t res;

    Block *block_tab = edit->block_table;
    edit->block_table = alloc(arena, Block);
    edit->block_table->block_id = block_id;
    edit->block_table->tag = Alive;
    edit->block_table->next = block_tab;

    res = fseek(edit->file_ptr, block_id * BLOCK_SIZE, SEEK_SET);
    assert(!res && "Could not seek a File");
    res = fread(edit->block_table->ptr, 1, BLOCK_SIZE, edit->file_ptr);
    assert((res == BLOCK_SIZE ||
            res == (edit->file_size - (block_id * BLOCK_SIZE))) &&
           "Could not read a File");

    return edit->block_table;
}

Block *find_block(Arena **arena, Editor *edit, size_t block_id) {
    if (block_id > edit->file_size / BLOCK_SIZE) {
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
    edit->line_table = alloc(arena, LineTable);
    edit->line_table->line_id = line_id;
    edit->line_table->next = line_tab;

    // First line is assumed and does not have a physical \n
    edit->line_table->line[0].block_id = 0;
    edit->line_table->line[0].idx = 0;
    edit->line_table->line[0].live = 1;

    size_t line_idx = 1;
    for (size_t i = 0; i < edit->file_size / BLOCK_SIZE; i++) {
        Block *block = find_block(arena, edit, i);
        for (size_t j = 0; j < BLOCK_SIZE; j++) {
            if (block->ptr[j] == '\n') {
                edit->line_table->line[line_idx].block_id = i;
                edit->line_table->line[line_idx].idx = j + 1;
                edit->line_table->line[line_idx].live = 1;
                line_idx += 1;
                if (line_idx == LINE_SIZE) {
                    return edit->line_table;
                }
            }
        }
    }

    Block *block = find_block(arena, edit, edit->file_size / BLOCK_SIZE);
    for (size_t j = 0; j < edit->file_size % BLOCK_SIZE; j++) {
        if (block->ptr[j] == '\n') {
            edit->line_table->line[line_idx].block_id =
                edit->file_size / BLOCK_SIZE;
            edit->line_table->line[line_idx].idx = j + 1;
            edit->line_table->line[line_idx].live = 1;
            line_idx += 1;
            if (line_idx == LINE_SIZE) {
                return edit->line_table;
            }
        }
    }

    edit->max_row = line_id * LINE_SIZE + line_idx;

    return edit->line_table;
}

Line find_line(Arena **arena, Editor *edit, size_t line) {
    if (line >= edit->file_size) {
        return (Line){0};
    }

    if (!line) {
        return (Line){.block_id = 0, .idx = 0, .live = 1};
    }

    size_t target_id = line / LINE_SIZE;
    size_t target_idx = line % LINE_SIZE;
    LineTable *line_tab = edit->line_table;
    while (line_tab) {
        if (line_tab->line_id == target_id) {
            return line_tab->line[target_idx];
        }
        line_tab = line_tab->next;
    }

    return load_line(arena, edit, target_id)->line[target_idx];
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

    printf("(cursor %zu:%zu) (line-idx %d:%d)\n", edit->cursor.row,
           edit->cursor.col, line_idx.block_id, line_idx.idx);

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
        } else if (j >= BLOCK_SIZE) {
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

// TODO: Proper key combo navigation
void glfw_key_cb(GLFWwindow *window, int key, int scancode, int action,
                 int mods) {
    Editor *edit = (Editor *)glfwGetWindowUserPointer(window);

    if (key == GLFW_KEY_J && action != GLFW_RELEASE) {
        if (edit->cursor.row < edit->max_row) {
            edit->cursor.row += 1;
            edit->dirty = 1;
        }
    }

    if (key == GLFW_KEY_K && action != GLFW_RELEASE) {
        if (edit->cursor.row) {
            edit->cursor.row -= 1;
            edit->dirty = 1;
        }
    }

    if (key == GLFW_KEY_L && action != GLFW_RELEASE) {
        if (edit->cursor.col < INIT_SCREEN_WIDTH - 1) {
            edit->cursor.col += 1;
            edit->dirty = 1;
        }
    }

    if (key == GLFW_KEY_H && action != GLFW_RELEASE) {
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
        printf("Need a file to read in the arguments!\n");
        return -1;
    }

    // A bit much, we might not need all of it, or might even need more
    // For now, just keep it for current testing/developing
    // TODO: Make more dynamic
    Arena *arena = new_arena(1024 * 1024 * 4);
    Editor edit = {0};
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
