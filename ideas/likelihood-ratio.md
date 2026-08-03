The full implementation would use the corresponding 2×2 contingency-table
likelihood ratio rather than relying solely on that Poisson approximation.

The necessary cells can be approximated from:

- pair occurrences        c(a,b)
- a without b              c(a) - c(a,b)
- b without preceding a    c(b) - c(a,b)
- neither                  N - c(a) - c(b) + c(a,b)

The lookups are:

- aggregate_entry_count("a b")
- aggregate_entry_count("a")
- aggregate_entry_count("b")
- IndexReader::count() for (N)
