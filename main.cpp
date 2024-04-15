
#include "util.h"
#include "mem.h"
#include "profiler.h"
#include "pagetree.h"

#include <set>

#define MAX_ROLLBACK_FRAMES 7
#define NUM_TEST_FRAMES_TO_SIMULATE 50
#define GAMESTATE_SIZE MEGABYTES_BYTES(170)

#define ENABLE_LOGGING

struct PageSnapshot
{
    // holds the address of this "snapshot" in the actual game mem
    void* originalAddress = nullptr;
    // a copy of the data. Could be allocated anywhere
    void* dataCopy = nullptr;

    void Print() const 
    { 
        printf("%p", originalAddress); 
    }
    operator bool() const { return originalAddress != nullptr; }
    bool operator<(const PageSnapshot& p) const { return originalAddress < p.originalAddress; }
    bool operator>(const PageSnapshot& p) const { return originalAddress > p.originalAddress; }
};

struct Savestate
{
    PageTree<PageSnapshot> afterSnapshots = {};
    PageTree<PageSnapshot> beforeSnapshots = {};
    bool valid = false;
};

struct SavestateInfo
{
    char* fullSnapshot = nullptr;
    // current frame % MAX_ROLLBACK_FRAMES is the index into this for the current frame
    Savestate savestates[MAX_ROLLBACK_FRAMES] = {};
};  


SavestateInfo savestateInfo = {};
char* gameState = nullptr;

char* GetGameState() { return gameState; }

void RandomWrites(u32 numWrites, bool isInit);

void Init()
{
    PROFILE_FUNCTION();
    char* gameMem = GetGameState();
    // gamestate is the only memory we want to track
    if (gameMem != nullptr)
    {
        TrackedFree(gameMem);
    }
    gameState = (char*)TrackedAlloc(GAMESTATE_SIZE);
    RandomWrites(345678, true); // do some writes on init to emulate the initial gamestate when we boot a game/match
    ResetWrittenPages(); // reset written pages since this is supposed to be initial state
}

u32 spotToWrite = GAMESTATE_SIZE / 2; // arbitrary
void RandomWrites(u32 numWrites, bool isInitWrites = false)
{
    PROFILE_FUNCTION();
    u64 pageSize = (u64)GetPageSize();
    char* gameMem = GetGameState();
    if (!gameMem) return;
    std::set<void*> writtenPages = {};
    auto start = std::chrono::high_resolution_clock::now();
    // do some random writes to random spots in mem
    for (int i = 0; i < numWrites; i++)
    {
        void* unalignedSpot = (void*)(gameMem + spotToWrite);
        void* pageAligned = (void*)(((u64)unalignedSpot) & ~(pageSize-1));
        writtenPages.insert(pageAligned);
        gameMem[spotToWrite] = spotToWrite * i; // arbitrary
        auto stop = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start);
        auto timecount = duration.count();
        srand(timecount+i);
        if (rand() % 2 == 0)
        {
            spotToWrite = HashBytes((char*)&spotToWrite, sizeof(spotToWrite)) ;
        }
        spotToWrite = spotToWrite % GAMESTATE_SIZE;
    }
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    auto timecount = duration.count();
    #ifdef ENABLE_LOGGING
    printf("RandomWrites %llu ms\n", timecount);
    if (!isInitWrites)
    {
        for (void* x : writtenPages)
        {
            printf("Wrote to page %p\n", x);
        }
    }
    #endif
}

// pastFrame represents the frame to rollback to
void Rollback(u32 pastFrame)
{
    // TODO:
}

// does this savestate contain region that was just written to? If so, return that region through out
bool DoesSavestateContainCorrespondingUnwrittenRegion(const Savestate& savestate, char* writtenRegion, char*& outBuffer)
{
    PROFILE_FUNCTION();
    PageSnapshot in;
    in.originalAddress = writtenRegion;
    PageSnapshot out;
    // don't need to search before *and* after snapshots - just one or the other.
    // because they will have the same original gamestate addresses
    bool found = savestate.afterSnapshots.Find(in, out);
    outBuffer = (char*)out.dataCopy;
    return found;
}

