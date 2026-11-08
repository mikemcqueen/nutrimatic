1. generate pb,pb,pb or "pb3" pairs

    `seed-pairs -s7 -m4 | bag-stats-cuda --threads 0 -s7 --ss pb,pb,pb --uc $S7 - $DICT \
       --progress 15 > tmp/s7/pairs.pb3.ranked`

  importantly: these pairs are preceded by "count" for number of times they occur

2. extract top words from those pairs, honoring the "count" associated with each input pair:

  with counts:

    `cat tmp/s7/pairs.pb3.ranked | top-segments -n0 --pair-words --by-count > tmp/s7/pairs.pb3.ranked.words`

  without counts ("dict" format):

    `cat tmp/s7/pairs.pb3.ranked | top-segments -n0 --pair-words --by-count --nc > tmp/s7/pairs.pb3.words`

4. generate seed pairs using the top Nk words from above p3-pairs top-words dict, and pipe them
   through pfilter. I want to do this for top 1k, 2k, 3k, 4k, and 5k words.

    `cat tmp/s7/pairs.pb3.words | head -n 2000 | seed-pairs -s7 -m4 -d /dev/stdin \
       | pfilter -s7 -l $S7 - > tmp/s7/pairs.from.pb3.2k`

  this is "(A) pairs generated from (B) top 2k words of (C) bag-stats pb,pb,pb output"

5a. for short sentences (S6-S9), we can bag-stats pb,pb,wb,n and pb,pb,pb,wb,p using step 4 pairs as seed pairs.

  pb2,wb,p:
    
    `cat tmp/s7/pairs.from.pb3.2k | bag-stats-cuda -n 5000000 --wb --wbfrac-min --wbwp-min --threads 0 -s7 \
       --ss pb,pb,wb,p $S7 - $DICT --progress 15 --cache-size 5000 > tmp/s7/bs.pb2.wb.from.pairs.pb3.2k`

  pb3,wb,p:

    `cat tmp/s7/pairs.from.pb3.2k | bag-stats-cuda -n 5000000 --wb --wbfrac-min --wbwp-min --threads 0 -s7 \
       --ss pb,pb,pb,wb,p $S7 - $DICT --progress 15 --cache-size 5000 > tmp/s7/bs.pb3.wb.from.pairs.pb3.2k`

5b. haven't figured out exactly what to do with these. we can, for example run:

    `./bs-to-top.sh tmp/s7/bs.pb3.wb.from.pairs.pb3.2k`

  which generates a .top version of that file. that loses both the pair count (which aren't currently calculated
  by anything, but could be) and all the auxillary count information. at the very least, we should probably
  perform a pair count of the input with a modified bs-to-top.sh.

  then get pairs:
    
    `cat tmp/s7/bs.pb3.wb.from.pairs.pb3.2k.top | top-segments -n0 --pairs -c > tmp/s7/pairs.pb3.wb.from.pairs.pb3.2k.ranked`
    
  this probably doesn't work as well as pb3 since the input pairs here aren't unranked currently.

6. for all sentences, we can bag-stats pb,pb,pb --uc with the step 4 pairs as seed pairs.  We can also run 
   pb,pb,pb,pb on long enough bags (S1,S2,S4,S5) with some specified -u letters; I haven't yet specified where
   those letters come from though.

  `cat tmp/s7/pairs.from.pb3.2k | bag-stats-cuda --threads 0 -s7 --ss pb,pb,pb --uc $S7 - $DICT \
     --progress 15 > tmp/s7/pairs.pb3.from.pairs.pb3.2k.ranked`

7. once we have a new pairs...ranked file, we can start over at 2?  or 2a?  do we run a cuda again? is
   there a last step here before returning to 1?
