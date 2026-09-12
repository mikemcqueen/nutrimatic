#!/usr/bin/env bash
set -euo pipefail

filter_segments=$1
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-filter-segments.XXXXXX")

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
9 alpha beta,beta gamma
8 delta epsilon,zeta eta
7 theta iota,kappa lambda
6 mu nu
EOF

reject=$test_dir/reject.pairs
cat > "$reject" <<'EOF'
gamma,beta
iota,theta
EOF

expected='8 delta epsilon,zeta eta
6 mu nu'
actual=$("$filter_segments" -r "$reject" "$input")
[[ $actual == "$expected" ]] || fail "file filtering is wrong: $actual"

expected='8 delta epsilon,zeta eta'
actual=$("$filter_segments" -n 1 --reject "$reject" "$input")
[[ $actual == "$expected" ]] || fail "output limit is wrong: $actual"

actual=$("$filter_segments" -n 0 --reject "$reject" "$input")
[[ -z $actual ]] || fail "-n 0 produced output: $actual"

expected='9 alpha beta,beta gamma
8 delta epsilon,zeta eta
7 theta iota,kappa lambda
6 mu nu'
actual=$("$filter_segments" < "$input")
[[ $actual == "$expected" ]] || fail "default stdin is wrong: $actual"

expected='8 delta epsilon,zeta eta
6 mu nu'
actual=$("$filter_segments" -r "$reject" - < "$input")
[[ $actual == "$expected" ]] || fail "explicit stdin is wrong: $actual"

dictionary=$test_dir/dictionary.txt
cat > "$dictionary" <<'EOF'
alpha
beta
gamma
EOF

expected='9 alpha beta,beta gamma'
actual=$("$filter_segments" -d "$dictionary" "$input")
[[ $actual == "$expected" ]] ||
  fail "standalone dictionary filtering is wrong: $actual"

wfroot=$test_dir/wf
mkdir -p "$wfroot/.wf/classified/yes" "$wfroot/.wf/classified/no"
cat > "$wfroot/.wf/classified/yes/yes.pairs" <<'EOF'
epsilon,delta
EOF
cat > "$wfroot/.wf/classified/no/no.pairs" <<'EOF'
nu,mu
EOF

# --wf and --wfroot always resolve a target, defaulting to "current", so every
# workflow root needs one even when a test has nothing target-specific to say.
target=$wfroot/.wf/best/s2/u-abc/m4/g4
mkdir -p "$target"
ln -s s2/u-abc/m4/g4 "$wfroot/.wf/best/current"

expected='8 delta epsilon,zeta eta'
actual=$(WFROOT=$wfroot "$filter_segments" --wf -r "$reject" "$input")
[[ $actual == "$expected" ]] || fail "--wf rejections are wrong: $actual"

actual=$(WFROOT=$test_dir/not-the-root \
  "$filter_segments" --wfroot "$wfroot" -r "$reject" "$input")
[[ $actual == "$expected" ]] || fail "--wfroot rejections are wrong: $actual"

mkdir -p "$wfroot/.wf/dict"
cat > "$wfroot/.wf/dict/words.filtered" <<'EOF'
alpha
beta
gamma
delta
epsilon
zeta
eta
EOF

expected='9 alpha beta,beta gamma
8 delta epsilon,zeta eta'
actual=$(WFROOT=$wfroot "$filter_segments" --wf "$input")
[[ $actual == "$expected" ]] || fail "--wf dictionary filtering is wrong: $actual"

cat > "$dictionary" <<'EOF'
theta
iota
kappa
lambda
EOF
expected='7 theta iota,kappa lambda'
actual=$(WFROOT=$wfroot \
  "$filter_segments" --wf --dict "$dictionary" "$input")
[[ $actual == "$expected" ]] || fail "--dict did not override workflow: $actual"

if "$filter_segments" --wf --wfroot "$wfroot" "$input" \
    >/dev/null 2>&1; then
  fail "--wf with --wfroot succeeded"
fi

if "$filter_segments" -i "$reject" "$input" >/dev/null 2>&1; then
  fail "unsupported -i option succeeded"
fi

if "$filter_segments" --yes "$input" >/dev/null 2>&1; then
  fail "unsupported --yes option succeeded"
fi

if "$filter_segments" -x "$reject" "$input" >/dev/null 2>&1; then
  fail "removed -x option succeeded"
fi

if "$filter_segments" "$input" "$input" >/dev/null 2>&1; then
  fail "multiple input files succeeded"
fi

if "$filter_segments" -n nope "$input" >/dev/null 2>&1; then
  fail "invalid -n succeeded"
fi

if "$filter_segments" --dict >/dev/null 2>&1; then
  fail "--dict without a path succeeded"
fi

if "$filter_segments" --dict '' "$input" >/dev/null 2>&1; then
  fail "--dict with an empty path succeeded"
fi

# The target a FILE is an artifact of, or the selected target when it names
# none, contributes its own rejections, which the pipeline relies on:
# filtering here is what lets a downstream tool read standard input without
# needing a target of its own.
cp "$input" "$target/dfs.seed"
cat > "$target/no.pairs" <<'EOF'
beta,alpha
EOF

expected='8 delta epsilon,zeta eta'
actual=$(WFROOT=$wfroot "$filter_segments" --wf "$target/dfs.seed")
[[ $actual == "$expected" ]] || fail "target no.pairs is wrong: $actual"

actual=$(WFROOT=$wfroot "$filter_segments" --wf < "$target/dfs.seed")
[[ $actual == "$expected" ]] ||
  fail "stdin did not apply the target no.pairs: $actual"

diagnostics=$test_dir/diagnostics.txt

# -t naming a directory under .wf/best that is not a target -- the shape
# needs exactly four components below best, ending in mN/gN -- is fatal.
mkdir -p "$wfroot/.wf/best/idx"
if WFROOT=$wfroot "$filter_segments" --wf -t idx "$input" \
    >/dev/null 2> "$diagnostics"; then
  fail "-t on a non-target directory succeeded"
fi
if ! grep -q 'not ROOT/.wf/best/SENTENCE/LETTERS/mN/gN' "$diagnostics"; then
  fail "-t on a non-target directory diagnostic is wrong: $(cat "$diagnostics")"
fi

# -t/--target only means something once a workflow root is selected.
if "$filter_segments" -t current "$input" >/dev/null 2> "$diagnostics"; then
  fail "-t without --wf or --wfroot succeeded"
fi
if ! grep -q -- '--target requires --wf or --wfroot' "$diagnostics"; then
  fail "-t without --wf diagnostic is wrong"
fi

echo PASS
