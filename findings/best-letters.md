ok so i documented the docs/best-pairs-workflow-v2.md and planned it
plans/integrate-best-pairs.md and actually implemented it.

i left out one very important thing though. the number of letters in
a typical sentence is too many, generally speaking, for the dfs-anagrams
and top-segments combination to generate useful candidate anagrams.

instead, in most cases, the BEST workflows will operate with a subset
of the letters of a sentence, specified either directly as a subset of
"use only these letters", which will be passed as the 2nd positional 
argument to dfs-anagrams, or as set of "used letters" which are subtracted
from the sentence letters, and which will pass through as the -u option
to dfs-anagrams.

as entire workflows will be run with these use-only/use-all-except letters
as part of the "context" of the workflow conceptually, i think i might need
to add another layer in the best/ directory structure to take these into
account.

i'm thinking something like:

  best/
    s2/
      letters
      seed.m4.idx2.85.15.pairs
      u-someletters/
        m4/
          g4/
            dfs.seed -> /home/mike/nutrimatic/results/s2/dfs.s2.idx2.85.15.m4.x2.g4.1000000.u-someletters
            ...same as current...
          g5/
            ...
      o-someletters
        m4/
          g4/
            ...
          g5/
            ...
            
the main ideas being:

* "someletters" is a string of letters that is a subset of s2/letters,
  indicating o(nly)- or u(sed)- letters.
* the m4 seed pairs file is the same for *all* m4 workflows. so we
  only need one per min-word-length per sentence.
* each u- or o- letters workflow will support different min-word-lengths
  (mN), and segment counts (gN).
* the generated dfs.seed and dfs.best filenames will need to be
  annotated to include the o-/u-letters

Additional notes:

1. I am using two axes o- and u- for human legibility. the 'someletters'
   component will typically be a human-readable form (such as combined
   words, e.g.: thisandthat) and that human readability is critical for
   identifying previous work and work in progress via best status, as well
   as locating and interpreting the dfs result files.
   Said another way, I cannot normalize to u- for all cases, using sorted
   "used" letters as that wouldn't be comprehensible to me.
   There should be a check though when a workflow with a new o- or u- letter
   set is created, to ensure it is not equal to existing u- or o- letter set.
2. I am putting the o- and u- directories at the top of the hierarchy, 
   above m4/, because that describes the root of the worfklow which is
   based on letter sets, and thus is the natural organizational structure
   for all work done within that workflow, regardless of min-word-lenght,
   and will help me the human see what work has been done for a paritcular 
   letter set.
   The fact that the m4 seed pairs sits above the m4/ of various letter
   sets might be a little *awkward* organizationally but it's not a deal
   breaker.  letter sets for each min-word-length are considered "global"
   for a sentence.
3. Ignore the empty o-/u- letterset for now.  i can imagine a u-nil or u-none
   in the future if necessary though.
5. best.pairs still exists in each leaf directory, and are specific to each 
   sentence/letters-set/m/g/ context. within each context they are independent
   from all other contexts.
   
  
