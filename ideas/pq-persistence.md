## Idea

I had and idea that maybe I want to persist the lower-half of the priority queue
when memory pressure hits.

This idea isn't completely fleshed out.  It'd be something like, persist the bottom
half, remember the topmost score, keep processing the PQ until an entry is reached with 
score < top persisted score, at which point we have to merge back in all or some top
chunk of the persisted PQ.  If multiple bottom-half chunks have been persisted, they'd
presumably need to be merged before any restoration.

Can you validate, brainstorm, analyze, flesh out, and criticize this idea for me?

Also: I could persist it to disk, but I could also persist to GPU memory, if that's
faster, per findings/gpu-memory.md

## Finding
