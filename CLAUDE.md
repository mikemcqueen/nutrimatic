## BUILD

source ~/code/nutrimatic/.env/bin/activate
conan build .

source build/dep-info/conanbuild.sh # for meson, ninja

## REVIEW

always /code-review (claude) or /review (codex) code before committing

## TESTS

keep tests minimal -- smoke tests -- unless otherwise instructed.  I'm more interested
in implementation than test coverage.

## INDEX FILE

export IDX=~/code/nutrimatic/idx/wiki-merged.5.index

and use $IDX as index file parameter when calling programs; load env("IDX") from tests.

## LETTERS

to get letters in the S6 environment variable use:

source ./s.sh

you can then use ${S6:0:N} for various letter counts.

## CONCURRENT SESSIONS

There may be other instances of dfs-anagrams running. Check before you run an instance if you're accurate timing data.