u32 Wrap(u32 x, u32 wrap)
{
    return abs((s32)x) % wrap;
}

// this takes in a page that has been written to
// it then searches our previous savestates (or falls back to our full snapshot)
// to find the data from this page before it was written to
// passed in page pointer is from game state mem. 
// Returned pointer points to a page worth of memory
// that has NOT had this frame's most recent changes.
// Or to put it another way, based on the pointer to the page passed in,
// we consider that passed in page to have just been written to.
// This returns a block of memory that represents that same page before that write happened
char* GetUnwrittenPageFromWrittenPage(char* newGameMemWrittenData, u32 savestateHead)
{
    PROFILE_FUNCTION();
    savestateHead = Wrap(savestateHead-1, MAX_ROLLBACK_FRAMES); // don't check *this* savestate, since we're in the process of building it
    // go through all the other savestates we have and see if they contain our new written data region
    for (u32 i = 0; i < MAX_ROLLBACK_FRAMES-1; i++)
    {
        Savestate& currentSavestate = savestateInfo.savestates[savestateHead];
        if (!currentSavestate.valid) continue;
        char* unwrittenRegion = nullptr;
        if (DoesSavestateContainCorrespondingUnwrittenRegion(currentSavestate, newGameMemWrittenData, unwrittenRegion))
        {
            #ifdef ENABLE_LOGGING
            printf("Found %p while looking for %p\n", unwrittenRegion, newGameMemWrittenData);
            #endif
            return unwrittenRegion;
        }
        // go backward which checks most recent frames first. Might not really make a difference, but similar mem regions may be written in consecutive frames
        // versus going from current savestateHead forward, we would be checking frames furthest in the past first
        savestateHead = Wrap(savestateHead-1, MAX_ROLLBACK_FRAMES); 
    }
    // not found in any savestates, so we fall back to grabbing that region from the full snapshot
    u64 writtenRegionOffsetFromBase = ((u64)newGameMemWrittenData) - ((u64)GetGameState());
    char* unwrittenSnapshotData = savestateInfo.fullSnapshot + writtenRegionOffsetFromBase;
    #ifdef ENABLE_LOGGING
    printf("Fell back to full snapshot trying to find %p\n", newGameMemWrittenData);
    #endif
    return unwrittenSnapshotData; // NOTE: returning pointer from our full snapshot. THis is not from gamestate mem
}

// TODO: fixed size allocator
void* AllocAndCopy(char* src, u32 size)
{
    void* result = malloc(size);
    memcpy(result, src, size);
    return result;
}

// the passed in buffer points to the mem region that has just been written to. 
// I.E. changedPage should be an address from our gamestate
void OnPageWritten(u32 savestateHead, char* changedGameMemPage)
{
    PROFILE_FUNCTION();
    char* beforeWrittenSnapshotData = GetUnwrittenPageFromWrittenPage(changedGameMemPage, savestateHead);
    u32 pageSize = GetPageSize();
    void* copyOfUnWrittenPage = AllocAndCopy(beforeWrittenSnapshotData, pageSize);
    void* copyOfWrittenPage = AllocAndCopy(changedGameMemPage, pageSize);
    PageSnapshot beforeSnapshot;
    beforeSnapshot.originalAddress = changedGameMemPage;
    beforeSnapshot.dataCopy = copyOfUnWrittenPage;
    PageSnapshot afterSnapshot;
    afterSnapshot.originalAddress = changedGameMemPage;
    afterSnapshot.dataCopy = copyOfWrittenPage;
    Savestate& currentSavestate = savestateInfo.savestates[savestateHead];
    currentSavestate.afterSnapshots.Insert(afterSnapshot);
    currentSavestate.beforeSnapshots.Insert(beforeSnapshot);
    currentSavestate.valid = true;
}

