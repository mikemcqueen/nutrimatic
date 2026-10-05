This document is an attempt to describe the workflow for generating BEST PAIRS.

BEST PAIRS are the set of semantically most-meaningful word-pairs for a particular
input text sequence (generally: either a sentence or a sentence fragment), for a
given segment count, which can then be used as an argument to the dfs-anagrams
--pairs option.

Segment count is the number of (index) entries that are combined to form an
anagram. Currently, anagram solutions are scored relative to other anagrams with 
the same segment count - there is no way to compare scores of potential anagram
solutions across different segment counts - so BEST PAIRS must be generated per
segment count.

Broadly, for a particular input and segment count, the process is:

  1. Generate some large number of anagrams with using the top
     10-15% (auto-classified) of all possible pairs that can be made from that
     sentence. 
  2. Determine the top N segments used in the output from 1. These need to be
     manually classified so there is some friction introduced in the process
     here. N=1000 is an easy starting point. Supporting the ability to increment
     this by 1000 at a time is likely going to be helpful.
  3. Manualy classify the N segments from 2 - create notes, manualy check them,
     get and parse results - this is the BEST PAIRS list.
  4. Run dfs-anagrams --pairs BEST PAIRS. This should give the
     most meaningful full anagram results for a particlar input and segment count.
     
A note on manual classification. With 9 sentences, and perhaps 3 segment count
variations on average per sentence, there are potentially 27000 top segments
to manually classify. However, this number could potentially be much lower, if
we take into account that the top segments for segment count 4 and 6, for example,
will likely overlap with the top segments for segment count 5.

So once the top segemnts for segment count 5 have been manually classified, any
of those segments that appear in the top segments for segment count 4 and 6 need
not be re-classified manually.

This will however necessitate both a very critical manual review of top segments
to ensure that any "unchecked" segments never need to be manually reviewed again,
as well as a mechanism for recording "already manually reviewed" (classified as
NO) segments - a planned feature of the workflow tool that is not currently
implemented.

The details:

