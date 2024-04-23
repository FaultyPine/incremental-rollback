
#include "util.h"
#include "mem.h"
#include "profiler.h"
#include "pagetree.h"

#include <set>

#include "external/mimalloc/src/static.c"

#define MAX_ROLLBACK_FRAMES 7
#define NUM_TEST_FRAMES_TO_SIMULATE 50
#define GAMESTATE_SIZE MEGABYTES_BYTES(170)

#define ENABLE_LOGGING
// should be ~1500 for real perf testing
constexpr u32 NUM_RANDOM_WRITES_PER_FRAME = 10;


// FUTURE: go faster than memcpy - https://squadrick.dev/journal/going-faster-than-memcpy.html
// we have very specific restrictions on the blocks of mem we move around
// always page-sized, and pages are always aligned... surely there's some wins there. Also very easy to parallelize

// TODO:
// - figure out how/why beforeSnapshots have frames from way in the past.
//      Changing the rollback to use afterSnapshots made all the frame numbers line up, but that doesn't make much sense
//      since I would think we'd need to rollback to the beginning of a frame, not after changes have been made.
//      so why do beforeSnapshots not have the correct frames? (or have frames from a while ago)
// - Test in dolphin - make really sure I know how many pages are changed in an average p+ game.
//      track mem1 and mem2 but ALSO track the other memory regions (fake vram, other stuff?) if possible. 
//      Test to absolutely make sure I know what kind of
//      perf environment im working with (in terms of how many pages are changed on average during a normal game)
//      ---------
//      RESULTS OF DOLPHIN TESTS
//          Getting a little over ~1500 changed pages (which is ~6mb) per frame during gameplay. This is for ALL of dolphin mem
//          replaced dolphin's backing pagefile with one big VirtualAlloc. Replaced MapViewOfFile and related calls
//          with just returning a pointer into the big backing memory block.
//          Tracking that entire arena is about 170mb, and includes stuff like L1 cache, fake vmem, mem1, mem2


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
    u32 frame = 0;
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
u32* GetGameMemFrame() { return (u32*)gameState; } // first 4 bytes of game mem are the current frame

void GameSimulateFrame(u32 currentFrame, u32 numWrites);

void Init(u32& currentFrame)
{
    PROFILE_FUNCTION();
    currentFrame = 0;
    char* gameMem = GetGameState();
    // gamestate is the only memory we want to track
    if (gameMem != nullptr)
    {
        TrackedFree(gameMem);
    }
    gameState = (char*)TrackedAlloc(GAMESTATE_SIZE);
    ResetWrittenPages(); // reset written pages since this is supposed to be initial state
}

// byte index into game mem block
u64 spotToWrite = GAMESTATE_SIZE / 2; // arbitrary
void GameSimulateFrame(u32 currentFrame, u32 numWrites)
{
    PROFILE_FUNCTION();
    u64 pageSize = (u64)GetPageSize();
    char* gameMem = GetGameState();
    if (!gameMem) return;
    std::set<void*> writtenPages = {};
    // for debugging. Write to the first 4 bytes of game mem every frame indicating the current 
    *GetGameMemFrame() = currentFrame; 
    /*
    u32 hash = currentFrame % 5;
    hash = HashBytes((char*)&hash, sizeof(hash));
    srand(hash);
    */
    auto start = std::chrono::high_resolution_clock::now();
    // do some random writes to random spots in mem
    for (int i = 0; i < numWrites; i++)
    {
        void* unalignedSpot = (void*)(gameMem + spotToWrite);
        void* pageAligned = (void*)(((u64)unalignedSpot) & ~(pageSize-1));
        writtenPages.insert(pageAligned);
        /*
        if (rand() % 2 == 0)
        {
            spotToWrite = HashBytes((char*)&spotToWrite, sizeof(spotToWrite)) ;
        }
        */
        spotToWrite = spotToWrite - ((u64)gameMem) + pageSize + (pageSize/2);
        spotToWrite = spotToWrite % GAMESTATE_SIZE;
        *(u32*)(&gameMem[spotToWrite]) = spotToWrite; // do some arbitrary write
    }
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    auto timecount = duration.count();
    #ifdef ENABLE_LOGGING
    printf("GameSimulateFrame %llu ms [frame %i] [wrote %u pages]\n", timecount, currentFrame, writtenPages.size());
    /*if (currentFrame > 0)
    {
        for (void* x : writtenPages)
        {
            printf("Wrote to page %p\n", x);
        }
    }*/
    #endif
}

s32 Wrap(s32 x, s32 wrap)
{
    if (x < 0) x = (wrap + x);
    return abs(x) % wrap;
}

void* TryFindBasePage(const PageTree<PageSnapshot>& pages)
{
    for (const PageSnapshot& snap : pages.pages)
    {
        if (snap.originalAddress == GetGameState())
        {
            return snap.originalAddress;
        }
    }
    return nullptr;
}

