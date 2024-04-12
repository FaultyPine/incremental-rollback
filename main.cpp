
#include "util.h"
#include "mem.h"

#define MAX_ROLLBACK_FRAMES 7
#define NUM_TEST_FRAMES_TO_SIMULATE 50

#define GAMESTATE_SIZE MEGABYTES_BYTES(170)
struct Savestate
{
    char** beforeSnapshots = nullptr;
    char** afterSnapshots = nullptr;
};

struct SavestateInfo
{
    char* fullSnapshot = nullptr;
    int savestateHead = 0;
    Savestate savestates[MAX_ROLLBACK_FRAMES] = {};
};  



SavestateInfo savestateInfo = {};
int frame = 0;
char* gameState = nullptr;

void Init()
{
    frame = 0;
    // gamestate is the only memory we want to track
    if (gameState != nullptr)
    {
        TrackedFree(gameState);
    }
    gameState = (char*)TrackedAlloc(GAMESTATE_SIZE);
}

int spotToWrite = GAMESTATE_SIZE / 2; // arbitrary
void RandomWrites()
{
    if (!gameState) return;
    auto start = std::chrono::high_resolution_clock::now();
    // do some random writes to random spots in mem
    int numWrites = 3453;
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
- Get written pages

Rollback:

*/

bool Tick()
{
    printf("---------------------------\n");
    auto start = std::chrono::high_resolution_clock::now();
    if (frame == 0)
    {
        // full snapshot
        if (savestateInfo.fullSnapshot)
        {
            free(savestateInfo.fullSnapshot);
            savestateInfo.fullSnapshot = nullptr;
        }
        savestateInfo.fullSnapshot = (char*)malloc(GAMESTATE_SIZE);
        memcpy(savestateInfo.fullSnapshot, gameState, GAMESTATE_SIZE);
    }

    RandomWrites();
    std::vector<TrackedBuffer> changedPages = GetAndResetWrittenPages();
    size_t numChangedBytes = 0;
    size_t numChangedPages = 0;
    for (const TrackedBuffer& buf : changedPages)
    {
        numChangedBytes += buf.changedPages.Count * buf.changedPages.PageSize;
        numChangedPages += buf.changedPages.Count;
    }
    f64 changedMB = numChangedBytes / 1024.0 / 1024.0;
    printf("Allocation blocks = %llu\nNum changed pages = %llu\nChanged bytes = %llu   MB = %f\n", 
            changedPages.size(), numChangedPages, numChangedBytes, changedMB);

    auto stop = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(stop - start);
    auto timecount = duration.count();
    printf("Frame %i took %llu ms\n", frame, timecount);
    frame++;
    return frame <= 10;
}

int main()
{
    Init();
    while (Tick()) {}
    return 0;
}





