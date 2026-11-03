
old way:

# note that pfilter --ts doesn't exist anymore.  top-segments auto-dectets and accepts "comma-separated pairs" as input.

# pw() { seed-pairs -s "$1" | pfilter -s "$1" -d "$DICT" -m 0 -x 0 -l "$2" --ts - | grep "${3}.* .*${3}" | top-segments --nc --pair-words -n0 2>/dev/null | sort -u; }

# calling it, e.g:

# for a in h ; do comm -23 <(pw 1 "$S1MIN5" "${a}") <(pw 1 "$S1MIN4" "${a}") | less; done

new way:

letter-pair-words.sh -s N [--pm P] OLD-BAG NEW-BAG LETTERS...

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


