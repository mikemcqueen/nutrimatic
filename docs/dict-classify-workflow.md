given a list of pairs, find the least common words in the wiki index:

  `cat tmp/s1/glllpu/1/all.pairs | top-segments  -n0 --pair-words --nc - | query-index - --filter --max-score 1500 | wc -l`

given a list of pairs, find the un-indexed words in the cluer index:
  NOTE: there may be value in finding "least common" in cluer index as well, i don't think cluer/query_index supports that yet.

  `cat tmp/s1/glllpu/1/all.pairs | top-segments  -n0 --pair-words --nc - | qi -f - -v | wc -l`

-------

This method isn't as important anymore now that I have best-subsets.sh, which
changes how i choose bag subsets entirely, making all of this subset-comparison
perhaps less important. although it may still have some value when i choose to
"upgrade" from one subset to the next, which I may wish to base on the *relative*
difference of pair count from OLD to NEW set.

old way:

# note that pfilter --ts doesn't exist anymore.  top-segments auto-dectets and accepts "comma-separated pairs" as input.

# pw() { seed-pairs -s "$1" | pfilter -s "$1" -d "$DICT" -m 0 -x 0 -l "$2" --ts - | grep "${3}.* .*${3}" | top-segments --nc --pair-words -n0 2>/dev/null | sort -u; }

# calling it, e.g:

# for a in h ; do comm -23 <(pw 1 "$S1MIN5" "${a}") <(pw 1 "$S1MIN4" "${a}") | less; done

new way:

letter-pair-words.sh -s N [--pm P] OLD-BAG NEW-BAG LETTERS...
letter-pair-words.sh -s N [--pm P] -l BAG -u USED -U NEW_USED LETTERS...

For each letter, it lists the words from seed pairs that fit NEW-BAG but not OLD-BAG, where
both words in the pair contain that letter. That's the same as:

  `seed-pairs | pfilter | grep | top-segments --pair-words | sort -u` pipeline as in pw, followed by comm.

Example:

`./letter-pair-words.sh -s 1 $S1MIN4 $S1MIN5 h`


---------

shows the whole table with word and pair counts for each letter, e.g.:

                            [SMAL]  [BIG]
`./letter-pair-diff.sh -s 1 $S1MIN4 $S1MIN5 h w d y m`


---------

see remaining pair counts (mabye letter-pair-diff does this), e.g.:


`seed-pairs -s1 -m4 | pfilter -s1 -l $S1MIN3 - -d $DICT | bag-subset $S1MIN3 - -l3 > tmp/s1/bs.s1min3.m4`


-----------

query-index --score --max-score 1500 on a "diff" of $S1,$S1-gllpu i think

before qi.1500

seed-pairs -s1 -m4 | pfilter -s1 -l $S1 - -d $DICT -u glllpu | wc -l
23203
seed-pairs -s1 -m4 | pfilter -s1 -l $S1 - -d $DICT -u glllpu | top-segments --nc -n0 --pair-words | wc -l
4407

after qi.1500

23167
4330

conclusion: qi --max-score 1500 not really worth it. most of those words probably only matched one pair.
