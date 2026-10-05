find-expr uses its own expression parser, which compiles to an FST rather than a Perl-style backtracking regex (source/expr-parse.cpp), so there's no (?=…) syntax. You can get the same result with its & operator, which intersects expressions:

.*a.*&.*b.*&.*c.*

A phrase matches only if it satisfies all three parts, so it must contain a, b and c in any order.

- Precedence: & binds tighter than |, so x&y|z means (x&y)|z. Use parentheses to group, for example (.*a.*&.*b.*)|….
- Spaces: . also matches a space, so a match can span word boundaries. Use _ to match letters and digits only.
- Quotes: outside quotes, spaces are allowed between atoms. Inside "…", matching is literal and no extra spaces are allowed.

-----

How to find "word ...."

build/find-expr $IDX '"green A+"'

I tried it against $IDX (idx/wiki-merged.2.index) and got:

3.252e+04 green bay
3.254e+04 green and
2.346e+04 green party
1.159e+04 green line
8289. green in
...

Each line is a frequency score followed by the phrase. The results come out roughly from most to least common, so pipe through head or awk '{print $3}' to get just the following word.

How the expression works:
- A matches one letter. _ matches a letter or digit, C a consonant, V a vowel, # a digit, and . any character, including a space.
- "..." makes the match literal, so the space after "green" is required and no extra spaces can appear. Without the quotes, spaces are allowed between atoms, so A+ could run on into more words.
- You don't need a trailing space. Matches always end on a word boundary.
- - stands for an optional space.
- & intersects two expressions, for example '"green A+"&.*s' (it binds tighter than |). docs/find-expr.md has more on this.
