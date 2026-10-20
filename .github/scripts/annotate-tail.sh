#!/usr/bin/env bash
# Turns the important lines of a CI log into one error annotation. Job logs
# need a login to read, annotations of a public repo do not, so failures can
# be diagnosed from the anonymous API as well.
# Usage: annotate-tail.sh <log file>
set -euo pipefail

log="${1:?usage: $0 <log file>}"
[ -s "$log" ] || exit 0

# GitHub cuts annotations at about 4 KB: error lines first, then the end of
# the log, without blank lines and ninja progress lines.
filtered=$(grep -v -e '^[[:space:]]*$' -e '^\[[0-9]*/[0-9]*\] ' "$log" || true)
body=$({
  grep -E -i 'error|fail|warning:|expected|undefined' <<<"$filtered" | head -n 25 || true
  echo "----- end of log -----"
  tail -n 20 <<<"$filtered"
} | cut -c1-300 | head -c 3800)

# Workflow commands need %, CR and LF escaped.
body=$(sed -e 's/%/%25/g' -e 's/\r/%0D/g' <<<"$body" | awk 'BEGIN { ORS = "%0A" } { print }')
echo "::error title=log tail::$body"