void Rollback(u32& currentFrame, u32 rollbackFrame)
{
    if (currentFrame < MAX_ROLLBACK_FRAMES) return;
    u32 pageSize = GetPageSize();
    char* gameMem = GetGameState();
    s32 savestateOffset = currentFrame-rollbackFrame;
    assert(rollbackFrame < currentFrame && savestateOffset < MAX_ROLLBACK_FRAMES);
    s32 currentSavestateIdx = currentFrame % MAX_ROLLBACK_FRAMES;
    s32 savestateIdx = Wrap(currentSavestateIdx - savestateOffset, MAX_ROLLBACK_FRAMES);
    assert(savestateIdx < MAX_ROLLBACK_FRAMES && savestateIdx != currentSavestateIdx);
    #ifdef ENABLE_LOGGING
    printf("Starting at game mem frame %i\n", *GetGameMemFrame());
    printf("Rolling back %i frames from idx %i -> %i\n", savestateOffset, currentSavestateIdx, savestateIdx);
    printf("Savestate frames stored:\n");
    for (u32 i = 0; i < MAX_ROLLBACK_FRAMES; i++)
    {
        printf("idx %u = frame %u |\t", i, savestateInfo.savestates[i].frame);
    }
    printf("\n");
    #endif
    // given an index into our savestates list, rollback to that.
    // go from the current index, walking backward applying each savestate's "before" snapshots
    // one at a time
    while (currentSavestateIdx != savestateIdx)
    {
        Savestate& savestate = savestateInfo.savestates[currentSavestateIdx];  
        if (savestate.valid)
        {
            for (PageSnapshot& pageSnapshot : savestate.beforeSnapshots.pages)
            {
                if (pageSnapshot.originalAddress != nullptr)
                {
                    #ifdef ENABLE_LOGGING
                    assert(pageSnapshot.originalAddress >= GetGameState() && pageSnapshot.originalAddress < GetGameState() + GAMESTATE_SIZE);
                    // first 4 bytes of game mem contains current frame
                    if (pageSnapshot.originalAddress == gameMem) 
                    {
                        u32 nowFrame = *GetGameMemFrame();
                        printf("rolling back %u -> %u\n", nowFrame, ((u32*)pageSnapshot.dataCopy)[0]);
                    }
                    #endif
                    memcpy(pageSnapshot.originalAddress, pageSnapshot.dataCopy, pageSize);
                }
            }
            #ifdef ENABLE_LOGGING
            printf("Rolled back savestate idx %i\n", currentSavestateIdx);
            #endif
        }
        currentSavestateIdx = Wrap(currentSavestateIdx-1, MAX_ROLLBACK_FRAMES);
    }
    currentFrame = rollbackFrame;
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
    bool found = savestate.beforeSnapshots.Find(in, out);
    outBuffer = (char*)out.dataCopy;
    return found;
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
            printf("Found %p while looking for %p  frame %u\n", unwrittenRegion, newGameMemWrittenData, currentSavestate.frame);
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
    //printf("Fell back to full snapshot trying to find %p\n", newGameMemWrittenData);
    #endif
    return unwrittenSnapshotData; // NOTE: returning pointer from our full snapshot. THis is not from gamestate mem
}

// PERF: fixed size allocator
// malloc is wicked slow holy shit
void* AllocAndCopy(char* src, u32 size)
{
    PROFILE_FUNCTION();
    void* result;
    { PROFILE_SCOPE("malloc");
        result = mi_malloc(size);
    }
    { PROFILE_SCOPE("memcpy");
        memcpy(result, src, size);
    }
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
        mi_free(snap.dataCopy);
    }
    savestate.beforeSnapshots.Clear();
    for (PageSnapshot& snap : savestate.afterSnapshots.pages)
    {
        mi_free(snap.dataCopy);
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
    savestateInfo.savestates[savestateHead].frame = frame;
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

bool Tick(u32& frame)
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
            mi_free(savestateInfo.fullSnapshot);
            savestateInfo.fullSnapshot = nullptr;
        }
        savestateInfo.fullSnapshot = (char*)mi_malloc(GAMESTATE_SIZE);
        memcpy(savestateInfo.fullSnapshot, GetGameState(), GAMESTATE_SIZE);
        auto stop = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
        auto timecount = duration.count();
        printf("Full snapshot took %llu ms\n", timecount);
    }

    // mess with our game memory. This is to simulate the game sim
    GameSimulateFrame(frame, NUM_RANDOM_WRITES_PER_FRAME);
    // whatever changes we made above, save those
    SaveWrittenPages(frame);
    constexpr u32 NUM_FRAMES_TO_ROLLBACK = 5;
    if (frame % 15 == 0)
    {
        // rollback
        u32 originalFrame = frame;
        u32 frameToRollbackTo = frame - NUM_FRAMES_TO_ROLLBACK;
        Rollback(frame, frameToRollbackTo);
        #ifdef ENABLE_LOGGING
        printf("Rolled back to frame %i\n", *GetGameMemFrame());
        #endif
        // resim
        for (u32 i = frameToRollbackTo; i <= originalFrame; i++)
        {
            GameSimulateFrame(i, NUM_RANDOM_WRITES_PER_FRAME);
            frame++;
        }
        #ifdef ENABLE_LOGGING
        printf("Resimulated back to frame %i\n", *GetGameMemFrame());
        #endif
        assert(originalFrame == *GetGameMemFrame());
        ResetWrittenPages();
    }

    #ifdef ENABLE_LOGGING
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    auto timecount = duration.count();
    printf("Frame %i took %llu ms\n", frame, timecount);
    #endif
    // tmp for testing. Should test for more frames later when savestates are properly evicted
    bool shouldExit = frame < NUM_TEST_FRAMES_TO_SIMULATE;
    frame++;
    return shouldExit;
}

u32 frame = 0;
int main()
{
    frame = 0;
    Init(frame);
    while (Tick(frame)) {}
    #ifdef _WIN32
    system("pause");
    #endif
    return 0;
}





