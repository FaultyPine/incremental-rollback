
This repo houses an experimental solution to the Brawlback problem.
The problem is that to do rollback netcode, we need to save and restore the game state frequently.
We can do this for games like Smash Melee easily because the gamecube/game only really has 8mb of memory.
Modern computers can copy 8mb easily and stay within 60fps frame budgets. For the wii though, we're looking at around 10x that.
80mb is a bit conservative... Modern computers cannot copy that much memory around multiple times a frame and stay within frame budgets.

# Concept
Exploiting the fact that modern MMUs have "dirty bits" in their pages, and that we can access those (albiet in a very limited fashion) in usermode apps, we can track what pages of memory have been written to each frame.
We can save those written pages off each frame. When we rollback, we apply those written pages like a "frame delta".
So we can get from frame X to frame X-1 by applying the written changes from the past into our current game memory.
Those written pages represent the "end" of a given frame. Meaning the changed pages of frame X represent the *beginning* of frame X and the *end* of frame X-1. 
This introduces a slight hiccup in that if we store 5 frames worth of savestates, and want to rollback as far in the past as we can - that being to frame X-5 - we cannot, because frame X-5's savestate is for the *end* of X-5 (or synonymously, the *beginning* of X-4). So we simply store 1 extra savestate than the number of frames we plan to ever rollback to, so if we need to get to X-5, we can use the savestate from X-6.

# Performance
Brawl/Project+ - the target of this software, runs on the wii emulator Dolphin.
I tested the performance of this with a few steps described below:
First, I replaced Dolphin's allocation logic with a single call to allocate a big block of (tracked) memory for anything dolphin needs. Dolphin internally requires ~170mb of memory which consists of fake vmem, l1 cache, and mem1/mem2 regions for wii/gamecube memory. It also uses a bit more memory for it's custom "fastmem" implementation, but I left those out of the test for now - wasn't sure if that should be included in the rollback-ed memory regions.
Now that all of dolphin's memory was tracked, I ran an instance of the game (brawl/project+) to see how many actual pages of memory the game writes to every frame. I saw ~1500 pages was the average in any typical 1v1 match. 
I used this info to test here by allocating a similar 170mb tracked region and writing into that region at ~1500 locations in memory making sure they were all in different pages.
Perf tests on this gave results of ~1 milisecond to save the state of the game on a normal frame (more than half of that time is spent in the windows GetWriteWatch api call where they fetch dirty pages). 
So non-rollback frames are fine, but the real kicker for performance is in frames when we need to rollback the state of the game and resimulate it.
Rolling back 7 frames, and resimulating them while re-saving those frames took ~16ms avg. 
This ~16ms is from the following:
~3ms to rollback from frame X to frame X-7.
avg ~1ms per resimulation frame to "simulate" a game frame (will be a lot slower with actual Brawl... brawl frames take ~3-4ms avg)
+
avg ~1ms per resimulation frame to save the new "frame delta" (aka savestate)

To see some of the profiling traces I took, go to https://tracy.nereid.pl/
Click the red Power button in the top-left, then "Open saved trace", then (download and then) select any of the traces in the `traces/` folder. The frames with abnormally long `Tick` functions are the ones where rollbacks happened.

While this might not be the fastest save/load state situation we could be in - I think the importance of this technique comes from the fact that we are not hardcoding regions of memory that we deem as "relevant gamestate". 
With this method - in theory - since we're rolling back/saving **all** regions of memory that change, there would be no concerns about desyncs, about savestates not accounting for all the relevant gamestate, or anything like that.
