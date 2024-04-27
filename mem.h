#pragma once
#include "util.h"


#define rbMemcpy(dst, src, size) fastMemcpy(dst, src, size)

struct Buffer
{
    char* data = nullptr;
    u32 size = 0;
    bool operator==(const Buffer& buf) const { return data == buf.data && size == buf.size; }
};

struct AddressArray
{
    void** Addresses;
    u64 Count;
};

struct TrackedBuffer
{
    Buffer buffer; // the actual buffer this is tracking
    AddressArray changedPages; // the pages that have been written to
};

u32 GetPageSize();

// passed in memory block MUST be allocated (at least on windows...) with VirtualAlloc and the MEM_WRITE_WATCH flag
void TrackAlloc(void* ptr, size_t size);
void UntrackAlloc(void* ptr);
void PrintTrackedBuf(const TrackedBuffer& buf);
void ResetWrittenPages();
bool GetAndResetWrittenPages(void** changedPageAddresses, u64* numChangedPages, u64 maxEntries);

// TODO: test/benchmark this
void fastMemcpy(void *pvDest, void *pvSrc, size_t nBytes);