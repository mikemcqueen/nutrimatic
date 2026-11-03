#!/bin/bash
# usage: letter-pair-words.sh -s N [--pm P] OLD-BAG NEW-BAG LETTERS...
# requires: source ./setup.sh
#
# For each letter, lists the words of seed pairs that fit NEW-BAG but not
# OLD-BAG and hold the letter in both words. LETTERS may be given as one
# argument ("hwdy", "h w d y") or several (h w d y). With more than one
# letter, each letter's words follow a "== L ==" header. --pm P is
# seed-pairs' p1 YES band, [P, 1] (default: seed-pairs' own).

set -euo pipefail

usage() {
  echo "usage: $(basename "$0") -s N [--pm P] OLD-BAG NEW-BAG LETTERS..." >&2
  exit 2
}

sentence=
seed_args=()
while [[ ${1-} == -* ]]; do
  [[ $# -ge 2 ]] || usage
  case $1 in
    -s) sentence=$2 ;;
    --pm) seed_args+=(--pm "$2") ;;
    *) usage ;;
  esac
  shift 2
done
[[ -n $sentence && $# -ge 3 ]] || usage
old_bag=$1
new_bag=$2
shift 2
letters=$(echo "$*" | tr -d ' ' | tr '[:upper:]' '[:lower:]')
[[ $letters =~ ^[a-z]+$ ]] || usage

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

pairs() {
  seed-pairs -s "$sentence" "${seed_args[@]}" |
    pfilter -s "$sentence" -d "$DICT" -m 0 -x 0 -l "$1" --ts -
}

words() {
  grep "$1.* .*$1" "$2" | top-segments --nc --pair-words -n0 2>/dev/null |
    sort -u || true
}

pairs "$old_bag" > "$tmp/old"
pairs "$new_bag" > "$tmp/new"

for ((i = 0; i < ${#letters}; i++)); do
  l=${letters:i:1}
  [[ ${#letters} -eq 1 ]] || echo "== $l =="
  comm -13 <(words "$l" "$tmp/old") <(words "$l" "$tmp/new")
done
