what wf should remember

The workflow should retain state with this directory structure

.wf/best/
  idx/
    wiki-merged.2.index # soft link but that's opaque to the tool

  dict/
    words.big # link or copy of: words.big
  
  s2/
    letters # contains $S2

    m4/
      seed.pairs

      g4/
        state
        top.segments.N
        best.pairs.N
    
      g5/
        state
        top.segments.N
        best.pairs.N


thoughts:
  * one thing i didn't really express well when making the workflow doc was
    that i don't think i necessarily need to keep a history of all the "phases"
    or "iterations of the refinement loop" that i've been through. i want to
    know where i am in a loop, if i'm in a loop, and what the next step is if
    i want to start or continue a loop, but i don't... (TB continued..)

  * introduce an ordered list of states and each best pairs folder has a file
    which contains the last state executed.  something like:

   state: command to advance
   -------------------------

   "dfs_seed":   "generate dfs seed",  # call dfs-anagrams with --pairs seed.pairs
    
   "seg_top": "generate top",  # top N generation needed.  wf generate s2 m4 g4 top [default=1000] to advance
              # does it automatically create notes to review or is a subsequent review
              # command necessary?

   "seg_review":  "complete? top",  # download/extract notes, save checked notes to best.pairs
                                    # is it additive? destructive/overwrite?
                                    # does it automatically generate best? or separate command?
      
   "dfs_best":   "generate dfs best"   # call dfs-anagrams with --pairs best.pairs


   * best.pairs.N must be built as common(top.segments.N, classified/yes/yes.pairs),
     not as this batch's *.p2.yes. Otherwise a pair confirmed while reviewing g4 is
     filtered out of g5's review and silently missing from g5's BEST PAIRS. Built
     as an intersection, it's overwrite-safe by construction — a pure function of
     two files, regenerable in milliseconds. So: destructive overwrite, and it doesn't matter.

   * we could determine where the most recent work was done by timestamp of state
   * we could determine if a top.segments or best.pairs (or seed.pairs) is outdated
     by timestamp relative to classified/no_pairs
     
