given a list of pairs, find the least common words in the wiki index:

  `cat tmp/s1/glllpu/1/all.pairs | top-segments  -n0 --pair-words --nc - | query-index - --filter --max-score 1500 | wc -l`

given a list of pairs, find the un-indexed words in the cluer index:
  NOTE: there may be value in finding "least common" in cluer index as well, i don't think cluer/query_index supports that yet.

  `cat tmp/s1/glllpu/1/all.pairs | top-segments  -n0 --pair-words --nc - | qi -f - -v | wc -l`

-------

letter-pair-words.sh -s N [--pm P] OLD-BAG NEW-BAG LETTERS...
letter-pair-words.sh -s N [--pm P] -l BAG -u USED -U NEW_USED LETTERS...

For each letter, it lists the words from seed pairs that fit NEW-BAG but not OLD-BAG, where
both words in the pair contain that letter, less words that already have a verdict (YES/NO
words, or in a YES pair). That's the same as:

  `seed-pairs | pfilter | grep | tr , '\n' | sort -u` pipeline, followed by
  comm and `filter-words -m 0`.

Example:

`./letter-pair-words.sh -s 1 $S1MIN4 $S1MIN5 h`

NOTE: i should probably convert this to c++.


---------

shows the whole table with word and pair counts for each letter, e.g.:

                            [SMAL]  [BIG]
`./letter-pair-diff.sh -s 1 $S1MIN4 $S1MIN5 h w d y m`


---------

see remaining pair counts (mabye letter-pair-diff does this), e.g.:


`seed-pairs -s1 -m4 | pfilter -s1 -l $S1MIN3 - -d $DICT | bag-subset $S1MIN3 - -l3 > tmp/s1/bs.s1min3.m4`


-----------

this was an attempt at taking the words from the aggregate of 'dd' 'mm' 'nnn', 'rrr', etc., pairs
using $S1 -u gllpu, comm | sort -u'ing them, and then wf reviewing the words with a query-index score
below 1500.

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
