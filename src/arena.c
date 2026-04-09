#include "arena.h"
#include <stdio.h>

Arena *create_from(void *space, size_t init_size) {
    Arena *arena = space;
    assert(arena && init_size >= sizeof(*arena) && "Could not create an Arena");
    memset(arena, 0, init_size);
    arena->size = init_size - sizeof(*arena);
    return arena;
}

Arena *new_arena(size_t init_size) {
    void *space = malloc(init_size);
    return create_from(space, init_size);
}

void *alloc_align(Arena **arena, size_t size, size_t align) {
    assert(align && !(align & (align - 1)) &&
           "Expected power of two alignment");
    Arena *local = *arena;
    uintptr_t ptr = (uintptr_t)local->space + local->cur;
    align = (align - (ptr & (align - 1))) & (align - 1);
    ptr += align;
    local->old = local->cur + align;
    local->cur += size + align;

    if (local->cur > local->size) {
        return 0;
    }

    return (void *)ptr;
}

void *realloc_align(Arena **arena, size_t size, size_t align, void *oldptr) {
    assert(align && !(align & (align - 1)) &&
           "Expected power of two alignment");
    Arena *local = *arena;
    if (local->old == (((char *)oldptr) - local->space)) {
        uintptr_t ptr = (uintptr_t)local->space + local->old;
        align = (align - (ptr & (align - 1))) & (align - 1);
        ptr += align;
        local->cur = local->old + size + align;

        if (local->cur > local->size) {
            return 0;
        }

        return (void *)ptr;
    } else {
        return alloc_align(arena, size, align);
    }
}

void start_scratch(Arena *arena) {
    arena->save_cur = arena->cur;
    arena->save_old = arena->old;
}
void end_scratch(Arena *arena) {
    arena->cur = arena->save_cur;
    arena->old = arena->save_old;
}

void free_all(Arena *arena) {
    arena->cur = 0;
    arena->old = 0;
}

void delete_arena(Arena *arena) { free(arena); }
