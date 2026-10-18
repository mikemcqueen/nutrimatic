# PairsExactCounter's tag buckets

`PairsExactCounter` keeps its hash tags in 16-byte buckets (`tags_`), apart
from the group numbers (`slots_`), so one SSE compare checks 16 tags at once.
Keeping the tags separate means most unsuccessful lookups never load a group
number or the group's 32-byte letter counts from `all_.keys`.

Each tag is the hash's top 7 bits with the high bit set. That is far fewer
bits than the old 32-bit tag, so about 1 in 16 unsuccessful lookups at a 50%
load falls through to a full key comparison. Even so, the tag-bucket table
runs about 2x as fast as the old one on a bag-stats `-n 100` run over
`$S1TEST`, so the 7-bit tags stay.
