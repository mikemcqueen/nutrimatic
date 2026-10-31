## Universe dictionary

idx/universe

the sorted union of idx/words, $BIG and $DICT, keeping words of 3 to 13
letters. every pair file below is built over it. written by
`make -f rebuild.mk universe` or `rebuild.sh universe`.


## Index pairs

idx/idx.2.sN

pairs present in wiki-merged.2.index for SN, words of 3 or more letters in
the universe dictionary.


## Cluer pairs

idx/cluer.sN

pairs of universe words in the cluer index's tokens.txt that the cluer
index finds for SN, words of 3 or more letters.


## Extra pairs

idx/sN.pairs

hand-added pairs for SN, optional. folded into pairs.sN when present, and
forced into the seed views of pairs.sN (see Seed pairs). keeping their
words in the dictionaries is up to whoever adds them.


## Required words

idx/sN.required

words for SN, one per line, optional; lines with '-' are skipped. pgui's
rw keeps only pairs with at least one of them, and is greyed out while the
file is missing or has no words. pgui rereads it on a sentence change or
Enter in the seed field, dropping the words it has when the file goes
missing or can't be read. `pfilter --rw FILE` takes the same format.


## Pair file

idx/pairs.sN

the one pair file per sentence: the index, cluer and extra pairs for SN,
deduplicated. written by `make -f rebuild.mk S=N` or `rebuild.sh sN`.

removing dictionary words doesn't rebuild it; the seed filters drop those
pairs when it is read. adding words to the dictionary, or changing SN's
letters, needs a rebuild (`-B universe wiki cluer combine`, or `-f`).


## Subset word lists

idx/words.sN.NAME

words for SN, one per line, optional; any number of them, NAME telling
them apart (e.g. pb2.top2k). made by hand, e.g. from top-segments
--pair-words over bag-stats output.


## Subset pair files

idx/pairs.sN.NAME

the pairs of pairs.sN whose words are both in words.sN.NAME, one file per
word list. written by `make -f rebuild.mk subsets S=N` or `rebuild.sh
subsets sN`, and by `all`; rebuilt when pairs.sN or the word list changes.
pgui opens one as it does pairs.sN (`pgui idx/pairs.s1.pb2.top2k`), taking
the sentence from its second dot-separated piece.


## Phase 1 index

idx/p1.idx

every phase 1 verdict in $WFROOT/.wf/p1/done/out/*.jsonl, each word order's
YES probability, written by `make-p1-index -o idx/p1.idx`. new phase 1
results take effect after it is rebuilt.


## Seed pairs

not a file: a view of a pair file (pairs.sN by default), filtered when it
is read.

`seed-pairs [-s N] [--pairs PAIRS] [--p1 INDEX] [--pm P] [-m N] [-x N] [-d FILE]`

-s N (1 to 9) picks sentence N, which does two things: the pairs of
idx/sN.pairs, when it exists, are forced in, and the pair file defaults to
idx/pairs.sN. --pairs PAIRS overrides that default and reads PAIRS instead;
idx/sN.pairs is still forced in. --pairs alone reads PAIRS with nothing
forced in. one of -s or --pairs is needed.

each pair of the pair file is kept when its larger-order YES in p1.idx is
in [P, 1], its words both have -m to -x letters (defaults 3 and 13), and,
with -d, its words are both in FILE, and is written in the order with the
larger YES. .85 (the default) and .5 are the usual bands. pgui offers the
views all, seed50 and seed85, filtered by its D: dictionary and its m/x
lengths.

forced pairs skip the YES band, and those the pair file lacks (a subset's
dropped pairs, say) print last, in idx/sN.pairs order. they still pass the
length and -d checks, and each of their words not in FILE gets a red
warning on stderr. pgui does the same for its seed's sN, warning once per
dictionary load.

.wf/best/sN/seed.m3.85.pairs, the sentence seed dfs-anagrams -t sN loads,
is still a file. rebuild.mk's commented-out seed target would write it
from pairs.sN; the existing links are left as they are.


## Older pair and seed files

idx/idx.2.sN.mM.xX.{all,dict}, idx/cluer.sN.mM.xX.{all,dict},
idx/{all,dict}.sN.mM.xX, idx/seed.sN.mM.{all,dict}.{85.15,50}.pairs,
idx/{dict,words}.mM.xX

the per-dictionary, per-length and per-band files the old rebuild.mk
wrote, and rebuild-seed.sh still writes. rebuild.mk and pgui no longer
read them; they are left in place.
