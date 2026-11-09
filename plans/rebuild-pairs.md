# rebuild-pairs

`rebuild-pairs.py` builds the intermediate files of
docs/new-pairs-workflow.md. A file's directory under `tmp/sN` says how it was
made, and a path given on the command line is the build request.

## Paths

A path alternates steps and cuts, and starts with a step:

    pb3/2k/pb3/pp-1sq-5k/pb2wb

* `pbN`: bag-stats-cuda `--ss pb,...,pb --uc`
* `pbNwb`: bag-stats-cuda `--ss pb,...,pb,wb,p`, sorted as in step 5a
* `Nk`: seed pairs from the top N thousand words of the step above it
* `pp-NAME-Nk`: the top N thousand pairs of the step above it, by the
  `pair-prior` score under the manifest `idx/pp.NAME`

A step takes three optional suffixes, in this order: `pb3-big-x15-glllpu`.

* `-dict`, `-big`: `seed-pairs -d` with `$DICT` or `$BIG`, keeping only seeds
  with both words in it; without it seeds have no dictionary filter. Accepted
  on a root `pbN` or `pbNwb` only. bag-stats-cuda's `DICT` argument stays
  `$DICT` either way. A forced pair with a word outside the dictionary is
  dropped, with a warning
* `-xN`: the step's input seeds go through `pfilter -xN` first, a cap of N
  letters per pair; without it they have no cap. Accepted on a root `pbN` only
* `-LETTERS`: bag-stats-cuda `-u LETTERS`, in sorted order and within the
  sentence

`-xN` and `-LETTERS` apply to that step alone: a cut below it isn't filtered by
them, so it can feed steps with different suffixes. `-dict` and `-big` carry
down: a cut's words come from the step's pairs, so they are all in the
dictionary.

The first step's seeds are `seed-pairs -sN -m4`; a later step's are the `pairs`
file of the cut above it.

## Files

    pbN     pairs.ranked  words.ranked  words
    pbNwb   dump  pairs.ranked  words.ranked  words
    Nk      pairs
    pp-NAME-Nk  pairs

`pairs.ranked` is `COUNT word word`, `words.ranked` is `COUNT word`, `pairs` is
`word,word`, and `words` is one word per line. Any step can be cut and fed
forward.

## Commands

    rebuild-pairs.py sN PATH...         build PATH and whatever it needs
    rebuild-pairs.py -n sN PATH...      print what would run
    rebuild-pairs.py -f sN PATH...      also rerun the steps of each PATH
    rebuild-pairs.py --status sN        the tree: files, line counts, stale steps
    rebuild-pairs.py --report sN        each tmp/sN/targets word's rank per node
    rebuild-pairs.py --adopt sN         move the old flat names into the tree

## Rebuilds

Each node's `.manifest.json` records, per file, its command and the size and
mtime of its inputs. A step reruns when an output is missing, its command or an
input changed, or a step it depends on reruns. `$DICT`, the seed pair file, and
a `pp` cut's `idx/pp.NAME`, `idx/p1.idx` and `cluer/data/cluedata` are
recorded and flagged by `--status` when they change, and don't cause a rebuild.
Adopted steps have no recorded command, so only their inputs count.

A root step's `bag-stats-cuda` run is the exception: without `-f` naming that
root, it runs only when its output is missing. If it is otherwise out of date,
by a changed command, a missing record or a changed recorded file, the run
warns with the reason and leaves it alone, and the steps after it don't rerun
because of it.

Outputs are written to `FILE.tmp` and renamed. Each run holds its sentence's
lock, so a second run on that sentence waits while runs on other sentences
proceed. `bag-stats-cuda` steps also hold a global GPU lock for the step's
duration, so GPU jobs never overlap.
