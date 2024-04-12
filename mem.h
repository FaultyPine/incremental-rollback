#pragma once
#include "util.h"

#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>
#if defined __APPLE__ || defined __FreeBSD__ || defined __OpenBSD__ || defined __NetBSD__
#include <sys/sysctl.h>
#elif defined __HAIKU__
#include <OS.h>
#else
#include <sys/sysinfo.h>
#endif
#endif


struct Buffer
{
    char* data;
    u32 size;
    bool operator==(const Buffer& buf) const { return data == buf.data && size == buf.size; }
};
struct AddressArray
{
  void** Addresses;
  u64 Count;
  u64 PageSize;
};
struct TrackedBuffer
{
  Buffer buffer; // the actual buffer this is tracking
  AddressArray changedPages; // the pages that have been written to
};

static std::vector<TrackedBuffer> TrackedMemList = {};


// https://github.com/cmuratori/computer_enhance/blob/main/perfaware/part3/listing_0122_write_watch_main.cpp
void HandleNewAllocationTracking(void* ptr, size_t size)
{
    if (!ptr || !size)
        return;
    TrackedBuffer tracked_buf = {};
    SYSTEM_INFO Info;
    GetSystemInfo(&Info);
    // make sure we have space for the maximum number of changed pages - that being total pages in the
    // allocated block
    u64 PageCount = ((size + Info.dwPageSize - 1) / Info.dwPageSize);

    tracked_buf.buffer.size = size;
    tracked_buf.buffer.data = (char*)ptr;

    AddressArray res;
    res.Count = PageCount;
    res.Addresses = (void**)malloc(PageCount * sizeof(void**));
    memset(res.Addresses, 0, PageCount * sizeof(void**));
    tracked_buf.changedPages = res;

    TrackedMemList.push_back(tracked_buf);
}

void UntrackMemory(void* ptr)
{
    if (!ptr)
        return;
    for (int i = 0; i < TrackedMemList.size(); i++)
    {
        if (TrackedMemList[i].buffer.data == ptr)
        {
            free(TrackedMemList[i].changedPages.Addresses);
            TrackedMemList.erase(TrackedMemList.begin() + i);
            break;
        }
    }
}

char* TrackedAlloc(size_t size)
{
    #ifdef _WIN32
    void* ptr = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_EXECUTE_READWRITE);
    HandleNewAllocationTracking(ptr, size);
    #else
    #error Unsupported OS type
    #endif
    return (char*)ptr;
}

void TrackedFree(char* ptr)
{
    UntrackMemory(ptr);
    free(ptr);
}


void PrintAddressArray(const TrackedBuffer& buf)
{
    const AddressArray& ChangedPages = buf.changedPages;
    u8* BaseAddress = (u8*)buf.buffer.data;
    for (u64 PageIndex = 0; PageIndex < ChangedPages.Count; ++PageIndex)
    {
        // offset from base buffer pointer that got changed
        u64 changedOffset = ((u8*)ChangedPages.Addresses[PageIndex] - BaseAddress) / ChangedPages.PageSize;
        printf("%llu : %llu\n", PageIndex, changedOffset);
    }
}

void PrintTrackedBuf(const TrackedBuffer& buf)
{
    printf("Tracked buffer [%p, %p]\n", 
        buf.buffer.data, buf.buffer.data + buf.buffer.size);
    PrintAddressArray(buf);
}


std::vector<TrackedBuffer> GetAndResetWrittenPages()
{
    std::vector<TrackedBuffer> result = {};
    for (const TrackedBuffer& buf : TrackedMemList)
    {
        AddressArray ChangedPages = {};

        DWORD PageSize = 0;
        ULONG_PTR AddressCount = buf.changedPages.Count;
        // gets changed pages in the specified alloc block (base.data, base.count)
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, buf.buffer.data, buf.buffer.size, buf.changedPages.Addresses,
                            &AddressCount, &PageSize) == 0)
        {
            ChangedPages.Addresses = buf.changedPages.Addresses;
            ChangedPages.Count = AddressCount;
            ChangedPages.PageSize = PageSize;
        }
        TrackedBuffer new_tracker = {};
        new_tracker.changedPages = ChangedPages;
        new_tracker.buffer = buf.buffer;
        result.push_back(new_tracker);
    }
    return result;
}