void EvictSavestate(Savestate& savestate)
{
    u32 pageSize = GetPageSize();
    char* fullSnapshot = savestateInfo.fullSnapshot;
    char* gameMem = GetGameState();
    // when we evict a frame of savestate, by copying that frame's data into our
    // full snapshot, we effectively move our window of frames we can rollback to up by 1
    for (PageSnapshot& snap : savestate.afterSnapshots.pages)
    {
        if (snap.originalAddress == nullptr) continue;
        u64 offset = ((u64)snap.originalAddress) - ((u64)gameMem);
        // the "original address" is relative to the game mem. Use that as an offset into our snapshot
        char* snapshotEquivalentAddress = fullSnapshot + offset;
        memcpy(snapshotEquivalentAddress, snap.dataCopy, pageSize);
    }

    //  free up all the page snapshots tied to it
    for (PageSnapshot& snap : savestate.beforeSnapshots.pages)
    {
        free(snap.dataCopy);
    }
    savestate.beforeSnapshots.Clear();
    for (PageSnapshot& snap : savestate.afterSnapshots.pages)
    {
        free(snap.dataCopy);
    }
    savestate.afterSnapshots.Clear();
    savestate.valid = false;
}

void SaveWrittenPages(u32 frame)
{
    PROFILE_FUNCTION();
    std::vector<AddressArray> changedPages = {};
    GetAndResetWrittenPages(changedPages);
    u32 savestateHead = frame % MAX_ROLLBACK_FRAMES;
    if (savestateInfo.savestates[savestateHead].valid)
    {
        EvictSavestate(savestateInfo.savestates[savestateHead]);
    }
    size_t numChangedBytes = 0;
    size_t numChangedPages = 0;
    u32 pageSize = GetPageSize();
    for (const AddressArray& buf : changedPages)
    {
        numChangedBytes += buf.Count * pageSize;
        numChangedPages += buf.Count;
        for (u32 i = 0; i < buf.Count; i++)
        {
            // pointer + size for the block of memory we know was written to during the game frame
            char* changedPageMem = (char*)buf.Addresses[i];
            OnPageWritten(savestateHead, changedPageMem);
        }
    }
    #ifdef ENABLE_LOGGING
    f64 changedMB = numChangedBytes / 1024.0 / 1024.0;
    printf("Frame %i, head = %i\nAllocation blocks = %llu\tNum changed pages = %llu\tChanged bytes = %llu   MB = %f\n", 
            frame, savestateHead, changedPages.size(), numChangedPages, numChangedBytes, changedMB);
    #endif
}

bool Tick(u32 frame)
{
    PROFILER_FRAME_MARK();
    PROFILE_FUNCTION();
    #ifdef ENABLE_LOGGING
    printf("---------------------------\n");
    #endif
    auto start = std::chrono::high_resolution_clock::now();
    if (frame == 0)
    {
        printf("Taking full gamestate snapshot...\n");
        // full snapshot
        if (savestateInfo.fullSnapshot)
        {
            free(savestateInfo.fullSnapshot);
            savestateInfo.fullSnapshot = nullptr;
        }
        savestateInfo.fullSnapshot = (char*)malloc(GAMESTATE_SIZE);
        memcpy(savestateInfo.fullSnapshot, GetGameState(), GAMESTATE_SIZE);
        auto stop = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
        auto timecount = duration.count();
        printf("Full snapshot took %llu ms\n", timecount);
    }

    // mess with our game memory. This is to simulate the game sim
    RandomWrites(10);
    // whatever changes we made above, save those
    SaveWrittenPages(frame);

    #ifdef ENABLE_LOGGING
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(stop - start);
    auto timecount = duration.count();
    printf("Frame %i took %llu microsec\n", frame, timecount);
    #endif
    // tmp for testing. Should test for more frames later when savestates are properly evicted
    return frame < NUM_TEST_FRAMES_TO_SIMULATE;
}

u32 frame = 0;
int main()
{
    frame = 0;
    Init();
    while (Tick(frame++)) {}
    #ifdef _WIN32
    system("pause");
    #endif
    return 0;
}





