ok trying to nail down the workflow for filtering top-segments --solo-words.

1) first, i run:

cat results/s7/dfs.s7.idx2.85.15.m4.x2.g5.best.1000000.u-vindiesel | top-segments --wf --solo-words -n 0 > results/s7/top.solo

full list of solo-words with counts.

2) eliminate all words from a smaller dict.

./dict-filter -v results/s7/top.solo > results/s7/nowords.solo

this is all the words (with counts) that are not in /usr/share/dict/words.

3) cp results/s7/nowords.solo results/s7/good.solo, manually edit good.solo to remove bad words.

good.solo contains "good" words. 

4) i need the bad words.

sort -u results/s7/nowords.solo > results/s7/nowords.solo.sorted
sort -u results/s7/good.solo | comm -23 results/s7/nowords.solo.sorted - > results/s7/bad.solo.

5) remove the bad words from words.big

./dict-remove results/s7/bad.solo

6) regenerate top-segments --solo-words, given the dictionary has been updated.

cat results/s7/dfs.s7.idx2.85.15.m4.x2.g5.best.1000000.u-vindiesel | filter-segments --wf | top-segments --wf --solo-words -n 0 > results/s7/top2.solo

the filter-segments call might be unnecessary.  just being careful.

7) eliminate all words from a smaller dict.

./dict-filter -v results/s7/top2.solo > results/s7/nowords2.solo

so far so good, that's the part that went smooth. let me know if you think i got
anything wrong. now the part i need help with.

8) so i've filtered out the words from the smaller dict. but it's not quite
   ready for manual review. because it may still contain a) good words i've
   already reviewed in good*.solo, and bad words i've already classified in
   bad*.solo.

   but i can't just comm -23 here, because of the counts -- they (potentially)
   differ across different top-segment runs after some dictionary words have
   been eliminated by a previous run.  but they are somewhat useful to me in
   the classifying step so i'm inclined to see what it would take to preserve
   them.
   
   presumably, without counts, and a bit more sorting of good*.solo, this
   becomes a bit easier (but frankly, still a PITA - looking forward to getting
   this integrated into the words workflow).
   
one thought here is i could extend dict_filter.py to take a source "dictionary"
but for which i could support the top-segments with-count format as a dictionary.
so i could use dict-filter itself as a substitute for `comm -23`, and i wouldn't
have to worry about sorting in that case.

what do you think?
