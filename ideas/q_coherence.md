findings/rethinking_normalization.md mentions Q_coherence and kinda hand-waves over the details on how Q_pair might be determined, or exactly what is meant by an
"average" in this case.  regarding bigrams data, i do have "multi-word frequency" in the index, and i suppose i could "index the index" to map individual-word index
entries to multi-word-entries that contain that word (and/or vice versa), and use that as some proxy for bigram frequency.  but the nitty gritty details of what a
good Q_coherence calculation would look like are still a bit foggy to me.  is it a case of "try some stuff and see what looks good to you" or is there sound
mathematical, or otherwise, foundation with respect to natural language coherence (or word, or letter, frequency distribution in the english language, or something
else), that you can expand upon?
