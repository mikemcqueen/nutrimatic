#!/usr/bin/env bash

set -euo pipefail

m=4
s=${S3:?S3 is not set; source ./setup.sh first}
s_idx=${S3_IDX:?S3_IDX is not set; source ./setup.sh first}
dict=/usr/share/dict/words.big

# Aliases are not expanded inside scripts, so fall back to the programs behind
# the interactive lettercount and freqsort aliases.
lettercount_command=${LETTERCOUNT:-}
if [[ -z $lettercount_command ]]; then
    lettercount_command=$(command -v lettercount || true)
fi
if [[ -z $lettercount_command && -x $HOME/code/clues/src/tools/lc.sh ]]; then
    lettercount_command=$HOME/code/clues/src/tools/lc.sh
fi

freqsort_command=${FREQSORT:-}
if [[ -z $freqsort_command ]]; then
    freqsort_command=$(command -v freqsort || true)
fi
if [[ -z $freqsort_command && -x $HOME/code/clues/src/tools/freqsort.sh ]]; then
    freqsort_command=$HOME/code/clues/src/tools/freqsort.sh
fi

if [[ -z $lettercount_command || -z $freqsort_command ]]; then
    echo "lettercount and freqsort must be installed or set via LETTERCOUNT and FREQSORT" >&2
    exit 127
fi
if [[ ! -r $dict ]]; then
    echo "dictionary is not readable: $dict" >&2
    exit 1
fi
if [[ ! -r $s_idx ]]; then
    echo "index is not readable: $s_idx" >&2
    exit 1
fi

# lettercount sorts by frequency factor, so the letter on its final row is the
# most over-frequent one.
l=$(
    "$lettercount_command" "$s" |
        tail -n 1 |
        awk '$1 ~ /^\047[[:alpha:]]\047$/ { print substr($1, 2, 1) }'
)
if [[ -z $l ]]; then
    echo "could not extract the most over-frequent letter from lettercount" >&2
    exit 1
fi

candidate_file=$(mktemp "${TMPDIR:-/tmp}/freq-find.XXXXXX")
trap 'rm -f "$candidate_file"' EXIT

# -w makes freqsort emit only words. Discard its status line and retain words
# containing the selected letter.
"$freqsort_command" -m "$m" -w "$s" "$dict" |
    awk -v letter="$l" '
        /^ignoring: / { next }
        $0 == "no results" { next }
        index($0, letter) { print }
    ' > "$candidate_file"

# S_IDX rows are "<frequency> <word or phrase>". Record exact candidate
# matches while scanning the index once, then emit them in reverse freqsort
# order.
awk '
    FILENAME == ARGV[1] {
        ordered[++count] = $0
        wanted[$0] = 1
        next
    }
    {
        word = $0
        sub(/^[^[:space:]]+[[:space:]]+/, "", word)
        if (word in wanted)
            found[word] = 1
    }
    END {
        for (i = count; i >= 1; --i)
            if (ordered[i] in found)
                print ordered[i]
    }
' "$candidate_file" "$s_idx"
