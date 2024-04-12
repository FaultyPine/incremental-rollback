
#include "util.h"
#include "mem.h"
#include "profiler.h"

#define MAX_ROLLBACK_FRAMES 7
#define NUM_TEST_FRAMES_TO_SIMULATE 50
#define GAMESTATE_SIZE MEGABYTES_BYTES(170)


struct Savestate
{
    std::vector<Buffer> beforeSnapshots = {};
    std::vector<Buffer> afterSnapshots = {};
};

struct SavestateInfo
{
    char* fullSnapshot = nullptr;
    // current frame % MAX_ROLLBACK_FRAMES is the index into this for the current frame
    Savestate savestates[MAX_ROLLBACK_FRAMES] = {};
};  


SavestateInfo savestateInfo = {};
char* gameState = nullptr;

void RandomWrites(u32 numWrites);

void Init()
{
    PROFILE_FUNCTION();
    for (u32 i = 0; i < MAX_ROLLBACK_FRAMES; i++)
    {
        savestateInfo.savestates[i].beforeSnapshots.reserve(4000);
        savestateInfo.savestates[i].afterSnapshots.reserve(4000);
    }
    // gamestate is the only memory we want to track
    if (gameState != nullptr)
    {
        TrackedFree(gameState);
    }
    gameState = (char*)TrackedAlloc(GAMESTATE_SIZE);
    RandomWrites(345678); // do some writes on init to emulate the initial gamestate when we boot a game/match
    GetAndResetWrittenPages(); // reset written pages since this is supposed to be initial state
}

int spotToWrite = GAMESTATE_SIZE / 2; // arbitrary
void RandomWrites(u32 numWrites)
{
    PROFILE_FUNCTION();
    if (!gameState) return;
    auto start = std::chrono::high_resolution_clock::now();
    // do some random writes to random spots in mem
    for (int i = 0; i < numWrites; i++)
    {
        gameState[spotToWrite] = spotToWrite * i; // arbitrary
        spotToWrite = HashBytes((char*)&spotToWrite, sizeof(spotToWrite)) % GAMESTATE_SIZE;
    }
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    auto timecount = duration.count();
    printf("RandomWrites %llu ms\n", timecount);
}

/*
Frame 0: 
- full snapshot - done

Frame x: 
- Get written pages - done


Rollback:

*/

// does this savestate contain region that was just written to? If so, return that region through out
bool DoesSavestateContainCorrespondingUnwrittenRegion(const Savestate& savestate, Buffer writtenRegion, Buffer& out)
{
    PROFILE_FUNCTION();
    // TODO: OPTIMIZE: change from vector search to tree search
    auto iter = std::find(savestate.beforeSnapshots.begin(), savestate.beforeSnapshots.end(), writtenRegion);
    if (iter != savestate.beforeSnapshots.end())
    {
        out = *iter;
        return true;
    }
    return false;
}

// this takes in a page that has been written to
// it then searches our previous savestates (or falls back to our full snapshot)
// to find the data from this page before it was written to
Buffer GetUnwrittenPageFromWrittenPage(Buffer newWrittenData, u32 savestateHead)
{
    PROFILE_FUNCTION();
    savestateHead = (savestateHead + 1) % MAX_ROLLBACK_FRAMES; // don't check *this* savestate, since we're in the process of building it
    // go through all the other savestates we have and see if they contain our new written data region
    for (u32 i = 0; i < MAX_ROLLBACK_FRAMES-1; i++)
    {
        Savestate& currentSavestate = savestateInfo.savestates[savestateHead];
        Buffer unwrittenRegion;
        if (DoesSavestateContainCorrespondingUnwrittenRegion(currentSavestate, newWrittenData, unwrittenRegion))
        {
            return unwrittenRegion;
        }
        savestateHead = (savestateHead + 1) % MAX_ROLLBACK_FRAMES;
    }
    // not found in any savestates, so we fall back to grabbing that region from the full snapshot
    u64 writtenRegionOffsetFromBase = ((u64)newWrittenData.data) - ((u64)gameState);
    char* unwrittenData = savestateInfo.fullSnapshot + writtenRegionOffsetFromBase;
    Buffer unwrittenBuffer = {unwrittenData, newWrittenData.size};
    return unwrittenBuffer;
}

void OnPageWritten(u32 frame, char* changedPage, u32 changedPageSize)
{
    PROFILE_FUNCTION();
    // the passed in buffer points to the mem region that has just been written to. So this contains the "new" written data
    Buffer newWrittenData = {changedPage, changedPageSize};
    u32 savestateHead = frame % MAX_ROLLBACK_FRAMES;
    Savestate& currentSavestate = savestateInfo.savestates[savestateHead];
    Buffer beforeWrittenData = GetUnwrittenPageFromWrittenPage(newWrittenData, savestateHead);
    // TODO: OPTIMIZE: change from vector push to tree insert
    currentSavestate.afterSnapshots.push_back(newWrittenData);
    currentSavestate.beforeSnapshots.push_back(beforeWrittenData);
    // TODO: evict old savestates (do it here? or elsewhere?)
}

void SaveWrittenPages(u32 frame)
{
    PROFILE_FUNCTION();
    std::vector<TrackedBuffer> changedPages = GetAndResetWrittenPages();
    // just for debugging
    size_t numChangedBytes = 0;
    size_t numChangedPages = 0;
    for (const TrackedBuffer& buf : changedPages)
    {
        numChangedBytes += buf.changedPages.Count * buf.changedPages.PageSize;
        numChangedPages += buf.changedPages.Count;
        for (u32 i = 0; i < buf.changedPages.Count; i++)
        {
            // pointer + size for the block of memory we know was written to during the game frame
            char* changedPageMem = (char*)buf.changedPages.Addresses[i];
            u32 changedPageSize = (u32)buf.changedPages.PageSize; // page sizes don't change... don't rlly need to store this
            OnPageWritten(frame, changedPageMem, changedPageSize);
        }
    }
    f64 changedMB = numChangedBytes / 1024.0 / 1024.0;
    printf("Frame %i\nAllocation blocks = %llu\nNum changed pages = %llu\nChanged bytes = %llu   MB = %f\n", 
            frame, changedPages.size(), numChangedPages, numChangedBytes, changedMB);
}

bool Tick(u32 frame)
{
    PROFILER_FRAME_MARK();
    PROFILE_FUNCTION();
    printf("---------------------------\n");
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
        memcpy(savestateInfo.fullSnapshot, gameState, GAMESTATE_SIZE);
        auto stop = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
        auto timecount = duration.count();
        printf("Full snapshot took %llu ms\n", timecount);
    }

    // mess with our game memory. This is to simulate the game sim
    RandomWrites(4000);
    // whatever changes we made above, save those
    SaveWrittenPages(frame);

    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    auto timecount = duration.count();
    printf("Frame %i took %llu ms\n", frame, timecount);
    // tmp for testing. Should test for more frames later when savestates are properly evicted
    return frame <= MAX_ROLLBACK_FRAMES;
}

u32 frame = 0;
int main()
{
    frame = 0;
    Init();
    while (Tick(frame++)) {}
    return 0;
}





