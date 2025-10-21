#include "arena.h"

#include <stdbool.h>

///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint64_t os_page_size_get() {
	SYSTEM_INFO sysinfo = {0};
	GetSystemInfo(&sysinfo);
	return sysinfo.dwPageSize;
}
uint64_t os_page_size_large_get() {
	return GetLargePageMinimum();
}
void* mem_reserve(uint64_t size) {
	return VirtualAlloc(0, size, MEM_RESERVE, PAGE_READWRITE);
}
void* mem_reserve_large(uint64_t size) {
	return VirtualAlloc(0, size, MEM_RESERVE|MEM_COMMIT|MEM_LARGE_PAGES, PAGE_READWRITE);
}
bool mem_commit(void* ptr, uint64_t size) {
	return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != 0;
}
bool mem_commit_large(void* ptr, uint64_t size) {
	return true;
}
void mem_decommit(void* ptr, uint64_t size) {
	VirtualFree(ptr, size, MEM_DECOMMIT);
}
void mem_release(void* ptr, uint64_t size) {
	VirtualFree(ptr, 0, MEM_RELEASE);
}

///////////////////////////////////////////////////////////////////////////////

#else

///////////////////////////////////////////////////////////////////////////////

#include <unistd.h> // getpagesize
#include <sys/mman.h> // mmap

uint64_t os_page_size_get() {
	return (uint64_t)getpagesize();
}
uint64_t os_page_size_large_get() {
	return 2*1024*1024;
}
void* mem_reserve(uint64_t size) {
	void *result = mmap(0, size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	return result == MAP_FAILED
		? 0
		: result;
}
void* mem_reserve_large(uint64_t size) {
	void *result = mmap(0, size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
	return result == MAP_FAILED
		? 0
		: result;
}
bool mem_commit(void *ptr, uint64_t size) {
	mprotect(ptr, size, PROT_READ|PROT_WRITE);
	return true;
}
bool mem_commit_large(void *ptr, uint64_t size) {
	mprotect(ptr, size, PROT_READ|PROT_WRITE);
	return true;
}
void mem_decommit(void *ptr, uint64_t size) {
	madvise (ptr, size, MADV_DONTNEED);
	mprotect(ptr, size, PROT_NONE);
}
void mem_release(void *ptr, uint64_t size) {
	munmap(ptr, size);
}

///////////////////////////////////////////////////////////////////////////////

#endif

///////////////////////////////////////////////////////////////////////////////

inline uint64_t align_pow_2(uint64_t value, uint64_t align) { return (value + (align - 1)) & ~(align - 1); }

///////////////////////////////////////////////////////////////////////////////

arena_t* arena_alloc_(uint64_t reserve_size, uint64_t commit_size, arena_flags_ flags) {
	uint64_t page_size = (flags & arena_flags_large_pages) > 0
		? os_page_size_large_get()
		: os_page_size_get      ();
	uint64_t final_reserve_size = align_pow_2(reserve_size, page_size);
	uint64_t final_commit_size  = align_pow_2(commit_size,  page_size);
	
	void *base = NULL;
	if (flags & arena_flags_large_pages) {
		base = mem_reserve_large(final_reserve_size);
		mem_commit_large(base, final_commit_size);
	} else {
		base = mem_reserve(final_reserve_size);
		mem_commit(base, final_commit_size);
	}
	
	arena_t *arena = (arena_t *)base;
	arena->current      = arena;
	arena->flags        = flags;
	arena->commit_size  = commit_size;
	arena->reserve_size = reserve_size;
	arena->base_pos     = 0;
	arena->pos          = align_pow_2(sizeof(arena_t), 16);
	arena->commit       = final_commit_size;
	arena->reserve      = final_reserve_size;
	return arena;
}

///////////////////////////////////////////////////////////////////////////////

void arena_release(arena_t *arena) {
	for(arena_t *n = arena->current, *prev = 0; n != 0; n = prev) {
		prev = n->prev;
		mem_release(n, n->reserve);
	}
}

///////////////////////////////////////////////////////////////////////////////

void* arena_push(arena_t *arena, uint64_t size, uint64_t align) {
	arena_t* current = arena->current;
	uint64_t pos_pre = align_pow_2(current->pos, align);
	uint64_t pos_pst = pos_pre + size;
	
	if (current->reserve < pos_pst && !(arena->flags & arena_flags_no_chain)) {
		uint64_t reserve_size = current->reserve_size;
		uint64_t commit_size  = current->commit_size;
		uint64_t header_size  = align_pow_2(sizeof(arena_t), 16);
		if (size + header_size > reserve_size) {
			reserve_size = align_pow_2(size + header_size, align);
			commit_size  = align_pow_2(size + header_size, align);
		}
		arena_t *new_block = arena_alloc_(reserve_size, commit_size, current->flags);
		
		new_block->base_pos = current->base_pos + current->reserve;
		new_block->prev     = arena->current;
		arena->current = new_block;
		
		current = new_block;
		pos_pre = align_pow_2(current->pos, align);
		pos_pst = pos_pre + size;
	}
	
	if (current->commit < pos_pst) {
		uint64_t commit_pst_aligned = pos_pst + current->commit_size-1;
		commit_pst_aligned -= commit_pst_aligned%current->commit_size;
		uint64_t commit_pst_clamped = commit_pst_aligned < current->reserve ? commit_pst_aligned : current->reserve;
		uint64_t commit_size        = commit_pst_clamped - current->commit;
		uint8_t* commit_ptr         = (uint8_t *)current + current->commit;
		if(current->flags & arena_flags_large_pages) {
			mem_commit_large(commit_ptr, commit_size);
		} else {
			mem_commit(commit_ptr, commit_size);
		}
		current->commit = commit_pst_clamped;
	}
	
	void *result = 0;
	if (current->commit >= pos_pst) {
		result = (uint8_t *)current+pos_pre;
		current->pos = pos_pst;
	}
	return result;
}

///////////////////////////////////////////////////////////////////////////////

arena_pos_t arena_pos(arena_t *arena) {
	return arena->current->base_pos + arena->current->pos;
}

///////////////////////////////////////////////////////////////////////////////

void arena_pop_to(arena_t *arena, arena_pos_t pos) {
	uint64_t start   = align_pow_2(sizeof(arena_t), 16);
	uint64_t big_pos = pos > start ? pos : start;
	arena_t* current = arena->current;
	
	for(arena_t *prev = 0; current->base_pos >= big_pos; current = prev) {
		prev = current->prev;
		mem_release(current, current->reserve);
	}
	arena  ->current = current;
	current->pos     = big_pos - current->base_pos;
}

///////////////////////////////////////////////////////////////////////////////

void arena_clear(arena_t *arena) {
	arena_pop_to(arena, 0);
}

///////////////////////////////////////////////////////////////////////////////

void arena_pop(arena_t *arena, uint64_t amt) {
	uint64_t pos_old = arena_pos(arena);
	arena_pop_to(arena, amt < pos_old 
		? pos_old - amt
		: pos_old);
}

///////////////////////////////////////////////////////////////////////////////

arena_temp_t temp_begin(arena_t *arena) {
	return (arena_temp_t){arena, arena_pos(arena)};
}

///////////////////////////////////////////////////////////////////////////////

void temp_end(arena_temp_t temp) {
	arena_pop_to(temp.arena, temp.pos);
}
