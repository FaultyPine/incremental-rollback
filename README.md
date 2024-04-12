
This repo houses an experimental solution to the Brawlback problem.
The problem is that to do rollback netcode, we need to save and restore the game state frequently.
We can do this for games like Smash Melee easily because the gamecube/game only really has 8mb of relevant gamestate.
Modern computers can copy 8mb easily and stay within 60fps frame budgets. For the wii though, we're looking at around 10x that.
80mb is a bit conservative... Modern computers cannot copy that much memory around multiple times a frame and stay within frame budgets.
So this is an idea to get around that limitation.

Brawlback idea:
MMIO chips track dirty pages. We can mark allocations to be "tracked", and at any point we can query an allocation
and see which pages of memory were written to.
problem with tracking written mem regions. Mem from frame X-1 that has been written to is lost. Need a way to actually rollback
Frame 0, so take full snapshot of EVERYTHING
End of frame 1, we get written pages. Save those new pages that have our "written" data. ALSO save those page regions from our frame 0 full snapshot.
	This way, we can go from frame 1 -> 0 and 0 -> 1.
End of frame 2, we get written pages. Save new pages. Look at those written regions, see if those regions are part of our frame 1 written regions. If so, we can get previous frame's data through frame 1's saved pages
	If any region isn't part of frame 1's written regions, we look at the next frame from the past, in this case frame 0. If that contains the regions we get the mem from there.
	(future) could also make some sort of tree structure to partition the address space. So when we need to find mem region from previous frame we don't have to search all of them, we can sort of bsearch the written pages
	(future) since pages are a fixed size, could also use a custom fixed size allocator to manage them so we don't have loads of heap pointers to all these allocs
We keep a ring buffer of a few frames worth of these before/after written regions as well as our frame 0 full snapshot.


Problem: what if frame 1 writes to address 0x10. We save the before/after because we have our full snapshot from frame 0. 
Then we advance a few frames and discard frame 1's data since it goes out of our ring buffer
Then on some future frame we again write to 0x10, and the mem region falls all the way back to our full snapshot.
The problem is that snapshot is from frame 0 and doesn't contain the frame 1 0x10 write. 
SO the SOLUTION: When we discard frames out the end of the ring buffer, we need to write their "after" data into our
full snapshot. This way our snapshot always contains a full snapshot of memory for frame (current frame - ring buffer size).
Keep in mind the ring buffer size is also the max number of frames we can rollback (any more out of sync and we do a total stall)
So updating the full snapshot with the oldest frame means when we need to rollback regions that fall all the way back to the snapshot,
it'll always be properly updated.
