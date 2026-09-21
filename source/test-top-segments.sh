#!/usr/bin/env bash
set -euo pipefail

top_segments=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-top-segments.XXXXXX")

cleanup() {
  rm -rf "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

input=$test_dir/results.txt
cat > "$input" <<'EOF'
9 alpha,beta gamma,alpha
8 beta gamma,delta
7 alpha,delta,epsilon zeta,one two three
EOF

expected='3 alpha
2 beta gamma
2 delta
1 epsilon zeta
1 one two three'

actual=$("$top_segments" "$input")
[[ $actual == "$expected" ]] || fail "file counts are wrong: $actual"

actual=$("$top_segments" < "$input")
[[ $actual == "$expected" ]] || fail "stdin counts are wrong: $actual"

dictionary=$test_dir/dictionary.txt
cat > "$dictionary" <<'EOF'
alpha
beta
gamma
EOF

expected_dict='2 alpha
1 beta gamma'
actual=$("$top_segments" -d "$dictionary" "$input")
[[ $actual == "$expected_dict" ]] ||
  fail "standalone dictionary filtering is wrong: $actual"

expected_pairs='beta,gamma
epsilon,zeta
one,two,three'

actual=$("$top_segments" --pairs "$input")
[[ $actual == "$expected_pairs" ]] || fail "pair output is wrong: $actual"

expected_pair_counts='2 beta,gamma
1 epsilon,zeta
1 one,two,three'
actual=$("$top_segments" --pairs --counts "$input")
[[ $actual == "$expected_pair_counts" ]] ||
  fail "pair count output is wrong: $actual"

actual=$("$top_segments" --pairs -c "$input")
[[ $actual == "$expected_pair_counts" ]] ||
  fail "-c pair count output is wrong: $actual"

expected_no_counts='alpha
beta gamma
delta
epsilon zeta
one two three'
actual=$("$top_segments" --no-counts "$input")
[[ $actual == "$expected_no_counts" ]] ||
  fail "--no-counts output is wrong: $actual"

expected_top='3 alpha
2 beta gamma'
actual=$("$top_segments" -n 2 "$input")
[[ $actual == "$expected_top" ]] || fail "-n output is wrong: $actual"

expected_top_pairs='beta,gamma
epsilon,zeta'
actual=$("$top_segments" --pairs -n 2 "$input")
[[ $actual == "$expected_top_pairs" ]] || fail "pair -n output is wrong: $actual"

expected_by_length='1 epsilon zeta
1 one two three
2 beta gamma
3 alpha
2 delta'
actual=$("$top_segments" --by-length "$input")
[[ $actual == "$expected_by_length" ]] ||
  fail "--by-length output is wrong: $actual"

actual=$("$top_segments" -l "$input")
[[ $actual == "$expected_by_length" ]] || fail "-l output is wrong: $actual"

# --elim partitions the surviving result lines for each exact candidate.  A
# duplicate on one line increments occurrences twice but line support once.
elim_input=$test_dir/elim-results.txt
cat > "$elim_input" <<'EOF'
9 pivot,common,repeat,repeat
8 pivot,common,repeat
7 pivot,common
6 common,other
5 common,other
4 rare
EOF

elim_header='DECISION REQUIRE REJECT COUNT SEGMENT'
expected_elim="$elim_header
       3       3      3     3 pivot
       2       4      2     3 repeat
       2       4      2     2 other
       1       1      5     5 common
       1       5      1     1 rare"
actual=$("$top_segments" --elim "$elim_input")
[[ $actual == "$expected_elim" ]] ||
  fail "--elim segment output is wrong: $actual"

expected_elim_pairs="$elim_header
       3       3      3     3 keep,together
       1       1      5     5 common,pair"
elim_pairs_input=$test_dir/elim-pairs-results.txt
cat > "$elim_pairs_input" <<'EOF'
9 keep together,common pair
8 keep together,common pair
7 keep together,common pair
6 common pair,solo1
5 common pair,solo2
4 solo3
EOF
actual=$("$top_segments" --elim --pairs "$elim_pairs_input")
[[ $actual == "$expected_elim_pairs" ]] ||
  fail "--elim --pairs output is wrong: $actual"

elim_words_input=$test_dir/elim-words-results.txt
cat > "$elim_words_input" <<'EOF'
9 ab cd,ef,ef
8 ab gh,ef
7 ij cd
6 solo
EOF

expected_elim_words="$elim_header
       2       2      2     3 ef
       2       2      2     2 ab
       2       2      2     2 cd
       1       3      1     1 gh
       1       3      1     1 ij
       1       3      1     1 solo"
actual=$("$top_segments" --elim --all-words "$elim_words_input")
[[ $actual == "$expected_elim_words" ]] ||
  fail "--elim --all-words output is wrong: $actual"

elim_unique_input=$test_dir/elim-unique-results.txt
cat > "$elim_unique_input" <<'EOF'
9 red fox,red fox
8 red fox
7 red dog
6 alpha
EOF

expected_elim_unique="$elim_header
       2       2      2     1 fox
       1       1      3     2 red
       1       3      1     1 dog"
actual=$("$top_segments" --elim --pair-words --unique "$elim_unique_input")
[[ $actual == "$expected_elim_unique" ]] ||
  fail "--elim --pair-words --unique output is wrong: $actual"

elim_filter_input=$test_dir/elim-filter-results.txt
cat > "$elim_filter_input" <<'EOF'
9 keep one,alpha
8 reject me,beta
7 keep one,gamma
EOF
elim_reject=$test_dir/elim-reject.pairs
cat > "$elim_reject" <<'EOF'
reject,me
EOF

expected_elim_filtered="$elim_header
       1       1      1     1 alpha
       1       1      1     1 gamma
       0       0      2     2 keep one"
actual=$("$top_segments" --elim -r "$elim_reject" "$elim_filter_input" \
  2>/dev/null)
[[ $actual == "$expected_elim_filtered" ]] ||
  fail "--elim counted a filtered result line: $actual"

if "$top_segments" --elim --by-length "$elim_input" >/dev/null 2>&1; then
  fail "--elim with --by-length succeeded"
fi

expected_longest_pairs='epsilon,zeta
one,two,three'
actual=$("$top_segments" --pairs --by-length -n 2 "$input")
[[ $actual == "$expected_longest_pairs" ]] ||
  fail "--pairs --by-length -n output is wrong: $actual"

# -u takes the letter bag from the first row and prints only what fits in it
# once the used letters come out.
used_input=$test_dir/used-results.txt
cat > "$used_input" <<'EOF'
9 ab cd,ef
8 abc,def
7 fed,cba
EOF
expected_used='1 ab cd
1 abc
1 cba'
actual=$("$top_segments" -u e --used-letters f "$used_input")
[[ $actual == "$expected_used" ]] || fail "-u output is wrong: $actual"

if "$top_segments" -u z "$used_input" >/dev/null 2>&1; then
  fail "-u with a letter not in the bag succeeded"
fi

expected_solo='3 alpha
2 delta'
actual=$("$top_segments" --solo-words "$input")
[[ $actual == "$expected_solo" ]] || fail "--solo-words output is wrong: $actual"

expected_solo_no_counts='alpha
delta'
actual=$("$top_segments" --solo-words --no-counts "$input")
[[ $actual == "$expected_solo_no_counts" ]] ||
  fail "--solo-words --no-counts output is wrong: $actual"

actual=$("$top_segments" --counts --solo-words "$input")
[[ $actual == "$expected_solo" ]] ||
  fail "--counts --solo-words output is wrong: $actual"

expected_all_words='3 alpha
2 beta
2 delta
2 gamma
1 epsilon
1 one
1 three
1 two
1 zeta'
actual=$("$top_segments" --all-words "$input")
[[ $actual == "$expected_all_words" ]] ||
  fail "--all-words output is wrong: $actual"

expected_all_words_no_counts='alpha
beta
delta
gamma
epsilon
one
three
two
zeta'
actual=$("$top_segments" --nc --all-words "$input")
[[ $actual == "$expected_all_words_no_counts" ]] ||
  fail "--nc --all-words output is wrong: $actual"

pair_words_input=$test_dir/pair-words-results.txt
cat > "$pair_words_input" <<'EOF'
9 red fox,alpha
8 red fox,beta
7 red dog,gamma
EOF

expected_pair_words='3 red
2 fox
1 dog'
actual=$("$top_segments" --pair-words "$pair_words_input")
[[ $actual == "$expected_pair_words" ]] ||
  fail "--pair-words output is wrong: $actual"

expected_unique_pair_words='2 red
1 dog
1 fox'
actual=$("$top_segments" --pair-words --unique "$pair_words_input")
[[ $actual == "$expected_unique_pair_words" ]] ||
  fail "--pair-words --unique output is wrong: $actual"

actual=$("$top_segments" --unique --pair-words "$pair_words_input")
[[ $actual == "$expected_unique_pair_words" ]] ||
  fail "--unique --pair-words output is wrong: $actual"

if "$top_segments" --pairs --all-words "$input" >/dev/null 2>&1; then
  fail "--pairs with --all-words succeeded"
fi

if "$top_segments" --counts --no-counts "$input" >/dev/null 2>&1; then
  fail "--counts with --no-counts succeeded"
fi

if "$top_segments" --nc -c "$input" >/dev/null 2>&1; then
  fail "--nc with -c succeeded"
fi

if "$top_segments" --elim --no-counts "$input" >/dev/null 2>&1; then
  fail "--elim with --no-counts succeeded"
fi

if "$top_segments" --unique "$input" >/dev/null 2>&1; then
  fail "--unique without --pair-words succeeded"
fi

if "$top_segments" --pair-words --pairs "$input" >/dev/null 2>&1; then
  fail "--pair-words with --pairs succeeded"
fi

ignore1=$test_dir/ignore1.pairs
cat > "$ignore1" <<'EOF'
gamma,beta
EOF
ignore2=$test_dir/ignore2.pairs
cat > "$ignore2" <<'EOF'
zeta,epsilon
EOF

expected_ignored='3 alpha
2 delta
1 one two three'
actual=$("$top_segments" -i "$ignore1" --ignore "$ignore2" "$input")
[[ $actual == "$expected_ignored" ]] || fail "ignored counts are wrong: $actual"

expected_rejected='1 alpha
1 delta
1 epsilon zeta
1 one two three'
actual=$("$top_segments" -r "$ignore1" "$input")
[[ $actual == "$expected_rejected" ]] || fail "rejected rows were counted: $actual"

allow_input=$test_dir/allow-results.txt
cat > "$allow_input" <<'EOF'
9 alpha beta,solo
8 delta epsilon,zeta eta
7 solo,alpha beta
6 solo2
5 alpha beta,unlisted pair
4 one two three,solo3
EOF
allow1=$test_dir/allow1.pairs
cat > "$allow1" <<'EOF'
beta,alpha
epsilon,delta
EOF
allow2=$test_dir/allow2.pairs
cat > "$allow2" <<'EOF'
eta,zeta
EOF
allow_dictionary=$test_dir/allow-dictionary.txt
cat > "$allow_dictionary" <<'EOF'
alpha
beta
delta
epsilon
eta
one
pair
solo
solo2
solo3
three
two
unlisted
zeta
EOF
allow_diagnostics=$test_dir/allow-diagnostics.txt

expected_allowed='2 alpha beta
2 solo
1 delta epsilon
1 solo2
1 zeta eta'
actual=$("$top_segments" -a "$allow1" --allow-pairs "$allow2" \
  -d "$allow_dictionary" "$allow_input" 2> "$allow_diagnostics")
[[ $actual == "$expected_allowed" ]] ||
  fail "allowlist counts are wrong: $actual"
[[ $(cat "$allow_diagnostics") == \
    'top-segments: Filtered 2 outside --allow-pairs' ]] ||
  fail "allowlist summary is wrong: $(cat "$allow_diagnostics")"

expected_allow_elim="$elim_header
       2       2      2     2 alpha beta
       2       2      2     2 solo
       1       3      1     1 delta epsilon
       1       3      1     1 solo2
       1       3      1     1 zeta eta"
actual=$("$top_segments" --elim -a "$allow1" -a "$allow2" \
  -d "$allow_dictionary" "$allow_input" 2>/dev/null)
[[ $actual == "$expected_allow_elim" ]] ||
  fail "--elim counted rows outside the allowlist: $actual"

empty_allow=$test_dir/empty-allow.pairs
: > "$empty_allow"
actual=$("$top_segments" -a "$empty_allow" -d "$allow_dictionary" \
  "$allow_input" 2>/dev/null)
[[ $actual == '1 solo2' ]] || fail "empty allowlist is wrong: $actual"

malformed_allow=$test_dir/malformed-allow.pairs
cat > "$malformed_allow" <<'EOF'
one,two,three
EOF
if "$top_segments" -a "$malformed_allow" "$allow_input" \
    >/dev/null 2>&1; then
  fail "malformed allowlist entry succeeded"
fi

precedence_input=$test_dir/allow-precedence-results.txt
cat > "$precedence_input" <<'EOF'
3 reject me,unlisted pair,badword
2 unlisted pair,badword
1 allowed pair,badword
0 badword,unlisted pair,reject me
EOF
precedence_reject=$test_dir/allow-precedence-reject.pairs
cat > "$precedence_reject" <<'EOF'
reject,me
EOF
precedence_allow=$test_dir/allow-precedence-allow.pairs
cat > "$precedence_allow" <<'EOF'
allowed,pair
EOF
precedence_dictionary=$test_dir/allow-precedence-dictionary.txt
cat > "$precedence_dictionary" <<'EOF'
allowed
pair
EOF
actual=$("$top_segments" -r "$precedence_reject" -a "$precedence_allow" \
  -d "$precedence_dictionary" "$precedence_input" \
  2> "$allow_diagnostics")
[[ -z $actual ]] || fail "allowlist precedence produced output: $actual"
expected_precedence_summary='top-segments: Filtered 2 rejected explicitly, 1 outside --allow-pairs, 1 rejected by dictionary'
[[ $(cat "$allow_diagnostics") == "$expected_precedence_summary" ]] ||
  fail "allowlist precedence is wrong: $(cat "$allow_diagnostics")"

wfroot=$test_dir/wf
mkdir -p "$wfroot/.wf/classified/yes" "$wfroot/.wf/classified/no"
cat > "$wfroot/.wf/classified/yes/yes.pairs" <<'EOF'
gamma,beta
EOF
cat > "$wfroot/.wf/classified/no/no.pairs" <<'EOF'
zeta,epsilon
EOF

# --wf and --wfroot always resolve a target, defaulting to "current", so every
# workflow root needs one even when a test has nothing target-specific to say.
target=$wfroot/.wf/best/s2/u-abc/m4/g4
mkdir -p "$target"
ln -s s2/u-abc/m4/g4 "$wfroot/.wf/best/current"

workflow_input=$test_dir/workflow-results.txt
cat > "$workflow_input" <<'EOF'
9 alpha,beta gamma,theta iota
8 beta gamma,delta
EOF

# With no selected unit, workflow mode defaults to pairs and ignores
# classified YES pairs. Explicit units retain their normal filtering: they do
# not add -y themselves.
actual=$(WFROOT=$wfroot "$top_segments" --wf "$workflow_input")
[[ $actual == 'theta,iota' ]] ||
  fail "--wf did not default to --pairs -y: $actual"

expected_wf_pairs='beta,gamma
theta,iota'
actual=$(WFROOT=$wfroot "$top_segments" --wf --pairs "$workflow_input")
[[ $actual == "$expected_wf_pairs" ]] ||
  fail "--wf --pairs unexpectedly enabled -y: $actual"

expected_wf_solo='1 alpha
1 delta'
actual=$(WFROOT=$wfroot "$top_segments" --wf --solo-words "$workflow_input")
[[ $actual == "$expected_wf_solo" ]] ||
  fail "--wf --solo-words selected the wrong unit: $actual"

expected_wf_all_words='2 beta
2 gamma
1 alpha
1 delta
1 iota
1 theta'
actual=$(WFROOT=$wfroot "$top_segments" --wf --all-words "$workflow_input")
[[ $actual == "$expected_wf_all_words" ]] ||
  fail "--wf --all-words unexpectedly enabled -y: $actual"

if "$top_segments" --pairs --solo-words "$input" >/dev/null 2>&1; then
  fail "--pairs with --solo-words succeeded"
fi

if "$top_segments" --solo-words --all-words "$input" >/dev/null 2>&1; then
  fail "--solo-words with --all-words succeeded"
fi

if "$top_segments" --wf --wfroot "$wfroot" "$input" \
    >/dev/null 2>&1; then
  fail "--wf with --wfroot succeeded"
fi

limit_input=$test_dir/limit.txt
for i in $(seq -w 0 1000); do
  printf '1 segment%s\n' "$i"
done > "$limit_input"
actual=$("$top_segments" "$limit_input" | wc -l)
[[ $actual == 1000 ]] || fail "default -n is not 1000: $actual"

actual=$("$top_segments" -n 0 "$limit_input" | wc -l)
[[ $actual == 1001 ]] || fail "-n 0 did not print all rows: $actual"

actual=$("$top_segments" -n0 "$limit_input" | wc -l)
[[ $actual == 1001 ]] || fail "-n0 did not print all rows: $actual"

if "$top_segments" -n nope "$input" >/dev/null 2>&1; then
  fail "invalid -n succeeded"
fi

if "$top_segments" --yes "$input" >/dev/null 2>&1; then
  fail "--yes without --wf succeeded"
fi

if "$top_segments" -x "$ignore1" "$input" >/dev/null 2>&1; then
  fail "removed -x option succeeded"
fi

# A target artifact's own no.pairs joins the root's; the target is the
# selected one ("current", made above, points here), not inferred from the
# input file. Its rejection is whole-row, so "delta" goes with the "mu nu" it
# shares a line with.
cat > "$target/dfs.seed" <<'EOF'
9 alpha,beta gamma
8 delta,mu nu
EOF

expected_untargeted='mu,nu'
actual=$("$top_segments" --wfroot "$wfroot" -y "$target/dfs.seed" 2>/dev/null)
[[ $actual == "$expected_untargeted" ]] ||
  fail "absent target no.pairs changed the counts: $actual"

cat > "$target/no.pairs" <<'EOF'
nu,mu
EOF

actual=$("$top_segments" --wfroot "$wfroot" -y "$target/dfs.seed" 2>/dev/null)
[[ -z $actual ]] || fail "target no.pairs is wrong: $actual"

workflow_precedence=$test_dir/workflow-precedence-results.txt
cat > "$workflow_precedence" <<'EOF'
5 badword,unlisted pair,reject me,mu nu,zeta epsilon
4 badword,unlisted pair,reject me,mu nu
3 badword,unlisted pair,reject me
2 badword,unlisted pair
1 allowed pair,badword
EOF
actual=$("$top_segments" --wfroot "$wfroot" -t current \
  -r "$precedence_reject" -a "$precedence_allow" \
  -d "$precedence_dictionary" "$workflow_precedence" \
  2> "$allow_diagnostics")
[[ -z $actual ]] || fail "workflow precedence produced output: $actual"
expected_precedence_summary='top-segments: Filtered 1 lines from classified/no/no.pairs, 1 lines from s2/u-abc/m4/g4/no.pairs, 1 rejected explicitly, 1 outside --allow-pairs, 1 rejected by dictionary'
[[ $(tail -n 1 "$allow_diagnostics") == "$expected_precedence_summary" ]] ||
  fail "workflow precedence is wrong: $(cat "$allow_diagnostics")"

# Standard input names no target of its own, but the selected target is
# resolved regardless of the input, so its no.pairs is applied all the same.
actual=$("$top_segments" --wfroot "$wfroot" -y - < "$target/dfs.seed" \
  2>/dev/null)
[[ -z $actual ]] || fail "stdin did not apply the target no.pairs: $actual"

# A results file kept outside the tree names its target instead, and the
# name is read inward from both ends: the seed annotation in the middle is
# any number of components, and .best may or may not be there.
cat > "$target/no.pairs" <<'EOF'
nu,mu
EOF
results=$test_dir/results/s2
mkdir -p "$results"
for name in dfs.s2.idx2.85.15.m4.x2.g4.1000000.u-abc \
            dfs.s2.idx2.85.15.m4.x2.g4.best.1000000.u-abc \
            dfs.s2.m4.x2.g4.1000000.u-abc; do
  cp "$target/dfs.seed" "$results/$name"
  actual=$("$top_segments" --wfroot "$wfroot" -y "$results/$name" 2>/dev/null)
  [[ -z $actual ]] || fail "$name named no target: $actual"
done

# The directory a results file sits in says nothing, so a no.pairs kept
# beside it is not the target's and is never read.
cat > "$results/no.pairs" <<'EOF'
gamma,beta
EOF
actual=$("$top_segments" --wfroot "$wfroot" -y \
  "$results/dfs.s2.m4.x2.g4.1000000.u-abc" 2>/dev/null)
[[ -z $actual ]] || fail "a results-dir no.pairs was read: $actual"

# A name that parses but names a target that is not there is, with no -t,
# the selected target itself -- so the failure is that target missing, not a
# disagreement with some other selection.
cp "$target/dfs.seed" "$results/dfs.s2.m4.x2.g9.1000000.u-abc"
mismatch_diagnostics=$test_dir/mismatch-diagnostics.txt
if "$top_segments" --wfroot "$wfroot" -y \
    "$results/dfs.s2.m4.x2.g9.1000000.u-abc" \
    >/dev/null 2>"$mismatch_diagnostics"; then
  fail "an absent inferred target did not fail"
fi
if ! grep -q 'target "s2/u-abc/m4/g9" is not a directory' \
    "$mismatch_diagnostics"; then
  fail "absent inferred target diagnostic is wrong: $(cat "$mismatch_diagnostics")"
fi

# The disagreement case is still real once -t names a different, existing
# target of its own: the two names are compared and neither wins silently.
if "$top_segments" --wfroot "$wfroot" --target s2/u-abc/m4/g4 -y \
    "$results/dfs.s2.m4.x2.g9.1000000.u-abc" \
    >/dev/null 2>"$mismatch_diagnostics"; then
  fail "a named target disagreeing with -t did not fail"
fi
if ! grep -q \
    'belongs to target "s2/u-abc/m4/g9", not the selected "s2/u-abc/m4/g4"' \
    "$mismatch_diagnostics"; then
  fail "mismatched target diagnostic is wrong: $(cat "$mismatch_diagnostics")"
fi

# -t/--target picks a different target explicitly, and the pick is announced.
target2=$wfroot/.wf/best/s2/u-abc/m4/g5
mkdir -p "$target2"
cp "$target/dfs.seed" "$target2/dfs.seed"
cat > "$target2/no.pairs" <<'EOF'
beta,gamma
EOF

expected_target2='mu,nu'
target2_diagnostics=$test_dir/target2-diagnostics.txt
actual=$("$top_segments" --wfroot "$wfroot" --target s2/u-abc/m4/g5 \
  "$target2/dfs.seed" 2>"$target2_diagnostics")
[[ $actual == "$expected_target2" ]] ||
  fail "--target counts are wrong: $actual"
grep -q "TARGET resolved to s2/u-abc/m4/g5" "$target2_diagnostics" ||
  fail "--target resolution was not announced: $(cat "$target2_diagnostics")"

# With no -t at all, the same g5 is picked up from the input instead: by
# directory for a file kept in the tree, by name for one kept outside it.
inferred_diagnostics=$test_dir/inferred-diagnostics.txt
"$top_segments" --wfroot "$wfroot" -y "$target2/dfs.seed" \
  >/dev/null 2>"$inferred_diagnostics"
grep -q "TARGET resolved to s2/u-abc/m4/g5" "$inferred_diagnostics" ||
  fail "target was not inferred from an in-tree path: $(cat "$inferred_diagnostics")"

cp "$target2/dfs.seed" "$results/dfs.s2.m4.x2.g5.1000000.u-abc"
"$top_segments" --wfroot "$wfroot" -y \
  "$results/dfs.s2.m4.x2.g5.1000000.u-abc" \
  >/dev/null 2>"$inferred_diagnostics"
grep -q "TARGET resolved to s2/u-abc/m4/g5" "$inferred_diagnostics" ||
  fail "target was not inferred from a results file name: $(cat "$inferred_diagnostics")"

echo PASS
