
#include "util.h"
#include "mem.h"
#include "profiler.h"

#include <set>

#include "external/mimalloc/src/static.c"

#define MAX_ROLLBACK_FRAMES 7
#define MAX_SAVESTATES (MAX_ROLLBACK_FRAMES+1)
#define NUM_TEST_FRAMES_TO_SIMULATE 20
#define GAMESTATE_SIZE MEGABYTES_BYTES(170)

#ifdef DEBUG
#define ENABLE_LOGGING
#endif
// should be ~1500 for real perf testing
constexpr u32 NUM_RANDOM_WRITES_PER_FRAME = 1500;


// FUTURE: go faster than memcpy - https://squadrick.dev/journal/going-faster-than-memcpy.html
// we have very specific restrictions on the blocks of mem we move around
// always page-sized, and pages are always aligned... surely there's some wins there. Also very easy to parallelize

// TODO:
// - THERE IS NO RESON TO HAVE BEFORESNAPSHOTS!!!!!
//      THE BEFORESNAPSHOTS FOR FRAME X WILL ALWAYS BE THE SAME AS THE AFTER SNAPSHOTS FOR FRAME X-1
//      Just take after snapshots, when rolling back, just rollback 1 extra frame. Since in this case
//      A snapshot taken DURING frame X will be an "after" snapshot, meaning it is AFTER frame X's logic has run.
//      so in a sense, it's really for frame X+1. I.E. if your on frame 10 and want to rollback to frame 5,
//      you'd actually roll back to frame 4, since the "end" of frame 4 is the same as the "beginning" of frame 5.
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
    // page-sized blocks of memory that contains data after this frame wrote to the pages
    void* afterCopies[MAX_NUM_CHANGED_PAGES] = {};
    u64 numChangedPages = 0;
    u32 frame = 0;
    bool valid = false;
};

struct SavestateInfo
{
    char* fullSnapshot = nullptr;
    Savestate savestates[MAX_SAVESTATES] = {};
};  


SavestateInfo savestateInfo = {};
char* gameState = nullptr;

inline char* GetGameState() { return gameState; }
inline u32* GetGameMemFrame() { return (u32*)gameState; } // first 4 bytes of game mem are the current frame

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
    assert(IS_ALIGNED(gameState, 32)); // for simd memcpy, need to be 32 byte aligned
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
        *(u32*)pageAligned = currentFrame;
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

static void RollbackSavestate(const Savestate& savestate)
{
    char* gameMem = GetGameState();
    u32 pageSize = GetPageSize();
    for (u32 i = 0; i < savestate.numChangedPages; i++)
    {
        void* orig = savestate.changedPages[i];
        // apply the "after" state of this past frame
        void* ssData = savestate.afterCopies[i];
        if (orig != nullptr)
        {
            #ifdef ENABLE_LOGGING
            assert(orig >= GetGameState() && orig < GetGameState() + GAMESTATE_SIZE);
            // first 4 bytes of game mem contains current frame
            if (orig == gameMem) 
            {
                u32 nowFrame = *GetGameMemFrame();
                printf("rolling back %u -> %u\n", nowFrame, ((u32*)ssData)[0]);
            }
            #endif
            rbMemcpy(orig, ssData, pageSize);
        }
    }
}

