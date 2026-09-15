# Workflow-aware `dfs-anagrams` CLI

## Summary

Replace the positional index and letters arguments with named options, add
workflow defaults and target-derived search parameters, and reuse a shared
workflow parser/resolver with the segment tools.

## Interface and implementation

- Change the synopsis to use `--idx INDEX`, `-l LETTERS`,
  `[--wf | --wfroot ROOT]`, and `[-t TARGET]`. Detailed help documents
  `-l/--letters` and `-t/--target`; the synopsis shows short forms only.
- Without workflow mode, require both `--idx` and `-l`. Reject all positional
  arguments.
- `--wf` requires nonempty `$WFROOT`; `--wfroot ROOT` never consults the
  environment; they remain mutually exclusive.
- With either workflow option:
  - Default `--idx` to `ROOT/.wf/best/idx/wiki-merged.2.idx`.
  - Default `--dict` to `ROOT/.wf/dict/words.filtered`.
  - Allow explicit `--idx` and `--dict` overrides.
  - Load global `ROOT/.wf/classified/no/no.pairs` plus existing explicit
    `--exclude-pairs` inputs.
  - Without `-t`, continue requiring `-l` and allow existing `-u`.
- With `-t`:
  - Require workflow mode and exact grammar
    `s[1-9]/[ou]-[a-z]+/m[1-9][0-9]*/g[1-9][0-9]*`.
  - Require the resolved target to be a directory beneath
    `ROOT/.wf/best` and require `ROOT/.wf/best/sN/letters`.
  - Derive `-m` and `-g` from the target; reject explicit `-m` or `-g`.
  - Reject both `-l` and existing `-u`; do not add an `-o` option.
  - Normalize the sentence file through the existing letter cleanup. For
    `u-TEXT`, subtract `TEXT`; for `o-TEXT`, search exactly `TEXT`. Validate
    that the named letters form a proper multiset subset of the sentence.
  - Also load `ROOT/.wf/best/TARGET/no.pairs` when present.
- Extract workflow root/target parsing, strict target validation, canonical
  below-`best` resolution, and path construction into a reusable helper.
  Retain segment tools' existing `current` inference, while `dfs-anagrams`
  resolves a target only when `-t` is explicit.
- Leave `query-index` unchanged. Preserve the unrelated `docs/todo`
  modification and untracked `docs/select-segment.md`.

## Tests and validation

- Mechanically migrate existing `dfs-anagrams` calls in `test-dfs-cli.sh`
  and `test-dfs-cli-differential.sh` to `--idx ... -l ...`; keep
  `query-index` calls positional.
- Add a compact workflow fixture covering direct required arguments, legacy
  positional rejection, root selection, defaults and overrides, implicit
  global/target NO exclusions, `u-` and `o-` derivation, locked `mN/gN`, and
  malformed/missing targets or sentence letters.
- Verify `-l` remains valid with workflow mode when no target is supplied,
  and is rejected with `-t`.
- Using Terra for test work, compile the affected tools and run:
  - `meson test -C build dfs-cli filter-segments first-segments top-segments`
    `--print-errorlogs`
  - `meson test -C build dfs-cli-differential --print-errorlogs` only when
    `IDX` names a usable real index; otherwise report its intentional skip.
  - `git diff --check`
- Run `/review` after implementation and validation, before any commit. No
  commit or `/home/mike/code/words` changes are included.
