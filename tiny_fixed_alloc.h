#ifndef TINY_FIXED_ALLOCATOR_H
#define TINY_FIXED_ALLOCATOR_H

// a simple fixed-size block allocator w/ freelist
#include "util.h"


struct FixedBlockAllocator
{
    u8* mem;
    size_t memSize;
    size_t blockSize;
    size_t offset;
    u8* freeListStart;
    u8* freeListEnd;
};


FixedBlockAllocator InitializeFixedBlockAllocator(u8* backingMem, size_t memSize, size_t blockSize);
void* fixedblock_alloc(FixedBlockAllocator* allocator);
void fixedblock_free_block(FixedBlockAllocator* allocator, void* block);
void fixedblock_clear(FixedBlockAllocator* allocator);


void fixedblock_allocator_tests();

#endif