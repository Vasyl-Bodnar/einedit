/* This Source Code Form is subject to the terms of the Mozilla Public
   License, v. 2.0. If a copy of the MPL was not distributed with this
   file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "arena.h"
#include "render.h"
#include <GLFW/glfw3.h>

// Default sizes and limits
#define DEFAULT_LINE_SIZE (4 * 1024)
#define DEFAULT_SMALL_BLOCK_SIZE (1 * 1024)
#define DEFAULT_BLOCK_SIZE (8 * 1024)
#define DEFAULT_MAX_BLOCK_CNT (2 * 1024)

#define CHILD_CNT 8

#define INIT_SCREEN_WIDTH 100
#define INIT_SCREEN_HEIGHT 30

#define TAB_SIZE 4

enum block_kind {
    Raw = 0,
    ModAdd,
    ModRemove,
};

typedef struct Block {
    enum block_kind kind;
    size_t size; // for mods, max is editor->block_size
    size_t idx;  // in the file, TODO: not the best idea
    struct Block *mods;
    char ptr[]; // From editor->block_size usually
} Block;

typedef struct LineInfo {
    size_t min;
    size_t max;
    Block *block;
} LineInfo;

typedef struct LineTree {
    struct LineInfo line;
    struct LineTree *children[CHILD_CNT];
} LineTree;

typedef struct Cursor {
    size_t row;
    size_t col;
} Cursor;

// Key combo is a state machine
enum key_state {
    KeyStateNone = 0,
    KeyStateStartInput,
    KeyStateInput,
    KeyStateCommand,
    KeyStateGo,
};

// For commands
enum cmd_state {
    CmdStateNone = 0,
    CmdStateSave,
};

typedef struct Editor {
    int dirty;
    Cursor cursor;
    enum key_state key_state;
    enum cmd_state cmd_state;

    size_t file_size;
    FILE *file_ptr;

    const size_t small_block_size;
    const size_t block_size;
    const size_t max_block_cnt;
    LineTree *tree;

    uint32_t screen[INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT];
} Editor;

// NOTE: Assumes the block is not already loaded
Block *load_block(Arena **arena, Editor *edit, size_t block_idx) {
    [[maybe_unused]] size_t res;

    Block *block = alloc_align(
        arena, sizeof(Block) + sizeof(*block->ptr) * edit->block_size,
        alignof(Block));
    block->size = edit->block_size;
    block->idx = block_idx;

    res = fseek(edit->file_ptr, block_idx * edit->block_size, SEEK_SET);
    assert(!res && "Could not seek a File");
    res = fread(block->ptr, 1, edit->block_size, edit->file_ptr);
    assert((res == edit->block_size ||
            res == (edit->file_size - (block_idx * edit->block_size))) &&
           "Could not read a File");
    if (res != edit->block_size) {
        block->size = res;
    }

    return block;
}

size_t get_block_line(Block *block, size_t line) {
    if (line == SIZE_MAX) {
        size_t max = 0;
        for (size_t i = 0; i < block->size; i++) {
            if (block->ptr[i] == '\n') {
                max += 1;
            }
        }
        return max;
    }

    if (!line) {
        return 0;
    }

    size_t cnt = 0;
    for (size_t i = 0; i < block->size; i++) {
        if (block->ptr[i] == '\n') {
            cnt += 1;
            if (cnt == line) {
                return i;
            }
        }
    }

    return 0;
}

LineInfo find_line(LineTree *tree, size_t line) {
    if (line >= tree->line.min && line <= tree->line.max) {
        if (tree->line.block) {
            return tree->line;
        }

        LineTree *child;
        for (size_t i = 0; i < CHILD_CNT; i++) {
            child = tree->children[i];
            if (child && child->line.block && line >= child->line.min &&
                line <= child->line.max) {
                return child->line;
            }
        }
    }

    LineInfo found;
    for (size_t i = 0; i < CHILD_CNT; i++) {
        if (!tree->children[i]) {
            return (LineInfo){0};
        }
        found = find_line(tree->children[i], line);
        if (found.block) {
            return found;
        }
    }

    return (LineInfo){0};
}

LineInfo load_line(Arena **arena, Editor *edit, size_t line) {
    if (!edit->tree) {
        edit->tree = alloc(arena, LineTree);
        edit->tree->line.block = load_block(arena, edit, 0);
        edit->tree->line.min = 0;
        edit->tree->line.max = get_block_line(edit->tree->line.block, SIZE_MAX);
    }

    // TODO: Redo
    LineTree *tree = edit->tree;
    LineTree *child;
    while (line > tree->line.max) {
        for (size_t i = 0; i < CHILD_CNT; i++) {
            if (!tree->children[i]) {
                tree->children[i] = alloc(arena, LineTree);
                child = tree->children[i];
                child->line.block = load_block(arena, edit, i);
                child->line.min = tree->line.max;
                child->line.max = get_block_line(child->line.block, SIZE_MAX);

                tree->line.max = child->line.max;

                if (child->line.max >= line) {
                    return child->line;
                }
            }
        }
    }

    return find_line(edit->tree, line);
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

/* TODO: Redo
void save_file(Editor *edit) {
    [[maybe_unused]] size_t res;

    Block *block = edit->block_table;
    while (block) {
        res = fseek(edit->file_ptr, block->id * edit->block_size, SEEK_SET);
        assert(!res && "Could not seek a File");
        res = fwrite(edit->block_table->ptr, 1, block->size, edit->file_ptr);
        assert((res == edit->block_size ||
                res == (edit->file_size - (block->id * edit->block_size))) &&
               "Could not write to a File");
        block = block->next;
    }
}
*/

