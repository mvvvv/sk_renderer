#pragma once

#include <stdint.h>

typedef enum arena_flags_ {
  arena_flags_no_chain    = (1<<0),
  arena_flags_large_pages = (1<<1),
} arena_flags_;

typedef struct arena_t arena_t;
typedef struct arena_t {
  arena_t*     prev;
  arena_t*     current;
  arena_flags_ flags;
  uint64_t     commit_size;
  uint64_t     reserve_size;
  uint64_t     base_pos;
  uint64_t     pos;
  uint64_t     commit;
  uint64_t     reserve;
} arena_t;

typedef struct arena_temp_t {
  arena_t*     arena;
  uint64_t     pos;
} arena_temp_t;

typedef uint64_t arena_pos_t;

static const uint64_t     arena_default_reserve_size = 64 * 1024 * 1024;
static const uint64_t     arena_default_commit_size  = 64 * 1024;
static const arena_flags_ arena_default_flags        = (arena_flags_)0;

arena_t*     arena_alloc_ (uint64_t reserve_size, uint64_t commit_size, arena_flags_ flags);
void         arena_release(arena_t* arena);
#define arena_alloc() arena_alloc_(arena_default_reserve_size, arena_default_commit_size, arena_default_flags)

void*        arena_push   (arena_t *arena, uint64_t size, uint64_t align);
arena_pos_t  arena_pos    (arena_t *arena);
void         arena_pop_to (arena_t *arena, arena_pos_t pos);

void         arena_clear  (arena_t *arena);
void         arena_pop    (arena_t *arena, uint64_t amt);

arena_temp_t temp_begin   (arena_t *arena);
void         temp_end     (arena_temp_t temp);

#define push_array_no_zero_aligned(a, T, c, align) (T *)arena_push((a), sizeof(T)*(c), (align))
#define push_array_aligned(a, T, c, align) (T *)MemoryZero(push_array_no_zero_aligned(a, T, c, align), sizeof(T)*(c))
#define push_array_no_zero(a, T, c) push_array_no_zero_aligned(a, T, c, Max(8, AlignOf(T)))
#define push_array(a, T, c) push_array_aligned(a, T, c, Max(8, AlignOf(T)))