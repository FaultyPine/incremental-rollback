#include "mem.h"
#include "profiler.h"

// the [i].AddressArray.Count here represents the total number of pages in this allocation
static std::vector<TrackedBuffer> TrackedMemList = {};

u32 GetPageSize()
{
    static u32 cachedPageSize = 0;
    if (cachedPageSize != 0) return cachedPageSize;
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    assert(info.dwPageSize <= UINT_MAX);
    cachedPageSize = info.dwPageSize;
    return cachedPageSize;
}

// https://github.com/cmuratori/computer_enhance/blob/main/perfaware/part3/listing_0122_write_watch_main.cpp
void HandleNewAllocationTracking(void* ptr, size_t size)
{
    if (!ptr || !size)
        return;
    TrackedBuffer tracked_buf = {};
    u32 pageSize = GetPageSize();
    // make sure we have space for the maximum number of changed pages - that being total pages in the
    // allocated block
    u64 PageCount = ((size + pageSize - 1) / pageSize);

    tracked_buf.buffer.size = size;
    tracked_buf.buffer.data = (char*)ptr;

    AddressArray res;
    // the Count here represents the total number of pages in this allocation
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
    u32 pageSize = GetPageSize();
    for (u64 PageIndex = 0; PageIndex < ChangedPages.Count; ++PageIndex)
    {
        // offset from base buffer pointer that got changed
        u64 changedOffset = ((u8*)ChangedPages.Addresses[PageIndex] - BaseAddress) / pageSize;
        printf("%llu : %llu\n", PageIndex, changedOffset);
    }
}

void PrintTrackedBuf(const TrackedBuffer& buf)
{
    printf("Tracked buffer [%p, %p]\n", 
        buf.buffer.data, buf.buffer.data + buf.buffer.size);
    PrintAddressArray(buf);
}

void ResetWrittenPages()
{
    for (TrackedBuffer& buf : TrackedMemList)
    {
        DWORD PageSize = 0;
        ResetWriteWatch(buf.buffer.data, buf.buffer.size);
    }
}

// [i].Count here represents the total number of CHANGED pages
void GetAndResetWrittenPages(std::vector<AddressArray>& out)
{
    PROFILE_FUNCTION();
    for (TrackedBuffer& buf : TrackedMemList)
    {
        DWORD PageSize = 0;
        ULONG_PTR numAddresses = buf.changedPages.Count;
        // gets changed pages in the specified alloc block (base.data, base.count)
        UINT result;
        { PROFILE_SCOPE("GetWriteWatch");
            result = GetWriteWatch(WRITE_WATCH_FLAG_RESET, buf.buffer.data, buf.buffer.size, buf.changedPages.Addresses,
                            &numAddresses, &PageSize);
        }
        if (result == 0)
        {
            AddressArray changed;
            changed.Addresses = buf.changedPages.Addresses;
            changed.Count = numAddresses;
            out.push_back(changed);
        }
    }
}
