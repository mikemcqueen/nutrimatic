#!/usr/bin/env bash
set -euo pipefail

if [[ -z ${IDX:-} ]]; then
  echo "SKIP: export IDX to run the dfs CLI differential test" >&2
  exit 77
fi

dfs_anagrams=$1
find_anagrams=$2
test_dir=$(mktemp -d "${TMPDIR:-/tmp}/nutrimatic-dfs-diff.XXXXXX")

cleanup() {
  rm -f "$test_dir"/*
  rmdir "$test_dir"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

normalize() {
  awk '
    {
      score = $1
      count = 0
      for (i = 2; i <= NF; ++i)
        words[++count] = $i
      for (i = 2; i <= count; ++i) {
        value = words[i]
        j = i - 1
        while (j >= 1 && words[j] > value) {
          words[j + 1] = words[j]
          --j
        }
        words[j + 1] = value
      }
      key = words[1]
      for (i = 2; i <= count; ++i)
        key = key " " words[i]
      print key "\t" score
      delete words
    }
  ' "$1" | sort
}

"$dfs_anagrams" "$IDX" penbuilt -m 3 -n 100000 \
  > "$test_dir/dfs.stdout" 2> "$test_dir/dfs.stderr"
"$dfs_anagrams" "$IDX" penbuilt -m 3 -n 100000 \
  --cache-size 0 --allow-cache-fallback \
  > "$test_dir/dfs-uncached.stdout" 2> "$test_dir/dfs-uncached.stderr"
cmp "$test_dir/dfs.stdout" "$test_dir/dfs-uncached.stdout" ||
  fail "cached and uncached dfs-anagrams output differs"
"$find_anagrams" "$IDX" penbuilt -m 3 -c \
  > "$test_dir/find.stdout" 2> "$test_dir/find.stderr"

normalize "$test_dir/dfs.stdout" > "$test_dir/dfs.normalized"
normalize "$test_dir/find.stdout" > "$test_dir/find.normalized"
cut -f 1 "$test_dir/dfs.normalized" > "$test_dir/dfs.keys"
cut -f 1 "$test_dir/find.normalized" > "$test_dir/find.keys"

[[ -s "$test_dir/dfs.keys" ]] ||
  fail "the differential bag produced no results"
cmp "$test_dir/find.keys" "$test_dir/dfs.keys" ||
  fail "dfs-anagrams and find-anagrams emitted different word sets"

awk -F '\t' '
  NR == FNR {
    expected[$1] = $2
    next
  }
  {
    a = expected[$1] + 0
    b = $2 + 0
    if (a == b)
      next
    denominator = a > b ? a : b
    if (denominator == 0 || (a > b ? a - b : b - a) / denominator > 0.001) {
      printf "score mismatch for %s: %.17g versus %.17g\n", $1, a, b \
        > "/dev/stderr"
      bad = 1
    }
  }
  END { exit bad }
' "$test_dir/find.normalized" "$test_dir/dfs.normalized" ||
  fail "scores differ beyond the legacy float tolerance"
