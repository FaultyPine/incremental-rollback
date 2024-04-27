
#include "util.h"
#include "incremental_rb.h"
#include "mem.h"
#include "profiler.h"

#include <set>

#define NUM_TEST_FRAMES_TO_SIMULATE 100
#define GAMESTATE_SIZE MEGABYTES_BYTES(170)
constexpr u32 NUM_RANDOM_WRITES_PER_FRAME = 1500;
constexpr u32 NUM_FRAMES_TO_ROLLBACK = MAX_ROLLBACK_FRAMES;

char* gameState = nullptr;

inline char* GetGameState() { return gameState; }
inline u64 GetGamestateSize() { return GAMESTATE_SIZE; }
inline u32* GetGameMemFrame() { return (u32*)gameState; } // first 4 bytes of game mem are the current frame


void GameSimulateFrame(u32 currentFrame, u32 numWrites)
{
    PROFILE_FUNCTION();
    u64 spotToWrite = GetGamestateSize() / 2; // arbitrary byte index into game mem block
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
        spotToWrite = spotToWrite % GetGamestateSize();
        *(u32*)(&gameMem[spotToWrite]) = spotToWrite; // do some arbitrary write
    }
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    auto timecount = duration.count();
    #ifdef ENABLE_LOGGING
    printf("GameSimulateFrame %llu ms [frame %i] [wrote %llu pages]\n", timecount, currentFrame, writtenPages.size());
    #endif
}


void Tick(s32 frame)
{
    PROFILER_FRAME_MARK();
    PROFILE_FUNCTION();
    #ifdef ENABLE_LOGGING
    printf("------------ FRAME %i ---------------\n", frame);
    #endif
    auto start = std::chrono::high_resolution_clock::now();

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
        for (s32 i = frameToRollbackTo; i < frame; i++)
        {
            GameSimulateFrame(i, NUM_RANDOM_WRITES_PER_FRAME);
            OnFrameEnd(i, true);
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
    OnFrameEnd(frame, false);

    #ifdef ENABLE_LOGGING
    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    auto timecount = duration.count();
    printf("Frame %i took %llu ms\n", frame, timecount);
    #endif
}


u32 frame = 0;
int main()
{
    frame = 0;
    gameState = (char*)TrackedAlloc(GAMESTATE_SIZE);
    IncrementalRBCallbacks cbs;
    cbs.getGamestateSize = GetGamestateSize;
    cbs.getGameState = GetGameState;
    cbs.getGameMemFrame = GetGameMemFrame;
    Init(cbs);
    while (frame < NUM_TEST_FRAMES_TO_SIMULATE) 
    {
        Tick(frame++);
    }
    Shutdown();
    #ifdef _WIN32
    system("pause");
    #endif
    return 0;
}





