#!/bin/bash
# usage: rebuild-seed.sh [-f|--force] sN|SN
# requires: source ./setup.sh

# braces make bash parse the whole script before running it, so editing the
# file during a run doesn't change what the run does
{
set -euo pipefail

die() {
    echo "rebuild-seed: $*" >&2
    exit 1
}

usage() {
    die "usage: $0 [-f|--force] sN|SN (N is 1-9)"
}

force=
seed=
for arg; do
    case $arg in
        -f|--force) force=1 ;;
        -*) usage ;;
        *) [ -z "$seed" ] || usage; seed=$arg ;;
    esac
done
[[ $seed =~ ^[sS]([1-9])$ ]] || usage
snum=${BASH_REMATCH[1]}
S=S${snum}
letters=${!S:-}
[ -n "$letters" ] || die "\$$S is unset or empty; source ./setup.sh"
[ -n "${IDX_DIR:-}" ] || die "\$IDX_DIR is unset; source ./setup.sh"
[ -d "$IDX_DIR" ] || die "IDX_DIR $IDX_DIR is not a directory"

here=$(cd "$(dirname "$0")" && pwd)
wf=$here/wf
cluer=$(dirname "$(readlink -f "$wf")")/cluer
tokens=$here/cluer/data/index/tokens.txt
extra_pairs=$IDX_DIR/s${snum}.pairs

for tool in query-index pairs pcomm python; do
    command -v "$tool" > /dev/null || die "$tool not found on PATH; source ./setup.sh"
done
[ -x "$wf" ] || die "$wf not found"
[ -f "$cluer/query_index.py" ] || die "$cluer/query_index.py not found"
[ -f "$tokens" ] || die "$tokens not found"

msg() {
    echo "$*" >&2
}

# true if $1 exists, is nonempty, and is no older than any of the remaining
# files
up_to_date() {
    local out=$1 src
    shift
    [ -s "$out" ] || return 1
    for src; do
        if [ "$src" -nt "$out" ]; then
            return 1
        fi
    done
}

# usage: run_to WHAT OUT [SRC...] -- CMD...
# skip if OUT is up to date with the SRC files and --force is not set;
# otherwise print WHAT and write the stdout of CMD to OUT, only if CMD
# succeeds; set slow=1 to warn that CMD takes a while
run_to() {
    local what=$1 out=$2
    local srcs=()
    shift 2
    while [ $# -gt 0 ] && [ "$1" != -- ]; do
        srcs+=("$1")
        shift
    done
    [ $# -gt 1 ] || die "usage: run_to WHAT OUT [SRC...] -- CMD..."
    shift
    if [ -z "$force" ] && up_to_date "$out" ${srcs[@]+"${srcs[@]}"}; then
        msg "Skipping $what, $(basename "$out") is up to date"
        return
    fi
    msg "Generating $what${slow:+, this might take a while...}"
    "$@" > "$out.tmp" || { rm -f "$out.tmp"; die "failed writing $out"; }
    mv "$out.tmp" "$out"
}

idx_pairs() {
    query-index "$letters" --wf -w2 -n0 --csv -m"$min" --dict "$words"
}

# cluer pairs, filtering out non-cluer-index dict words before sending to pairs
cluer_pairs() {
    LC_ALL=C comm -12 "$tokens" "$words" |
        pairs "$letters" -d - -m"$min" |
        python "$cluer/query_index.py" -f - -j --threads 2
}

# adds $extra_pairs when it exists
all_pairs() {
    if [ -f "$extra_pairs" ]; then
        pcomm "$cluer_out" "$idx_out" | pcomm - "$extra_pairs"
    else
        pcomm "$cluer_out" "$idx_out"
    fi | sort -u
}

build_pairs() {
    [ $# -eq 4 ] || die "usage: build_pairs DICT BASE MIN MAX"
    dict=$1
    base=$2
    min=$3
    max=$4
    ext=m${min}.x${max}
    words=$IDX_DIR/${dict}.${ext}
    [ -f "$words" ] || die "$words not found"

    idx_out=$IDX_DIR/idx.2.s${snum}.${ext}.${base}
    cluer_out=$IDX_DIR/cluer.s${snum}.${ext}.${base}

    run_to "$ext wiki-index pairs" "$idx_out" -- idx_pairs
    slow=1 run_to "$ext cluer pairs" "$cluer_out" -- cluer_pairs
    run_to "$ext combined pairs" "$IDX_DIR/${base}.s${snum}.${ext}" \
        "$cluer_out" "$idx_out" "$extra_pairs" -- all_pairs
}

build_pairs dict  dict 3 10
build_pairs dict  dict 3 13
build_pairs dict  dict 4 10
build_pairs words all  3 10
build_pairs words all  3 13
build_pairs words all  4 10

extract_yes_pairs() {
    [ $# -eq 3 ] || die "usage: extract_yes_pairs BASE MIN MAX"
    base=$1
    min=$2
    max=$3
    ext=m${min}.x${max}
    pairs_file=$IDX_DIR/${base}.s${snum}.$ext
    [ -f "$pairs_file" ] || die "$pairs_file not found"

    msg "Extracting $base $ext .85/.15 seed pairs"
    "$wf" extract p1 yes -f --pairs "$pairs_file" --pm .85 --pr .15 \
        -o "$IDX_DIR/seed.s${snum}.m${min}.${base}.85.15.pairs" ||
        die "wf extract failed for $pairs_file (.85/.15)"
    msg "Extracting $base $ext .50 seed pairs"
    "$wf" extract p1 yes -f --pairs "$pairs_file" --pm .5 --pr .5 \
        -o "$IDX_DIR/seed.s${snum}.m${min}.${base}.50.pairs" ||
        die "wf extract failed for $pairs_file (.50)"
}

for base in all dict; do
    extract_yes_pairs "$base" 3 10
    extract_yes_pairs "$base" 4 10
done

exit
}
