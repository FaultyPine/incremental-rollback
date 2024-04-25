
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
