#include "incremental_rb.h"
#include "util.h"
#include "mem.h"
#include "profiler.h"
#include "tiny_arena.h"
#include "job_system.h"

#include <set>

#include "external/mimalloc/src/static.c"

#define MAX_SAVESTATES (MAX_ROLLBACK_FRAMES+1)
#define MULTITHREAD // to turn on/off multithreading for debugging

constexpr u32 numWorkerThreads = 4;


// FUTURE: go faster than memcpy - https://squadrick.dev/journal/going-faster-than-memcpy.html
// we have very specific restrictions on the blocks of mem we move around
// always page-sized, and pages are always aligned... surely there's some wins there. Also very easy to parallelize


constexpr u64 MAX_NUM_CHANGED_PAGES = 2000;

struct Savestate
{
    // sorted (ascending) list of changed pages
    void* changedPages[MAX_NUM_CHANGED_PAGES] = {}; 
    // page-sized blocks of memory that contains data after this frame wrote to the pages
    void* afterCopies[MAX_NUM_CHANGED_PAGES] = {};
    Arena arena = {};
    u64 numChangedPages = 0;
    u32 frame = 0;
    bool valid = false;
};

struct SavestateInfo
{
    Savestate savestates[MAX_SAVESTATES] = {};
};  


SavestateInfo savestateInfo = {};
static jobsystem::context jobctx;

static IncrementalRBCallbacks cbs = {};

inline char* GetGameState() 
{
    if (cbs.getGameState)
    {
        return cbs.getGameState();
    }
    return nullptr; 
}
inline u64 GetGamestateSize() 
{ 
    if (cbs.getGamestateSize)
    {
        cbs.getGamestateSize();
    }
    return 0; 
}
inline u32* GetGameMemFrame() 
{ 
    if (cbs.getGameMemFrame)
    {
        return cbs.getGameMemFrame();
    }
    return nullptr; 
}



void GameSimulateFrame(u32 currentFrame, u32 numWrites);

void Init(IncrementalRBCallbacks cb)
{
    PROFILE_FUNCTION();
    cbs = cb;
    jobsystem::Initialize(numWorkerThreads-1); // -1 because when we do our async and join stuff, main thread also becomes a worker
    assert(IS_ALIGNED(GetGameState(), 32)); // for simd memcpy, need to be 32 byte aligned
    ResetWrittenPages(); // reset written pages since this is supposed to be initial state

    // allocate mem for savestates
    u64 savestateMemSize = MAX_NUM_CHANGED_PAGES * GetPageSize();
    for (u32 i = 0; i < ARRAY_SIZE(savestateInfo.savestates); i++)
    {
        Savestate& savestate = savestateInfo.savestates[i];
        void* backingMem = mi_malloc_aligned(savestateMemSize, 32);
        assert(IS_ALIGNED(backingMem, 32));
        savestate.arena = arena_init(backingMem, savestateMemSize);
    }
}

s32 Wrap(s32 x, s32 wrap)
{
    if (x < 0) x = (wrap + x);
    return abs(x) % wrap;
}

