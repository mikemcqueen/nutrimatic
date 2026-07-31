so i wonder if there is a realistic (from execution-time perspective) compromise here,
where, when a final search result is being emitted, prior to emitting we check all
permutations of the words in the current search result, for the perm that has the
highest score, and use that score.
