find-expr uses its own expression parser, which compiles to an FST rather than a Perl-style backtracking regex (source/expr-parse.cpp), so there's no (?=…) syntax. You can get the same result with its & operator, which intersects expressions:

.*a.*&.*b.*&.*c.*

A phrase matches only if it satisfies all three parts, so it must contain a, b and c in any order.

- Precedence: & binds tighter than |, so x&y|z means (x&y)|z. Use parentheses to group, for example (.*a.*&.*b.*)|….
- Spaces: . also matches a space, so a match can span word boundaries. Use _ to match letters and digits only.
- Quotes: outside quotes, spaces are allowed between atoms. Inside "…", matching is literal and no extra spaces are allowed.