static void RollbackSavestate(const Savestate& savestate)
{
    PROFILE_FUNCTION();
    char* gameMem = GetGameState();
    u32 pageSize = GetPageSize();
    #ifdef MULTITHREAD
    u32 pagesPerThread = savestate.numChangedPages / numWorkerThreads;
    for (u32 i = 0; i < numWorkerThreads; i++)
    {
        u32 pageOffset = i * pagesPerThread;
        jobsystem::Execute(jobctx, 
            [pageOffset, pagesPerThread, &savestate, pageSize, gameMem, i](jobsystem::JobArgs args)
            {
                PROFILE_FUNCTION();
                u32 endPageIdx = pageOffset + pagesPerThread;
                for (u32 pageIdx = pageOffset; pageIdx < endPageIdx; pageIdx++)
                {
                    void* orig = savestate.changedPages[pageIdx];
                    void* ssData = savestate.afterCopies[pageIdx];
                    #ifdef ENABLE_LOGGING
                    assert(orig >= GetGameState() && orig < GetGameState() + GetGamestateSize());
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
        );
    }
    if (numWorkerThreads % 2 != 0)
    {
        // odd number of worker threads means we can't evenly split up the work, so do the last bit here
        u32 pageIdx = savestate.numChangedPages-1;
        void* orig = savestate.changedPages[pageIdx];
        void* ssData = savestate.afterCopies[pageIdx];
        rbMemcpy(orig, ssData, pageSize);
    }
    jobsystem::Wait(jobctx);
    #else
    for (u32 i = 0; i < savestate.numChangedPages; i++)
    {
        PROFILE_SCOPE("rollback page");
        // apply the "after" state of this past frame
        // changedPages[i] will always correspond to the same index in afterCopies
        void* orig = savestate.changedPages[i];
        void* ssData = savestate.afterCopies[i];
        #ifdef ENABLE_LOGGING
        assert(orig >= GetGameState() && orig < GetGameState() + GetGamestateSize());
        // first 4 bytes of game mem contains current frame
        if (orig == gameMem) 
        {
            u32 nowFrame = *GetGameMemFrame();
            printf("rolling back %u -> %u\n", nowFrame, ((u32*)ssData)[0]);
        }
        #endif
        rbMemcpy(orig, ssData, pageSize);
    }
    #endif
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
    // free up all the page snapshots tied to it
    arena_clear(&savestate.arena);
    // NOTE: we *do* rely on nullptrs in afterCopies indicating an unwritten/evicted savestate
    // so that when we are resimulating, we don't have to reallocate
    memset(savestate.afterCopies, 0, sizeof(void*) * savestate.numChangedPages);
    memset(savestate.changedPages, 0, savestate.numChangedPages); // optional
    savestate.valid = false;
}


void OnPagesWritten(Savestate& savestate)
{
    PROFILE_FUNCTION();
    u32 pageSize = GetPageSize();
    // for parallelization, need to do the allocation stuff first since this needs to be serial. Arenas are not threadsafe
    for (u32 i = 0; i < savestate.numChangedPages; i++)
    {
        void* copyOfWrittenPage = savestate.afterCopies[i];
        // during resim frames, our afterCopies won't have been cleared to 0, so we don't need to realloc there
        // since while resimulating, we are just overwriting the contents of past frames with our new resim-ed stuff
        if (copyOfWrittenPage == nullptr)
        {
            copyOfWrittenPage = arena_alloc(&savestate.arena, pageSize);
        }
        savestate.afterCopies[i] = copyOfWrittenPage;
    }
    #ifdef MULTITHREAD
    u32 pagesPerThread = savestate.numChangedPages / numWorkerThreads;
    for (u32 i = 0; i < numWorkerThreads; i++)
    {
        u32 pageOffset = i * pagesPerThread;
        jobsystem::Execute(jobctx, [pageOffset, pagesPerThread, pageSize, &savestate](jobsystem::JobArgs args){
            PROFILE_FUNCTION();
            // if numWorkerThreads is 3, we do pages in chunks like this: [0,333), [333, 666), [666, 999)
            u32 endPageIdx = pageOffset + pagesPerThread;
            for (u32 pageIdx = pageOffset; pageIdx < endPageIdx; pageIdx++)
            {
                char* changedGameMemPage = (char*)savestate.changedPages[pageIdx];
                assert(changedGameMemPage >= GetGameState() && changedGameMemPage < GetGameState()+GetGamestateSize());
                rbMemcpy(savestate.afterCopies[pageIdx], changedGameMemPage, pageSize);
            }
        });
    }
    if (numWorkerThreads % 2 != 0)
    {
        // odd number of worker threads means we can't evenly split up the work, so do the last bit here
        u32 pageIdx = savestate.numChangedPages-1;
        char* changedGameMemPage = (char*)savestate.changedPages[pageIdx];
        rbMemcpy(savestate.afterCopies[pageIdx], changedGameMemPage, pageSize);
    }
    jobsystem::Wait(jobctx);
    #else
    for (u32 i = 0; i < savestate.numChangedPages; i++)
    {
        PROFILE_SCOPE("save page");
        char* changedGameMemPage = (char*)savestate.changedPages[i];
        assert(changedGameMemPage >= GetGameState() && changedGameMemPage < GetGameState()+GetGamestateSize());
        rbMemcpy(savestate.afterCopies[i], changedGameMemPage, pageSize);
        #ifdef ENABLE_LOGGING
        if (changedGameMemPage == GetGameState())
        {
            printf("[head page] internal frames: current = %u\twritten = %u\n", *GetGameMemFrame(), *(u32*)changedGameMemPage);
        }
        #endif
    }
    #endif
}

void SaveWrittenPages(u32 frame, bool isResim)
{
    PROFILE_FUNCTION();
    u32 savestateHead = frame % MAX_SAVESTATES;
    Savestate& savestate = savestateInfo.savestates[savestateHead];
    if (savestate.valid && !isResim)
    {
        // only evict old savestates when we're simulating/saving the current frame
        EvictSavestate(savestate);
    }
    savestate.frame = frame;
    savestate.numChangedPages = 0;
    savestate.valid = GetAndResetWrittenPages(savestate.changedPages, &savestate.numChangedPages, MAX_NUM_CHANGED_PAGES);
    assert(savestate.valid);
    assert(savestate.numChangedPages <= MAX_NUM_CHANGED_PAGES);
    OnPagesWritten(savestate);

    #ifdef ENABLE_LOGGING
    u64 numChangedBytes = savestate.numChangedPages * GetPageSize();
    f64 changedMB = numChangedBytes / 1024.0 / 1024.0;
    printf("Frame %i, head = %i\tNum changed pages = %llu\tChanged MB = %f\n", 
            frame, savestateHead, savestate.numChangedPages, changedMB);
    #endif
}

void OnFrameEnd(s32 frame, bool isResim)
{
    SaveWrittenPages(frame, isResim);
}

void Shutdown()
{
    jobsystem::ShutDown();
}

