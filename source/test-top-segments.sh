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

expected_longest_pairs='epsilon,zeta
one,two,three'
actual=$("$top_segments" --pairs --by-length -n 2 "$input")
[[ $actual == "$expected_longest_pairs" ]] ||
  fail "--pairs --by-length -n output is wrong: $actual"

expected_solo='3 alpha
2 delta'
actual=$("$top_segments" --solo-words "$input")
[[ $actual == "$expected_solo" ]] || fail "--solo-words output is wrong: $actual"

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

if "$top_segments" --pairs --all-words "$input" >/dev/null 2>&1; then
  fail "--pairs with --all-words succeeded"
fi

if "$top_segments" --counts "$input" >/dev/null 2>&1; then
  fail "--counts without --pairs succeeded"
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

expected_wf='2 alpha
2 beta gamma
1 delta'
actual=$(WFROOT=$wfroot "$top_segments" --wf "$input")
[[ $actual == "$expected_wf" ]] || fail "--wf counts are wrong: $actual"

expected_wf_yes='2 alpha
1 delta'
actual=$(WFROOT=$wfroot "$top_segments" --wf --yes "$input")
[[ $actual == "$expected_wf_yes" ]] || fail "--wf --yes counts are wrong: $actual"

actual=$(WFROOT=$wfroot "$top_segments" --wf -y "$input")
[[ $actual == "$expected_wf_yes" ]] || fail "--wf -y counts are wrong: $actual"

actual=$(WFROOT=$test_dir/not-the-root \
  "$top_segments" --wfroot "$wfroot" -y "$input")
[[ $actual == "$expected_wf_yes" ]] || fail "--wfroot -y counts are wrong: $actual"

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

expected_untargeted='1 alpha
1 delta
1 mu nu'
actual=$("$top_segments" --wfroot "$wfroot" -y "$target/dfs.seed" 2>/dev/null)
[[ $actual == "$expected_untargeted" ]] ||
  fail "absent target no.pairs changed the counts: $actual"

cat > "$target/no.pairs" <<'EOF'
nu,mu
EOF

actual=$("$top_segments" --wfroot "$wfroot" -y "$target/dfs.seed" 2>/dev/null)
[[ $actual == "1 alpha" ]] || fail "target no.pairs is wrong: $actual"

# Standard input names no target of its own, but the selected target is
# resolved regardless of the input, so its no.pairs is applied all the same.
actual=$("$top_segments" --wfroot "$wfroot" -y - < "$target/dfs.seed" \
  2>/dev/null)
[[ $actual == "1 alpha" ]] || fail "stdin did not apply the target no.pairs: $actual"

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
  [[ $actual == "1 alpha" ]] || fail "$name named no target: $actual"
done

# The directory a results file sits in says nothing, so a no.pairs kept
# beside it is not the target's and is never read.
cat > "$results/no.pairs" <<'EOF'
gamma,beta
EOF
actual=$("$top_segments" --wfroot "$wfroot" -y \
  "$results/dfs.s2.m4.x2.g4.1000000.u-abc" 2>/dev/null)
[[ $actual == "1 alpha" ]] || fail "a results-dir no.pairs was read: $actual"

# A name that parses but names a target that is not there disagrees with the
# selected target ("current", i.e. g4) and fails outright, rather than
# silently applying no exclusions.
cp "$target/dfs.seed" "$results/dfs.s2.m4.x2.g9.1000000.u-abc"
mismatch_diagnostics=$test_dir/mismatch-diagnostics.txt
if "$top_segments" --wfroot "$wfroot" -y \
    "$results/dfs.s2.m4.x2.g9.1000000.u-abc" \
    >/dev/null 2>"$mismatch_diagnostics"; then
  fail "an absent named target did not fail"
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

expected_target2='1 delta
1 mu nu'
target2_diagnostics=$test_dir/target2-diagnostics.txt
actual=$("$top_segments" --wfroot "$wfroot" --target s2/u-abc/m4/g5 \
  "$target2/dfs.seed" 2>"$target2_diagnostics")
[[ $actual == "$expected_target2" ]] ||
  fail "--target counts are wrong: $actual"
grep -q "TARGET resolved to s2/u-abc/m4/g5" "$target2_diagnostics" ||
  fail "--target resolution was not announced: $(cat "$target2_diagnostics")"

echo PASS
