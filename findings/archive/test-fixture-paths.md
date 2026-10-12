# Test Fixture Paths

## Problem

`test-expr.cpp` wrote its index fixture as `test-expr.index` in the current
working directory. Meson runs tests with the build directory as the working
directory, so the fixture landed in the build tree, and it was removed only on
the success path. Every failing run left the file behind, and the fixed name
meant two concurrent runs of the binary would overwrite each other's index
between the write and the read.

`test-measure-coherence.cpp` created a unique directory with `mkdtemp`, which
avoided the name collision, but it hardcoded `/tmp` instead of honoring
`$TMPDIR`, and it removed three files by name and then the directory only at
the end of `main`. Because `check()` exits on the first failure, every failing
run leaked a directory.

The other C++ tests use `tmpfile()`, which is anonymous and unlinked on close,
and the shell tests already use `mktemp -d` under `$TMPDIR` with a
`trap cleanup EXIT`. Neither group needed a change.

## Approach

`test-temp-dir.h` supplies `TestTempDir`, which creates one unique directory
per instance under `$TMPDIR` and removes the tree in its destructor.

The `$TMPDIR` lookup mirrors `::testing::TempDir()` so that a later migration
to GoogleTest can drop the helper and substitute the framework call without
changing how fixture paths are spelled. GoogleTest's `TempDir()` returns a
shared directory rather than a unique one, so the per-instance `mkdtemp`
subdirectory is still required after that swap.

The destructor alone is not enough while the tests still call `exit(1)` on the
first failure, because that path runs no destructors. The constructor also
registers an `atexit` hook over a registry of live directories. The registry is
a function-local static constructed before the first `atexit` call, so it
outlives the hook. Once the tests move to GoogleTest's non-fatal assertions the
hook becomes redundant, since cases return normally and the destructor runs.
