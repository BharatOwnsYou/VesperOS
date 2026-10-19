#include "../include/stdlib.h"
#include "../include/types.h"
#include "athenalibc.h"

#define HEAP_SIZE (64 * 1024)
static u8 heap_arena[HEAP_SIZE] __attribute__((aligned(16)));

typedef struct block_header {
    size_t size;
    bool   free;
    struct block_header *next;
} block_header_t;

static block_header_t *heap_head = NULL;

static void heap_init(void) {
    heap_head = (block_header_t*)heap_arena;
    heap_head->size = HEAP_SIZE - sizeof(block_header_t);
    heap_head->free = true;
    heap_head->next = NULL;
}

void *malloc(size_t size) {
    if (!heap_head) heap_init();
    if (size == 0) return NULL;
    size = (size + 7u) & ~(size_t)7u;

    block_header_t *cur = heap_head;
    while (cur) {
        if (cur->free && cur->size >= size) {
            if (cur->size >= size + sizeof(block_header_t) + 8) {
                block_header_t *rest = (block_header_t*)((u8*)cur + sizeof(block_header_t) + size);
                rest->size = cur->size - size - sizeof(block_header_t);
                rest->free = true;
                rest->next = cur->next;
                cur->next = rest;
                cur->size = size;
            }
            cur->free = false;
            return (u8*)cur + sizeof(block_header_t);
        }
        cur = cur->next;
    }
    return NULL;
}

void free(void *ptr) {
    if (!ptr) return;
    block_header_t *blk = (block_header_t*)((u8*)ptr - sizeof(block_header_t));
    blk->free = true;

    block_header_t *cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += sizeof(block_header_t) + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) k_memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    block_header_t *blk = (block_header_t*)((u8*)ptr - sizeof(block_header_t));
    if (blk->size >= size) return ptr;
    void *newp = malloc(size);
    if (newp) {
        size_t copy_len = (blk->size < size) ? blk->size : size;
        k_memcpy(newp, ptr, copy_len);
        free(ptr);
    }
    return newp;
}
