1.


build/dfs-anagrams $IDX $S6 -m 4 -S 20 -p 10000000 -u toyfastmusketsalvo -n 1000000 --word-bonus 1 --dict tmp/words.big --pairs results/s6.m4.90.10.yes -x 2
-g 4 > results/dfs.s6.m4.b1.bw.x2.g4.pairs.1000000.u_toyfms

* note the -g 4 here.  scores are relative to the  number of segments.
* is --pairs s6.m4.90.10.yes already bigram filtered?  because s1 is not (though it's easy to make with comm).


2. top 1000 (arbitrary) segments of N (??)


build/top-segments results/dfs.s6.m4.b1.bw.x2.g4.pairs.1000000.u_toyfms --pairs | head -n 1000 > results/s6.idx.m4.yes.1000


3. pass those through to p2/yes to create notes; then get & parse YES from notes. Need some custom workflow for this probably.


for a in a b c; do note -pf.72 --get s6.idx.m4.1000.yes.filtered.a$a --production > tmp/s6.m4.1000.yes.a$a; done
for a in a b c; do note --parse-file tmp/s6.m4.1000.yes.a$a --type yes --lines >> tmp/s6.m4.1000.p2.yes ; done


4. use that newly generated p2.yes as the new --pairs input


build/dfs-anagrams $IDX $S6 -m 4 -S 20 -p 10000000 -u toyfastmusketsalvo -n 1000000 --word-bonus 1 --dict tmp/words.big --pairs tmp/s6.m4.1000.p2.yes -x 2 -g 4 > results/dfs.s6.m4.b1.bw.x2.g4.80yes.1000000.u_toyfms

* Note i actually ended up with 79yes somehow after removing henry,viii and adding wooden,toy. but this is the gist.
