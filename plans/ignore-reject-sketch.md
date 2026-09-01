i want to distinguish between "exclude pairs/segments" and "invalid pairs/segments"
in first-segments and filter-segments, and i want to add this support to top-segments
as well, along with the --wf support".

currently both first-/filter- segments really only support one CLI option type, "exclude".

I want to change that to two options:

-i/--ignore  : do not consider these segments as candidates for top/first/
-r/--reject  : do not consider an *result lines* that contain these segments;
               the results themselves are invalid due to the rejected segment


additionally:

--wf: loads classified NO into reject_pairs by default.

and an additional swtich:

--yes  : only valid if --wf is supplied; loads classified YES pairs into ignore_pairs.


first-segments/top-segments:
  * support -r/-i/--wf/--yes

filter-segments:
  * supports only -r/--wf.
   * -i makes no sense.
   * --yes unsupported.