void Rollback(s32 currentFrame, s32 rollbackFrame)
{
    PROFILE_FUNCTION();
    if (currentFrame < MAX_SAVESTATES) return;

    // -1 because all savestates are taken after a frame's simulation
    // this means if you want to rollback to frame 5, you'd actually need to restore the data captured on frame 4
    s32 savestateOffset = currentFrame-rollbackFrame-1; 
    assert(rollbackFrame < currentFrame && savestateOffset < MAX_SAVESTATES);
    // -1 because we want to start rolling back on the index before the current frame
    // another -1 because our savestates are for the end of the frame, so need to go back another
    s32 currentSavestateIdx = Wrap(currentFrame-1-1, MAX_SAVESTATES);
    s32 endingSavestateIdx = Wrap(currentSavestateIdx - savestateOffset, MAX_SAVESTATES);
    
    assert(endingSavestateIdx < MAX_SAVESTATES && endingSavestateIdx != currentSavestateIdx);
    #ifdef ENABLE_LOGGING
    printf("Starting at game mem frame %i\n", *GetGameMemFrame());
    printf("Rolling back %i frames from idx %i -> %i | frame %u -> %u\n", 
                        savestateOffset, currentSavestateIdx, endingSavestateIdx, currentFrame, rollbackFrame);
    printf("Savestate frames stored:\n");
    for (u32 i = 0; i < MAX_SAVESTATES; i++)
    {
        printf("| idx %u = frame %u |\t", i, savestateInfo.savestates[i].frame);
    }
    printf("\n");
    #endif

    // given an index into our savestates list, rollback to that.
    // go from the current index, walking backward applying each savestate's "before" snapshots
    // one at a time
    while (currentSavestateIdx != endingSavestateIdx)
    {
        Savestate& savestate = savestateInfo.savestates[currentSavestateIdx];  
        RollbackSavestate(savestate);
        currentSavestateIdx = Wrap(currentSavestateIdx-1, MAX_SAVESTATES);
    }
    // summary - 
    // we are on frame 15, trying to rollback to frame 10 since we didn't get inputs on frame 10, have been predicting, and just got past inputs from 10
    // ---
    // we start at frame 14 because all snapshots are for the end of the frame, so since we're at the beginning of frame 15, we're technically at the end of 14
    // so we go from 14->13 by applying frame 13's changed pages, then 13->12, .... then 11->10. We stop here, but
    // applying 11->10 actually gets us to the end of frame 10/beginning of 11. We want to be at the beginning of 10/end of 9
    // since that's where we need to reapply the new inputs and start resimulating
    // so we do one more at the end of this loop
    assert(savestateInfo.savestates[currentSavestateIdx].frame == rollbackFrame-1);
    RollbackSavestate(savestateInfo.savestates[currentSavestateIdx]);
    assert(*GetGameMemFrame() == rollbackFrame-1);
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
            // apply changes made during this frame - hence the "after"
            void* data = savestate.afterCopies[i];
            assert(orig && data);
            u64 offset = ((u64)orig) - ((u64)gameMem);
            // the "original address" is relative to the game mem. Use that as an offset into our snapshot
            char* snapshotEquivalentAddress = fullSnapshot + offset;
            rbMemcpy(snapshotEquivalentAddress, data, pageSize);
        }
    }

    { PROFILE_SCOPE("copy freeing");
        //  free up all the page snapshots tied to it
        // TODO: [PERF] could also parallelize this?
        for (u32 i = 0; i < savestate.numChangedPages; i++)
        {
            mi_free(savestate.afterCopies[i]);
        }
    }
    { PROFILE_SCOPE("buffer clearing");
        // TODO: these are optional - put them in for sanity for now while things are still in flux
        memset(savestate.afterCopies, 0, sizeof(void*) * savestate.numChangedPages);
        memset(savestate.changedPages, 0, savestate.numChangedPages);
    }
    savestate.valid = false;
}

// PERF: fixed size allocator
// malloc is wicked slow holy shit
void* AllocAndCopy(void* src, u32 size)
{
    PROFILE_FUNCTION();
    void* result = nullptr;
    { PROFILE_SCOPE("malloc");
        result = mi_malloc_aligned(size, 32);
        assert(IS_ALIGNED(result, 32));
    }
    assert(result);
    { PROFILE_SCOPE("memcpy");
        rbMemcpy(result, src, size);
    }
    return result;
}