0. The data dependency for (1) is that we have generated and auto-classified all
   possible pairs for a given sentence. For the full 4-line sentence S1 with min
   word-length 4, for example, there are ~12M pairs, or about 12 days of auto-
   classification runtime. Extrapolating that across all sentences, I expect
   about 45-50M pairs, or 50ish days of auto-classification runtime. The net
   result should be one 85.15.yes pairs file for each sentence.
   
   I am not convinced an over-arching compound workflow command is necessary for
   this process as it is mostly a one-time cost and I think reasonably achieveable
   manually. But I should document the manual process here.
   
   Steps for generating pairs to submit/eval in P1 workflow:

    * build list of 4-letter wiki index pairs:

      `query-index $S2 --wf -w2 -n0 --csv > idx/idx.2.s2.m4`
      
      NOTE: can remove sort (replace with uniq) after pcomm is broadly incorporated into workflow.

      NOTE: this will have reversed entries: "one,two" and "two,one". this might be desirable at
            this stage for p1_done filtering because it's still using comm instead of pcomm. when
            P1 filtering gets upgraded to use pcomm it may not longer be desirable.

    * build list of 4-letter cluer index pairs:
    
      `pairs $S8 [-u subset] | python cluer/query_index.py -f - -j > ../nutrimatic/idx/cluer.s8.m4`

    * making 3-letter word pairs:
    
      NOTE: for cluer, i should build a dictionary that combines words.filtered & a filtered 3-letter
            wordlist (idx/words.3.bowie.filtered is a start).  and use that as pair's --dict.
      old:
      
      index:   `query-index $S9 --wf -w2 -n0 --csv -m3 --dict idx/words.big.l3-10 > idx/idx.2.s9.m3`
      cluer:   `pairs $S9 -m3 | python cluer/query_index.py -f - -j > ../nutrimatic/idx/cluer.s9.m3`
      combine: `pcomm idx/cluer.s9.m3 idx/idx.2.s9.m3 > idx/all.s9.m3`

      updated;

      index:   `query-index $S2 --wf -w2 -n0 --csv -m3 --dict idx/words.big.3.13 > idx/idx.2.s2.m3.3.13`
      # filter out non-cluer-index dict words before sending to pairs
      updated: `time cat idx/words.big.3.13 | ../nutrimatic/dict-filter -d cluer/data/index/tokens.txt - | pairs $S2 -d - -m3 |
                python cluer/query_index.py -f - -j --threads 2 > cluer.s2.m3.3.13`
      combine: `pcomm idx/cluer.s2.m3.3.13 idx/idx.2.s2.m3.3.13 | sort -u > idx/all.s2.m3.3.13`

    * calculate and auto-classify emaining:

       `cat $WFROOT/.wf/p1/done/p1_done.pairs | pcomm -23 idx/all.s9.m3 - > tmp/s9/all.s9.m3.remain`
       
       edit juniper.sh, run it.


    * merge them into final pairs file:

      `pcomm idx/idx.2.s8.m4 idx/cluer.s8.m4 > idx/pairs.s8.m4`

    * TBD: for fine tuning later with specific subsets:

      `python cluer/query_index.py -f ../nutrimatic/results/s7/pairs.big.s7.w.w -j | sort -u  > ../nutrimatic/results/s7/pairs.cluer.s7.w.w`
      `python -m src.filter -y --pm .9 --pr .10 ../nutrimatic/results/s7/pairs.cluer.s7.w.w.p1_done -d $WFROOT/.wf/p1/done/out > ../nutrimatic/results/s7/pairs.cluer.s7.w.w.90.10`
      `python -m src.filter -y --pm .85 --pr .15 ../nutrimatic/results/s7/pairs.cluer.s7.w.w.p1_done.not.in.seed -d $WFROOT/.wf/p1/done/out > ../nutrimatic/results/s7/pairs.cluer.s7.w.w.85.15`


  * filter out already-auto-classified pairs:

      `pcomm -23 idx/pairs.s2.m4 $WFROOT/.wf/p1/done/p1_done.pairs > idx/idx.2.s2.m4.remain`
      
      NOTE:   this is dumb, need a better dedicated tool. need to think about it.
      UPDATE: src.filter has the logic on how to approach this, that we can probably leverage in 
            another tool.  load pairs in canonical form, check for membership in p1_done.pairs.
            or, alternatively, just leverage src.filter itself, and add some freaky new option
            to not filter on --pm/--pm at all, but stream *all* results and return all matches
            whether YES or NO classified. normally you'd expect it to show matches, and -v to
            show non-matches (like grep), but in our case maybe show non-matches by default is
            better.  maybe --matches to "filter (out) matches". not thrilled with that option
            name but that's the gist of it i think.
      UPDATE: pcomm is probably the solution to this.
           
      * if necessary (substantially more than 1M lines):
       `split -n N idx/idx.2.s2.m4.remain idx/idx.2.s2.m4.remain`

    * seems i left out a step here, where i actually submit the .remain to p1 for auto-classification?
      why did I do the filter + split manually here instead of letting p1 handle it?  i do recall
      there being some .enex artifacts in nutrimatic/tmp/ like i was manually downloading notes and
      extracting yes-probs from them, but i can't remember why i would do it that way.
      probably need to dig into the history log to refresh my memory.
      in any case, if i do end up using p1 submit/eval i need to fix it's "done filtering" to be
      smarter than the dumb version it's using, which i think is to use the `pcomm` tool.

   From here, we need to run
     `./wf extract p1 yes --pairs idx/all.s2.m3.3.13 --pm .85 --pr .15 -o idx/seed.s2.m3.all.85.15.pairs`
     `./wf extract p1 yes --pairs idx/all.s2.m3.3.13 --pm .5 --pr .5 -o idx/seed.s2.m3.all.50.pairs`

       input: the original (pre-filtered) pairs file
       output: 15%/50% classified YES results.
       
   That will give us the --pairs file input to step 1.

NEW WAY:

1. Find letter bag subsets for over-represented and/or high-count letters.

    `cat seed.pairs | bag-subset $S9 - -s9 [-u letters] | less`
    
    generally i look for a subset ~400 words or less as that fits into a single p2 review
    note and 
    
