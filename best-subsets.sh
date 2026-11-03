#!/bin/bash
# usage: best-subsets.sh -s N [-l K] [-n TOP] [--only] BAG
# requires: source ./setup.sh; $DICT
#
# For k = 1..K letters removed from BAG (default K 6), prints the TOP
# removals (default 3) that leave the fewest sentence-N seed pairs fitting
# BAG, with the share of the full-BAG count left and the per-letter factor
# (left/base)^(1/k), which compares removals of different sizes. --only
# prints k = K alone.

set -euo pipefail

usage() {
  echo "usage: $(basename "$0") -s N [-l K] [-n TOP] [--only] BAG" >&2
  exit 2
}

sentence=
length=6
top=3
only=0
while [[ ${1-} == -* ]]; do
  if [[ $1 == --only ]]; then
    only=1
    shift
    continue
  fi
  [[ $# -ge 2 ]] || usage
  case $1 in
    -s) sentence=$2 ;;
    -l) length=$2 ;;
    -n) top=$2 ;;
    *) usage ;;
  esac
  shift 2
done
[[ -n $sentence && $# -eq 1 ]] || usage
bag=$1
dict=${DICT:?DICT is not set}

pairs=$(mktemp)
trap 'rm -f "$pairs"' EXIT

seed-pairs -s "$sentence" -m4 |
  pfilter -s "$sentence" -l "$bag" -d "$dict" - > "$pairs"
base=$(wc -l < "$pairs")
echo "base: $base pairs"

bag-subset -r -s "$sentence" -l "$length" -d "$dict" "$bag" "$pairs" |
  awk -v base="$base" -v top="$top" -v first=$((only ? length : 1)) -v width=$((length > 7 ? length : 7)) '
    length($1) >= first && seen[length($1)]++ < top {
      k = length($1)
      rows[k] = rows[k] sprintf("%2d  %-*s  %7d  %5.1f%%  %.3f\n",
          k, width, $1, $2, 100 * $2 / base, exp(log($2 / base) / k))
    }
    END {
      printf "%2s  %-*s  %7s  %6s  %s\n", "k", width, "removed", "left",
          "%left", "per-letter"
      for (k = first; k in rows; ++k) printf "%s", rows[k]
    }'
