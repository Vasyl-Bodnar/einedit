#include "arena.h"

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

#define alloc(arena, type) alloc_align(arena, sizeof(type), alignof(type))
#define alloc_arr(arena, n, type)                                              \
    alloc_align(arena, sizeof(type) * n, alignof(type))

void start_scratch(Arena *arena) { arena->save = arena->cur; }
void end_scratch(Arena *arena) { arena->cur = arena->save; }

void free_all(Arena *arena) { arena->cur = 0; }

void delete_arena(Arena *arena) { free(arena); }
