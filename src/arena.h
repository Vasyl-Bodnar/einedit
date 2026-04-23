#ifndef ARENA_H_
#define ARENA_H_
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct Arena {
    size_t size;
    size_t max_size;
    size_t cur;
    size_t old;
    struct Arena *next;
    char space[];
} Arena;

// No need to delete, you manage space yourself
Arena *create_from(void *space, size_t init_size, size_t max_size);

// Requires correspanding delete
Arena *new_arena(size_t init_size, size_t max_size);

// Allocate with a certain alignment
void *alloc_align(Arena **arena, size_t size, size_t align);

#define alloc(arena, type) alloc_align(arena, sizeof(type), alignof(type))
#define alloc_arr(arena, n, type)                                              \
    alloc_align(arena, sizeof(type) * n, alignof(type))

// Create a subarena
Arena *sublet(Arena **arena, size_t size);

// Resets arena to 0 allocations
void free_all(Arena *arena);

// Only delete if created with `new_arena`
void delete_arena(Arena *arena);

#endif // ARENA_H_
