## IDEA

for find-anagrams I'm thinking it still might be useful for large letter sets to find some
high-score results that don't use *all* the letters, i.e. they are "partial" anagrams but
not *complete* anagrams.

for example, if i provide 40 letters, and a flag -p/--partial-length 20, it will stop
searching a particular path after it exceeds 20 characters (not including spaces) total. 
an additional useful step for this would be to first determine that there is at least one
valid combination of words that could be made from the remaining 20 letters, which could
presumably be done purely with a DFS and early exit on first found.
