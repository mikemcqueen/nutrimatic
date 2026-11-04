#!/bin/bash
# usage: letter-pair-diff.sh -s N [--pm P] OLD-BAG NEW-BAG LETTERS...
#        letter-pair-diff.sh -s N [--pm P] -l BAG -u USED -U NEW_USED LETTERS...
# requires: source ./setup.sh
#
# For each letter, counts the seed pairs (and their words) that fit NEW-BAG
# but not OLD-BAG and hold the letter in both words. With -l, OLD-BAG is BAG
# less USED and NEW-BAG is BAG less NEW_USED; a USED or NEW_USED that doesn't
# fit BAG, or uses all of it, is an error. LETTERS may be given as
# one argument ("hwdy", "h w d y") or several (h w d y). Prints
# "LTR WORDS PAIRS" rows: the letter, its new words, its new pairs, then an
# "ALL" row of distinct words and pairs across the letters (not the column
# sums: a pair with two of the letters counts once). --pm P is seed-pairs' p1
# YES band, [P, 1] (default: seed-pairs' own).

set -euo pipefail

usage() {
  local name
  name=$(basename "$0")
  echo "usage: $name -s N [--pm P] OLD-BAG NEW-BAG LETTERS..." >&2
  echo "       $name -s N [--pm P] -l BAG -u USED -U NEW_USED LETTERS..." >&2
  exit 2
}

sentence=
seed_args=()
unset bag used new_used
while [[ ${1-} == -* ]]; do
  [[ $# -ge 2 ]] || usage
  case $1 in
    -s) sentence=$2 ;;
    --pm) seed_args+=(--pm "$2") ;;
    -l) bag=$2 ;;
    -u) used=$2 ;;
    -U) new_used=$2 ;;
    *) usage ;;
  esac
  shift 2
done
[[ -n $sentence ]] || usage
if [[ -n ${bag+x} ]]; then
  [[ -n ${used+x} && -n ${new_used+x} && $# -ge 1 ]] || usage
  old_bag=$bag
  new_bag=$bag
else
  [[ -z ${used+x} && -z ${new_used+x} && $# -ge 3 ]] || usage
  old_bag=$1
  new_bag=$2
  used=
  new_used=
  shift 2
fi
letters=$(echo "$*" | tr -d ' ' | tr '[:upper:]' '[:lower:]')
[[ $letters =~ ^[a-z]+$ ]] || usage

pairs() {
  seed-pairs -s "$sentence" "${seed_args[@]}" |
    pfilter -s "$sentence" -d "$DICT" -m 0 -x 0 -l "$1" -u "$2" -
}

old=$(pairs "$old_bag" "$used")
new=$(pairs "$new_bag" "$new_used")

awk -F, -v letters="$letters" '
  {
    for (i = 1; i <= length(letters); i++) {
      l = substr(letters, i, 1)
      if (!index($1, l) || !index($2, l)) continue
      if (FILENAME == ARGV[1]) {
        old_pair[l, $0] = 1
        old_word[l, $1] = 1
        old_word[l, $2] = 1
      } else {
        new_pair[l, $0] = 1
        new_word[l, $1] = 1
        new_word[l, $2] = 1
      }
    }
  }
  END {
    for (k in new_pair) if (!(k in old_pair)) {
      split(k, p, SUBSEP); pairs[p[1]]++; all_pair[p[2]] = 1
    }
    for (k in new_word) if (!(k in old_word)) {
      split(k, p, SUBSEP); words[p[1]]++; all_word[p[2]] = 1
    }
    rule = "-----------------"
    printf "%-3s %6s %6s\n", "LTR", "WORDS", "PAIRS"
    print rule
    for (i = 1; i <= length(letters); i++) {
      l = substr(letters, i, 1)
      printf "%-3s %6d %6d\n", l, words[l], pairs[l]
    }
    nw = 0; for (w in all_word) nw++
    np = 0; for (k in all_pair) np++
    print rule
    printf "%-3s %6d %6d\n", "ALL", nw, np
  }
' <(printf '%s\n' "$old") <(printf '%s\n' "$new")
