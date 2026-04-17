#include "arena.h"
#include <stdio.h>
#include <stdlib.h>

Arena *create_from(void *space, size_t init_size, size_t max_size) {
    Arena *arena = space;
    printf("%zu %zu\n", max_size, init_size);
    assert(arena && init_size >= sizeof(*arena) &&
           (!max_size || max_size >= init_size) && "Could not create an Arena");
    memset(arena, 0, init_size);
    arena->size = init_size - sizeof(*arena);
    arena->max_size = max_size ? max_size - sizeof(*arena) : 0;
    return arena;
}

Arena *new_arena(size_t init_size, size_t max_size) {
    void *space = malloc(init_size);
    return create_from(space, init_size, max_size);
}

void *alloc_align(Arena **arena, size_t size, size_t align) {
    assert(align && !(align & (align - 1)) &&
           "Expected power of two alignment");
    Arena *arena_ref = *arena;
    uintptr_t ptr = (uintptr_t)arena_ref->space + arena_ref->cur;
    size_t real_align = (align - (ptr & (align - 1))) & (align - 1);
    ptr += real_align;
    arena_ref->old = arena_ref->cur + align;
    arena_ref->cur += size + align;

    if (arena_ref->cur > arena_ref->size) {
        if (arena_ref->max_size > arena_ref->size && size < arena_ref->size) {
            if (!arena_ref->next) {
                arena_ref->next =
                    new_arena(arena_ref->size + sizeof(*arena),
                              (arena_ref->max_size - arena_ref->size) +
                                  sizeof(*arena) * 2);
            }
            return alloc_align(&arena_ref->next, size, align);
        }
        return 0;
    }

    return (void *)ptr;
}

// TODO: These might not be the nicest api now
void start_scratch(Arena *arena) {
    if (arena) {
        arena->save_cur = arena->cur;
        arena->save_old = arena->old;
        start_scratch(arena->next);
    }
}

void end_scratch(Arena *arena) {
    if (arena) {
        arena->cur = arena->save_cur;
        arena->old = arena->save_old;
        end_scratch(arena->next);
    }
}

void free_all(Arena *arena) {
    if (arena) {
        arena->cur = 0;
        arena->old = 0;
        free_all(arena->next);
    }
}

// TODO: What if initial on stack
void delete_arena(Arena *arena) {
    if (arena) {
        Arena *next = arena->next;
        free(arena);
        delete_arena(next);
    }
}
