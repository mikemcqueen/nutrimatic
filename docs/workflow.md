1. start with a USED letter sequence and wf best prepare -g (N1 N2 N3)
   --source seed for that sequence, depending on length.  -g 4, -g 5, -g6
   are candidates for 2-line sentences. (sentence - USED) / 8 is a rough
   estimate. around 40 characters is where dfs-anagrams sometimes tops 
   out for reasonable execution time at -g 5.  (-g 6 of one s7 USED
   variation takes 3.5 hrs).
   
2. best review and exclude (NO-classify) top segment candidates (and maybe
   select a few good ones for YES classification). I'm not absolutely 
   certain this review step is required but it seems like a small price
   to pay and might actually be more beneficial that I realize.
   
3. best prepare -g N --source best.  at this point, the dfs.best result
   should generally always be filtered through filter-segments --wf to
   remove all results with NO-classified segments.

4. now it's time to start analyzing top-segments and first-segments of
   dfs.best, generally using --wf, with variations of --pairs (--count),
   --solo-words, and --all-words.

5. I've been using a manually edited results/sN/no.pairs to declare
   target-local NO-pairs that can be included with --reject. As more 
   result sets within a particular sentence get analyzed, it will likely
   make sense to either extend the results/ hierarchy to include a USED
   pairs directory and possibly a gN directory. Either that, or adopt
   the target-local no.pairs directly into the managed target directory
   itself (makes editing a tiny bit more painful but good for organization).
   


