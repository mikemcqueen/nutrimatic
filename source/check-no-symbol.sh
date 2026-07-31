#!/usr/bin/env bash
set -euo pipefail

nm_command=$1
binary=$2
stamp=$3
symbol=visit_fitting_class

# Do not use grep -q here: it closes the pipe as soon as it finds a match,
# making nm fail with SIGPIPE under pipefail and hiding the match.
if "$nm_command" -C --defined-only "$binary" | grep -F "$symbol" >/dev/null; then
  echo "error: $binary defines forbidden symbol $symbol" >&2
  exit 1
fi

touch "$stamp"