// Convert a cursor position to a screen view
void update_screen(Arena **arena, Editor *edit) {
    if (!edit->dirty) {
        return;
    }

    size_t cur_line = (edit->cursor.row > (INIT_SCREEN_HEIGHT / 2) - 1)
                          ? edit->cursor.row - (INIT_SCREEN_HEIGHT / 2)
                          : 0;
    LineInfo line_info = load_line(arena, edit, cur_line);
    if (!line_info.block) {
        return;
    }

    edit->dirty = 0;

    Block *block = line_info.block;
    for (size_t i = 0, j = 0; i < INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT; j++) {
        if (j * block->idx > edit->file_size) {
            for (; i < INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT; i++) {
                edit->screen[i] = 0;
            }
            return;
        } else if (j >= edit->block_size) {
            j = 0;
            LineInfo info = load_line(arena, edit, cur_line);
            if (!info.block) {
                for (; i < INIT_SCREEN_WIDTH * INIT_SCREEN_HEIGHT; i++) {
                    edit->screen[i] = 0;
                }
                return;
            }
            block = info.block;
        }

        switch (block->ptr[j]) {
        case '\t':
            for (size_t tmp = i;
                 i < tmp + TAB_SIZE &&
                 i < tmp + (INIT_SCREEN_WIDTH - (tmp % INIT_SCREEN_WIDTH));
                 i++) {
                edit->screen[i] = 0;
            }
            break;
        case '\n':
            cur_line += 1;
            for (size_t tmp = i;
                 i < tmp + (INIT_SCREEN_WIDTH - (tmp % INIT_SCREEN_WIDTH));
                 i++) {
                edit->screen[i] = 0;
            }
            break;
        default:
            edit->screen[i++] = (uint32_t)block->ptr[j];
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
    Editor *edit = ((Editor **)glfwGetWindowUserPointer(window))[0];
    // Arena **arena = ((Arena ***)glfwGetWindowUserPointer(window))[1];

    // Could work a bit more on this to make it smoother
    if (yoffset < 0) {
        if (edit->cursor.row <= edit->tree->line.max) {
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

// TODO: This feels suboptimal
void glfw_char_cb(GLFWwindow *window, unsigned int code) {
    Editor *edit = ((Editor **)glfwGetWindowUserPointer(window))[0];
    Arena **arena = ((Arena ***)glfwGetWindowUserPointer(window))[1];

    if (edit->key_state == KeyStateStartInput) {
        edit->key_state = KeyStateInput;
        return;
    }

    else if (edit->key_state == KeyStateInput) {
        // TODO: Should be combined with above for faster access
        LineInfo line = load_line(arena, edit, edit->cursor.row);
        Block *block = line.block;

        if (!block) {
            // TODO: Possibly EOF, should create a new block here then
            return;
        }

        // TODO: Proper idx
        size_t nl = get_block_line(block, edit->cursor.row);
        size_t idx = nl + edit->cursor.col + 1;
        block->ptr[idx] = code;
        if (edit->cursor.col < INIT_SCREEN_WIDTH - 1) {
            edit->cursor.col += 1;
        } else {
            if (edit->cursor.row < edit->tree->line.max) {
                edit->cursor.row += 1;
            }
            edit->cursor.col = 0;
        }
        edit->dirty = 1;
    }
}

void glfw_key_cb(GLFWwindow *window, int key, int scancode, int action,
                 int mods) {
    Editor *edit = ((Editor **)glfwGetWindowUserPointer(window))[0];
    // Arena **arena = ((Arena ***)glfwGetWindowUserPointer(window))[1];

    switch (edit->key_state) {
    case KeyStateNone:
        switch (key) {
        case GLFW_KEY_J:
            if (action != GLFW_RELEASE &&
                edit->cursor.row < edit->tree->line.max) {
                edit->cursor.row += 1;
                edit->dirty = 1;
            }
            break;
        case GLFW_KEY_K:
            if (action != GLFW_RELEASE && edit->cursor.row) {
                edit->cursor.row -= 1;
                edit->dirty = 1;
            }
            break;
        case GLFW_KEY_L:
            if (action != GLFW_RELEASE &&
                edit->cursor.col < INIT_SCREEN_WIDTH - 1) {
                edit->cursor.col += 1;
                edit->dirty = 1;
            }
            break;
        case GLFW_KEY_H:
            if (action != GLFW_RELEASE && edit->cursor.col) {
                edit->cursor.col -= 1;
                edit->dirty = 1;
            }
            break;
        case GLFW_KEY_G:
            if (action != GLFW_RELEASE) {
                if (mods == GLFW_MOD_SHIFT) {
                    edit->cursor.row =
                        edit->tree->line.max ? edit->tree->line.max - 1 : 0;
                    edit->cursor.col = 0;
                    edit->dirty = 1;
                } else {
                    edit->key_state = KeyStateGo;
                }
            }
            break;
        case GLFW_KEY_I:
            if (action != GLFW_RELEASE) {
                edit->key_state = KeyStateStartInput;
            }
            break;
        case GLFW_KEY_SEMICOLON:
            if (action != GLFW_RELEASE && mods == GLFW_MOD_SHIFT) {
                edit->key_state = KeyStateCommand;
            }
            break;
        default:
            break;
        }
        break;
    case KeyStateCommand:
        switch (key) {
        case GLFW_KEY_W:
            if (action != GLFW_RELEASE) {
                edit->cmd_state = CmdStateSave;
            }
            break;
        case GLFW_KEY_ENTER:
            if (action != GLFW_RELEASE) {
                switch (edit->cmd_state) {
                case CmdStateNone:
                    edit->key_state = KeyStateNone;
                    break;
                case CmdStateSave:
                    // save_file(edit);
                    edit->key_state = KeyStateNone;
                    edit->cmd_state = CmdStateNone;
                    break;
                }
            }
            break;
        default:
            break;
        }
        break;
    case KeyStateGo:
        switch (key) {
        case GLFW_KEY_G:
            if (action != GLFW_RELEASE) {
                edit->cursor.row = 0;
                edit->cursor.col = 0;
                edit->dirty = 1;

                edit->key_state = KeyStateNone;
            }
            break;
        default:
            edit->key_state = KeyStateNone;
            break;
        }
        break;
    case KeyStateStartInput:
    case KeyStateInput:
        switch (key) {
        case GLFW_KEY_ESCAPE:
            if (action != GLFW_RELEASE) {
                edit->key_state = KeyStateNone;
            }
            break;
        default:
            break;
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

    Arena *arena = new_arena(1024 * 1024 * 4, 1024 * 1024 * 128);
    Editor edit = {
        .max_block_cnt = DEFAULT_MAX_BLOCK_CNT,
        .block_size = DEFAULT_BLOCK_SIZE,
        .small_block_size = DEFAULT_SMALL_BLOCK_SIZE,
    };
    Context ctx = {0};

    if (!glfwInit()) {
        assert(!"GLFW did not init");
    }
    glfwSetErrorCallback(glfw_err_cb);

    init_ctx(&arena, &ctx, 1600, 800);

    void *user[] = {&edit, &arena};
    glfwSetWindowUserPointer(ctx.window, user);

    glfwSetScrollCallback(ctx.window, glfw_scroll_cb);
    glfwSetKeyCallback(ctx.window, glfw_key_cb);
    glfwSetCharCallback(ctx.window, glfw_char_cb);

    setup_bufs(&arena, &ctx, "unscii-8.bin", INIT_SCREEN_WIDTH,
               INIT_SCREEN_HEIGHT);

    open_editor(&arena, &edit, file_path);
    update_screen(&arena, &edit);

    Arena *draw_arena = sublet(&arena, 1024); // Only for drawing process needs

    while (!glfwWindowShouldClose(ctx.window)) {
        draw(&draw_arena, &ctx, edit.screen, sizeof(edit.screen));
        free_all(draw_arena);

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
