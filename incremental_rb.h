#pragma once

#include "util.h"

#ifdef DEBUG
#define ENABLE_LOGGING
#endif

#define MAX_ROLLBACK_FRAMES 7

// size of gamestate mem block
typedef u64(*GetGamestateMemSizeCb)();
// gamestate mem block
typedef char*(*GetGameStateCb)();
// [optional - for debugging] internal game frame derived from game memory, not our tracked frame 
// we use this for asserts and stuff to make sure the internal game frame/data is on the frame we expect it to be
// this location in memory should be part of the tracked allocation and should be written to every frame
typedef u32*(*GetGameMemFrameCb)();

struct IncrementalRBCallbacks
{
    GetGamestateMemSizeCb getGamestateSize = nullptr;
    GetGameStateCb getGameState = nullptr;
    GetGameMemFrameCb getGameMemFrame = nullptr;
};

void Init(IncrementalRBCallbacks cb);
void OnFrameEnd(s32 frame, bool isResim);
void Shutdown();

void Rollback(s32 currentFrame, s32 rollbackFrame);