void OnPagesWritten(Savestate& savestate, u32 savestateHead)
{
    PROFILE_FUNCTION();
    u32 pageSize = GetPageSize();
    Savestate& currentSavestate = savestateInfo.savestates[savestateHead];
    // TODO: parallelize. Only thing that needs special attention for multithreading is the allocation step
    for (u32 i = 0; i < savestate.numChangedPages; i++)
    {
        if (!savestate.valid) continue;
        char* changedGameMemPage = (char*)savestate.changedPages[i];
        assert(changedGameMemPage >= GetGameState() && changedGameMemPage < GetGameState()+GAMESTATE_SIZE);
        void* copyOfWrittenPage = AllocAndCopy(changedGameMemPage, pageSize);
        savestate.afterCopies[i] = copyOfWrittenPage;
        #ifdef ENABLE_LOGGING
        if (changedGameMemPage == GetGameState())
        {
            printf("[head page] internal frames: current = %u\twritten = %u\n", *GetGameMemFrame(), *(u32*)changedGameMemPage);
        }
        #endif
    }
}

void SaveWrittenPages(u32 frame)
{
    PROFILE_FUNCTION();
    u32 savestateHead = frame % MAX_SAVESTATES;
    Savestate& savestate = savestateInfo.savestates[savestateHead];
    if (savestate.valid)
    {
        EvictSavestate(savestate);
    }
    savestate.frame = frame;
    savestate.numChangedPages = 0;
    savestate.valid = GetAndResetWrittenPages(savestate.changedPages, &savestate.numChangedPages, MAX_NUM_CHANGED_PAGES);
    assert(savestate.valid);
    assert(savestate.numChangedPages <= MAX_NUM_CHANGED_PAGES);
    OnPagesWritten(savestate, savestateHead);

    #ifdef ENABLE_LOGGING
    u64 numChangedBytes = savestate.numChangedPages * GetPageSize();
    f64 changedMB = numChangedBytes / 1024.0 / 1024.0;
    printf("Frame %i, head = %i\tNum changed pages = %llu\tChanged MB = %f\n", 
            frame, savestateHead, savestate.numChangedPages, changedMB);
    #endif
}

bool Tick(u32& frame)
{
    PROFILER_FRAME_MARK();
    PROFILE_FUNCTION();
    #ifdef ENABLE_LOGGING
    printf("------------ FRAME %u ---------------\n", frame);
    #endif
    auto start = std::chrono::high_resolution_clock::now();
    if (frame == 0)
    {
        #ifdef ENABLE_LOGGING
        printf("Taking full gamestate snapshot...\n");
        #endif
        // full snapshot
        if (savestateInfo.fullSnapshot)
        {
            mi_free(savestateInfo.fullSnapshot);
            savestateInfo.fullSnapshot = nullptr;
        }
        savestateInfo.fullSnapshot = (char*)mi_malloc(GAMESTATE_SIZE);
        rbMemcpy(savestateInfo.fullSnapshot, GetGameState(), GAMESTATE_SIZE);
        #ifdef ENABLE_LOGGING
        auto stop = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
        auto timecount = duration.count();
        printf("Full snapshot took %llu ms\n", timecount);
        #endif
    }

    constexpr u32 NUM_FRAMES_TO_ROLLBACK = 5;
    if (frame % 15 == 0 && frame > MAX_ROLLBACK_FRAMES)
    {
        // rollback
        s32 frameToRollbackTo = frame - NUM_FRAMES_TO_ROLLBACK;
        // if we're at frame 15, and rollback to frame 10,
        // we end up at the end of frame 9/beginning of frame 10.
        Rollback(frame, frameToRollbackTo);
        #ifdef ENABLE_LOGGING
        printf("Rolled back to frame %i\n", *GetGameMemFrame());
        #endif
        // resim
        // if we're at beginning of frame 10, and need to get to beginning of 15,
        // need to simulate 10->11, 11->12, 12->13, 13->14, 14->15
        for (u32 i = frameToRollbackTo; i < frame; i++)
        {
            GameSimulateFrame(i, NUM_RANDOM_WRITES_PER_FRAME);
            // TODO: add this in since this'll have to happen too. Commented out for now to keep testing other parts of this simpler
            SaveWrittenPages(i);
        }
        #ifdef ENABLE_LOGGING
        printf("---- END Resimulation ----\n");
        #endif
        assert(frame == *GetGameMemFrame()+1); // +1 since we should be at the end of the previous frame/beginning of this one
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





