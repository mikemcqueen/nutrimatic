read arch/architecture.md and findings/anagram-perf.md.

none of the ideas in anagram-perf are exactly right for my use case i don't think.
but i do want to brainstorm on how i might eliminate (or short circuit) a lot of the
permutations of the same words in different orders.

i don't really care about the processing time of that permutation work honestly, it's
more about the memory consumption with all of those duplicate branches polluting the 
priority queue with what i'm guessing are probably lots of low-score entries that will
just take up lots of memory and possibly result in an OOM before they become the .next()
entry to pop.

one idea is to have a vector<string> of all unique words in the corpse.
and unordered_map<const string&, uint16_t> of those words mapping to their index.
should be less than, what, 65,535 you'd think for english corpus? so a 16-bit unsigned
int could hold the index to a unique word. 

then we have a WordComboIdx struct that is an array of 8 uint16_t's that holds the
sorted indices of the current word sequence being searched.

and a unordered_set<struct WordComboIdx> visited_combos

then, when the searcher maintains a word count if it doesn't already.  when it hits 
the end of word > 1, it sorts the words, builds a WordComboIdx struct, and checks
for its presence in the visited_combos set.  if already present, we've already checked
this combo, bail.  if not present, add to set and continue.

Idea Status: asked and answered.
Result: plans/trie-node-ordered-permutations.md, plans/trie-node-ordering-phase2.md
