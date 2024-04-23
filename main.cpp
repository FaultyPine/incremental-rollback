
#include "util.h"
#include "mem.h"
#include "profiler.h"
#include "pagetree.h"

#include <set>

#include "external/mimalloc/src/static.c"

#define MAX_ROLLBACK_FRAMES 7
#define NUM_TEST_FRAMES_TO_SIMULATE 10
#define GAMESTATE_SIZE MEGABYTES_BYTES(170)

#define ENABLE_LOGGING
// should be ~1500 for real perf testing
constexpr u32 NUM_RANDOM_WRITES_PER_FRAME = 1500;


// FUTURE: go faster than memcpy - https://squadrick.dev/journal/going-faster-than-memcpy.html
// we have very specific restrictions on the blocks of mem we move around
// always page-sized, and pages are always aligned... surely there's some wins there. Also very easy to parallelize

// TODO:
// - figure out how/why beforeSnapshots have frames from way in the past.
//      Changing the rollback to use afterSnapshots made all the frame numbers line up, but that doesn't make much sense
//      since I would think we'd need to rollback to the beginning of a frame, not after changes have been made.
//      so why do beforeSnapshots not have the correct frames? (or have frames from a while ago)
// - [Done] Test in dolphin - make really sure I know how many pages are changed in an average p+ game.
//      track mem1 and mem2 but ALSO track the other memory regions (fake vram, other stuff?) if possible. 
//      Test to absolutely make sure I know what kind of
//      perf environment im working with (in terms of how many pages are changed on average during a normal game)
//      ---------
//      RESULTS OF DOLPHIN TESTS
//          Getting a little over ~1500 changed pages (which is ~6mb) per frame during gameplay. This is for ALL of dolphin mem
//          replaced dolphin's backing pagefile with one big VirtualAlloc. Replaced MapViewOfFile and related calls
//          with just returning a pointer into the big backing memory block.
//          Tracking that entire arena is about 170mb, and includes stuff like L1 cache, fake vmem, mem1, mem2
// - [Done] GetWriteWatch returns it's addresses in ascending order! We can exploit this to speed up our tree/lookup stuff
//      instead of inserting each written page into a tree-like structure. We could just memcpy the changed addresses in
//      that array of addresses could already be bsearched, so we would just need to iterate it and make our copies

constexpr u64 MAX_NUM_CHANGED_PAGES = 2000;

