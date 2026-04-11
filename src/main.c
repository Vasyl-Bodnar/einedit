/* This Source Code Form is subject to the terms of the Mozilla Public
   License, v. 2.0. If a copy of the MPL was not distributed with this
   file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "arena.h"
#include "render.h"

#define LINE_SIZE (32 * 1024)
// NOTE: BLOCK_BITSIZE MUST be log2(BLOCK_SIZE)
#define BLOCK_SIZE (1024 * 1024)
#define BLOCK_BITSIZE 20

#define INIT_SCREEN_WIDTH 80
#define INIT_SCREEN_HEIGHT 25

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

typedef struct Editor {
    size_t file_size;
    FILE *file_ptr;
    Block *block_table;
    LineTable *line_table;
    uint32_t screen[INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT];
} Editor;

// NOTE: Assume the block is not already loaded
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
LineTable *load_line(Arena **arena, Editor *edit, size_t line_id) {
    LineTable *line_tab = edit->line_table;
    edit->line_table = alloc(arena, LineTable);
    edit->line_table->line_id = line_id;
    edit->line_table->next = line_tab;

    size_t line_idx = 0;
    for (size_t i = 0; i < edit->file_size / BLOCK_SIZE; i++) {
        Block *block = find_block(arena, edit, i);
        for (size_t j = 0; j < BLOCK_SIZE; j++) {
            if (block->ptr[j] == '\n') {
                edit->line_table->line[line_idx].block_id = i;
                edit->line_table->line[line_idx].idx = j;
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
            edit->line_table->line[line_idx].idx = j;
            edit->line_table->line[line_idx].live = 1;
            line_idx += 1;
            if (line_idx == LINE_SIZE) {
                return edit->line_table;
            }
        }
    }

    return edit->line_table;
}

Line find_line(Arena **arena, Editor *edit, size_t line) {
    if (line > edit->file_size) {
        return (Line){0};
    }

    if (!line) {
        return (Line){.block_id = 0, .idx = 0, .live = 1};
    }

    line -= 1;

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

    load_line(arena, edit, 0);
}

void close_editor(Editor *edit) { fclose(edit->file_ptr); }

// Map from a specific line in the file to screen
void map_to_chars(Arena **arena, Editor *edit, size_t line) {
    Line line_idx = find_line(arena, edit, line);
    if (!line_idx.live) {
        return;
    }

    size_t block_id = line_idx.block_id;
    size_t init_idx = line_idx.idx;
    char *block = find_block(arena, edit, block_id)->ptr;
    for (size_t i = 0, j = init_idx; i < INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT;
         j++) {
        if (j >= BLOCK_SIZE) {
            j = 0;
            block_id += 1;
            block = find_block(arena, edit, block_id)->ptr;
        }

        switch (block[j]) {
        case '\t':
            for (size_t tmp = i;
                 i < tmp + TAB_SIZE ||
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
}

void glfw_err_cb(int error, const char *desc) {
    fprintf(stderr, "GLFW Error: %s\n", desc);
}

int main(int argc, char *argv[]) {
    assert(argc < 2 && "Need a file path argument");

    // A bit much, we might not need all of it, or might even need more
    // For now, just keep it for current testing/developing
    Arena *arena = new_arena(1024 * 1024 * 4);
    Editor edit;
    Context ctx;

    if (!glfwInit()) {
        assert(!"GLFW did not init");
    }
    glfwSetErrorCallback(glfw_err_cb);

    init_ctx(&arena, &ctx, 1600, 800);

    setup_bufs(&arena, &ctx, "unscii-8.bin", INIT_SCREEN_WIDTH,
               INIT_SCREEN_HEIGHT);

    open_editor(&arena, &edit, "../buildlib.scm");
    map_to_chars(&arena, &edit, 0);

    while (!glfwWindowShouldClose(ctx.window)) {
        draw(&arena, &ctx, edit.screen, sizeof(edit.screen));

        if (ctx.frame_cnt % 5 == 0) {
            map_to_chars(&arena, &edit, ctx.frame_cnt % 200);
        }

        if (ctx.resize_flag) {
            resize(&ctx);
        }

        glfwWaitEvents();
    }

    // Wait for all work to finish for cleanup
    vkDeviceWaitIdle(ctx.dev);

    empty_ctx(&ctx);

    close_editor(&edit);

    glfwTerminate();

    delete_arena(arena);

    return 0;
}