2. Find pairs with letters of that subset.

    start with all letters or choose some word/pair you're biased towards as -u, and use the
    solution to or abbreviation of that as a prefix

    e.g., for prefix 'bowie', subset 'llo':

    `cat seed.pairs | pfilter - -s8 -l $S8 -u bowie | grep 'l.*l' | grep 'o' > resutls/s8/pairs/bowie.llo`

2. manual  review

    `./wf review p2 results/s9/pairs/pairs.ooo --as s9.pairs.ooo -s9 --checked no`
    ** manual review **
    `./wf complete p2 s9.pairs.ooo`








OLD WAY:

1. Generate the anagrams:

   `dfs-anagrams $S2 -p 10000000  -n 1000000 --wf --pairs idx/seed.s2.m4.85.15.pairs -g 4 > results/s2/dfs.s2.m4.g4.pairs.1000000`

   * note the -g 4 here.  scores are relative to the number of segments, we'll
     need to run this command for every relevant segment-count per sentence.

   * once again not happy with the output filename losing the 85.15 detail. not
     sure how important that is to maintain.

   * the -n 1000000 is kind of arbitrary. it was "good enough" for a sentence
     of the length of S6, but for S2 it may not be enough. S1 may require more
     than S2.

   The following notes are more about steps 1 & 2 combined, not restricted to step 1:

   * furthermore, a million sounds like a lot, but it may be dominated by common
     (and bad, or wrong) segments. I think in addition to tuning -n to sentence
     length, or perhaps "length of --pairs", or some combination, that steps 1 +
     2 may be a "loop", where I will have to generate the "first pass" anagrams,
     review the top segements, look for high-frequency bad/likely-wrong segements,
     and (manually?) remove them from the --pairs file.
   * in other words, the workflow, as it was originally executed on S6 (which is
     the template for what I'm generalizing here) is not simply a matter of:

     * end up with some *really* good pairs by reviewing the top-segments of
       anagrams generated with *just good* (top 15%) pairs.

     Instead, it's more like:
     
     1. generate what are likely to be good pairs
     2. generate anagrams from those pairs.
     3. generate top segments by frequency from those anagrams
     4. review those top segments. if some high-frequency segments look bad or
        unlikely, we need to remove them from the "good pairs" list and restart
        at step 2.
     
   Personal note, AI can ignore:

   * note to self: s6.idx.m4.pairs.filtered was the name of the file i
       submitted to p1 presumably that was.. comm-filtered using p1/done/p1_done.pairs.
       so the "actual full set" of S6 idx pairs is the non-.filtered version.
       might be able to find that comm call in bash history

2. top 1000 (arbitrary) segments of N (??)

   build/top-segments results/s2/dfs.s2.m4.b1.bw.x2.g4.pairs.1000000 --pairs | head -n 1000 > results/s2/s2.m4.yes.1000

   * not thrilled with the output filename here.  once again losing some detail
     from the source input.

   * see 1) for notes on the potential for a refining "loop" here, eliminating
     bad/likely-wrong top segments.

3. pass those through to p2/yes to create notes; then get & parse YES from notes.
   Need some custom workflow for this probably. I think the core "steps" might
   exist but I am not sure there are "commands" (i.e. "extract") that support 
   this out of the box as it stands.

   * for a in a b c; do note -pf.72 --get s2.idx.m4.1000.yes.filtered.a$a --production > tmp/s2.m4.1000.yes.a$a; done
   * for a in a b c; do note --parse-file tmp/s2.m4.1000.yes.a$a --type yes --lines >> tmp/s2.m4.1000.p2.yes ; done

4. use that newly generated p2.yes as the new --pairs input

   * build/dfs-anagrams $IDX $S2 -m 4 -S 20 -p 10000000 -n 1000000 --word-bonus 1 --dict tmp/words.big --pairs tmp/s2.m4.1000.p2.yes -x 2 -g 4 > results/s2/dfs.s2.m4.b1.bw.x2.g4.yes.1000000