struct Savestate
{
    // sorted (ascending) list of changed pages
    void* changedPages[MAX_NUM_CHANGED_PAGES] = {}; 
    // list of pointers to blocks of memory (page sized) that contain data before that page was written to on this frame
    void* beforeCopies[MAX_NUM_CHANGED_PAGES] = {}; 
    // same as above ^ but contains data after this frame wrote to the pages
    void* afterCopies[MAX_NUM_CHANGED_PAGES] = {}; 
    u64 numChangedPages = 0;
    u32 frame = 0;
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

void GameSimulateFrame(u32 currentFrame, u32 numWrites)
{
    PROFILE_FUNCTION();
    u64 spotToWrite = GAMESTATE_SIZE / 2; // arbitrary byte index into game mem block
    u64 pageSize = (u64)GetPageSize();
    char* gameMem = GetGameState();
    if (!gameMem) return;
    std::set<void*> writtenPages = {};
    // for debugging. Write to the first 4 bytes of game mem every frame indicating the current 
    #ifdef ENABLE_LOGGING
    printf("Advanced internal frame %u -> %u\n", *GetGameMemFrame(), currentFrame);
    #endif
    *GetGameMemFrame() = currentFrame; 
    writtenPages.insert(GetGameMemFrame());
    auto start = std::chrono::high_resolution_clock::now();
    // do some random writes to random spots in mem
    for (int i = 0; i < numWrites; i++)
    {
        void* unalignedSpot = (void*)(gameMem + spotToWrite);
        void* pageAligned = (void*)(((u64)unalignedSpot) & ~(pageSize-1));
        writtenPages.insert(pageAligned);
        spotToWrite = spotToWrite - ((u64)gameMem) + pageSize + (pageSize/2);
        spotToWrite = spotToWrite % GAMESTATE_SIZE;
        *(u32*)(&gameMem[spotToWrite]) = spotToWrite; // do some arbitrary write
    }
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    auto timecount = duration.count();
    #ifdef ENABLE_LOGGING
    printf("GameSimulateFrame %llu ms [frame %i] [wrote %llu pages]\n", timecount, currentFrame, writtenPages.size());
    #endif
}

s32 Wrap(s32 x, s32 wrap)
{
    if (x < 0) x = (wrap + x);
    return abs(x) % wrap;
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
    printf("Rolling back %i frames from idx %i -> %i | frame %u -> %u\n", 
                        savestateOffset, currentSavestateIdx, savestateIdx, currentFrame, rollbackFrame);
    printf("Savestate frames stored:\n");
    for (u32 i = 0; i < MAX_ROLLBACK_FRAMES; i++)
    {
        printf("| idx %u = frame %u |\t", i, savestateInfo.savestates[i].frame);
    }
    printf("\n");
    #endif
    // given an index into our savestates list, rollback to that.
    // go from the current index, walking backward applying each savestate's "before" snapshots
    // one at a time
    while (currentSavestateIdx != savestateIdx)
    {
        Savestate& savestate = savestateInfo.savestates[currentSavestateIdx];  
        for (u32 i = 0; i < savestate.numChangedPages; i++)
        {
            void* orig = savestate.changedPages[i];
            void* beforeCopy = savestate.beforeCopies[i];
            if (orig != nullptr)
            {
                #ifdef ENABLE_LOGGING
                assert(orig >= GetGameState() && orig < GetGameState() + GAMESTATE_SIZE);
                // first 4 bytes of game mem contains current frame
                if (orig == gameMem) 
                {
                    u32 nowFrame = *GetGameMemFrame();
                    printf("rolling back %u -> %u\n", nowFrame, ((u32*)beforeCopy)[0]);
                }
                #endif
                memcpy(orig, beforeCopy, pageSize);
            }
        }
        #ifdef ENABLE_LOGGING
        printf("Rolled back savestate idx %i\n", currentSavestateIdx);
        #endif
        currentSavestateIdx = Wrap(currentSavestateIdx-1, MAX_ROLLBACK_FRAMES);
    }
    currentFrame = rollbackFrame;
}

// a and b are pointers to the data
s32 cmp(void *a, void *b)
{
    void* data1 = *(void**)a;
    void* data2 = *(void**)b;
    if (data1 < data2) return -1;
    else if (data1 > data2) return 1;
    else return 0;
}
s32 binsearch(void* elt, size_t size, const void* arr, size_t length, s32 (*compare)(void*, void*))
{
    size_t i = length / 2;
    char* array = (char*)arr;
    while (i < length)
    {
        s32 comparison = compare(array + i * size, elt);
        if (comparison == 0)
        {
            return i;
        }
        if (comparison < 0)
        {
            i += (length - i + 1) / 2;
        }
        else
        {
            length = i;
            i /= 2;
        }
    }
    return -1;
}
// does this savestate contain region that was just written to? If so, return that region through out
bool DoesSavestateContainCorrespondingUnwrittenRegion(const Savestate& savestate, char* writtenRegion, char*& outBuffer)
{
    PROFILE_FUNCTION();
    int result = binsearch(&writtenRegion, sizeof(writtenRegion), savestate.changedPages, savestate.numChangedPages, cmp);
    if (result == -1) return false;
    void* returnedAddress = savestate.changedPages[result];
    if (returnedAddress != writtenRegion) return false;
    // becuase the savestate's changedPages and data copies indices line up, we can do this
    outBuffer = (char*)savestate.beforeCopies[result];
    return true;
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
        const Savestate& currentSavestate = savestateInfo.savestates[savestateHead];
        if (!currentSavestate.valid) continue;
        char* unwrittenRegion = nullptr;
        if (DoesSavestateContainCorrespondingUnwrittenRegion(currentSavestate, newGameMemWrittenData, unwrittenRegion))
        {
            #ifdef ENABLE_LOGGING
            printf("Found %p while looking for %p  frame %u\n", unwrittenRegion, newGameMemWrittenData, currentSavestate.frame);
            #endif
            // NOTE: returning pointer from our data copies. This is not from gamestate mem
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
    // NOTE: returning pointer from our full snapshot. This is not from gamestate mem
    return unwrittenSnapshotData; 
}


void EvictSavestate(Savestate& savestate)
{
    PROFILE_FUNCTION();
    u32 pageSize = GetPageSize();
    char* fullSnapshot = savestateInfo.fullSnapshot;
    char* gameMem = GetGameState();
    { PROFILE_SCOPE("update full snapshot");
        // when we evict a frame of savestate, by copying that frame's data into our
        // full snapshot, we effectively move our window of frames we can rollback to up by 1
        // TODO: [PERF] can multithread this easily
        for (u32 i = 0; i < savestate.numChangedPages; i++)
        {
            void* orig = savestate.changedPages[i];
            void* data = savestate.afterCopies[i];
            if (orig == nullptr) continue;
            u64 offset = ((u64)orig) - ((u64)gameMem);
            // the "original address" is relative to the game mem. Use that as an offset into our snapshot
            char* snapshotEquivalentAddress = fullSnapshot + offset;
            memcpy(snapshotEquivalentAddress, data, pageSize);
        }
    }

    { PROFILE_SCOPE("copy freeing");
        //  free up all the page snapshots tied to it
        // TODO: [PERF] could also parallelize this?
        for (u32 i = 0; i < savestate.numChangedPages; i++)
        {
            mi_free(savestate.afterCopies[i]);
            mi_free(savestate.beforeCopies[i]);
        }
    }
    { PROFILE_SCOPE("buffer clearing");
        // TODO: these are optional - put them in for sanity for now while things are still in flux
        memset(savestate.beforeCopies, 0, sizeof(void*) * savestate.numChangedPages);
        memset(savestate.afterCopies, 0, sizeof(void*) * savestate.numChangedPages);
        memset(savestate.changedPages, 0, savestate.numChangedPages);
    }
    savestate.valid = false;
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

void OnPagesWritten(Savestate& savestate, u32 savestateHead)
{
    // TODO: parallelize. Only thing that needs special attention for multithreading is the allocation step
    for (u32 i = 0; i < savestate.numChangedPages; i++)
    {
        if (!savestate.valid) continue;
        char* changedGameMemPage = (char*)savestate.changedPages[i];
        char* beforeWrittenSnapshotData = GetUnwrittenPageFromWrittenPage(changedGameMemPage, savestateHead);
        u32 pageSize = GetPageSize();
        Savestate& currentSavestate = savestateInfo.savestates[savestateHead];
        void* copyOfUnWrittenPage = AllocAndCopy(beforeWrittenSnapshotData, pageSize);
        savestate.beforeCopies[i] = copyOfUnWrittenPage;
        void* copyOfWrittenPage = AllocAndCopy(changedGameMemPage, pageSize);
        savestate.afterCopies[i] = copyOfWrittenPage;
    }
}

void SaveWrittenPages(u32 frame)
{
    PROFILE_FUNCTION();
    u32 savestateHead = frame % MAX_ROLLBACK_FRAMES;
    Savestate& savestate = savestateInfo.savestates[savestateHead];
    if (savestate.valid)
    {
        EvictSavestate(savestate);
    }
    savestate.frame = frame;
    savestate.numChangedPages = 0;
    bool success = GetAndResetWrittenPages(savestate.changedPages, &savestate.numChangedPages, MAX_NUM_CHANGED_PAGES);
    assert(success);
    savestate.valid = true;
    OnPagesWritten(savestate, savestateHead);

    #ifdef ENABLE_LOGGING
    u64 numChangedBytes = savestate.numChangedPages * GetPageSize();
    f64 changedMB = numChangedBytes / 1024.0 / 1024.0;
    printf("Frame %i, head = %i\nNum changed pages = %llu\tChanged MB = %f\n", 
            frame, savestateHead, savestate.numChangedPages, changedMB);
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
    // mess with our game memory. This is to simulate the game sim
    GameSimulateFrame(frame, NUM_RANDOM_WRITES_PER_FRAME);
    // whatever changes we made above, save those
    SaveWrittenPages(frame);

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





