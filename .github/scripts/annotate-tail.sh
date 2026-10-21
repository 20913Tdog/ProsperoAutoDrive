#!/usr/bin/env bash
# Turns the important lines of a CI log into one annotation. Job logs need
# a login to read, annotations of a public repo do not, so results can be
# read from the anonymous API as well.
# Usage: annotate-tail.sh <log file> [error|notice] [title]
#   error (default): error lines first, then the end of the log
#   notice: the whole log, for results that are already short
set -euo pipefail

log="${1:?usage: $0 <log file> [error|notice] [title]}"
level="${2:-error}"
title="${3:-log tail}"
[ -s "$log" ] || exit 0

# GitHub cuts annotations at about 4 KB. Blank lines and ninja progress
# lines are dropped. head ends its pipe early on a long log; that SIGPIPE
# is expected, so pipefail is off from here on.
set +o pipefail
filtered=$(grep -v -e '^[[:space:]]*$' -e '^\[[0-9]*/[0-9]*\] ' "$log" || true)
if [ "$level" = notice ]; then
  body=$(cut -c1-300 <<<"$filtered" | head -c 3800)
else
  body=$({
    grep -E -i 'error|fail|warning:|expected|undefined|falsch|fehler' <<<"$filtered" | head -n 25 || true
    echo "----- end of log -----"
    tail -n 20 <<<"$filtered"
  } | cut -c1-300 | head -c 3800)
fi

# Workflow commands need %, CR and LF escaped.
body=$(sed -e 's/%/%25/g' -e 's/\r/%0D/g' <<<"$body" | awk 'BEGIN { ORS = "%0A" } { print }')
echo "::${level} title=${title}::$body"
