## SETUP

source ./setup.sh

## BUILD

conan build .

source build/dep-info/conanbuild.sh # for meson, ninja

## REVIEW

always /code-review (claude) or /review (codex) code before committing

## TESTS

keep tests minimal -- smoke tests -- unless otherwise instructed.  I'm more interested
in implementation than test coverage.

## PERF TESTING

There may be other instances of query-index or dfs-anagrams running. Check for
both in the host process table, outside any sandbox PID namespace, before
every run when accurate timing data matters.
