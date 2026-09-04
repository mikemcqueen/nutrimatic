so we know the Total segment list length T after removing the selected segment 
and we know the remaining segment list length R1 after removing all segments
that are incompatible with the selected segment. now i'd like to continue
iteratively, then recursively, for each reamining segment, removing segment
letters at each level, producing output (to stderr) that looks something like this:

selected segment (440/999)
  remaining segment (234/440)
    remaining segment (123/234)
    ...
      ...
      ...
  remaining segment (123/440)
    remaining segment (22/123)


ultimately i'd like to have a function that can be called with a list of segments,
and display (to stderr) the above nested segment hierarchy for the list, and 
displaying a diagnostic when it reaches a segment in the list that can no longer
be made from the remaining letters.
