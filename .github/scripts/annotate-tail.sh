#!/usr/bin/env bash
# Turns the last lines of a CI log into one error annotation. Job logs need
# a login to read, annotations of a public repo do not, so failures can be
# diagnosed from the anonymous API as well.
# Usage: annotate-tail.sh <log file> [lines]
set -euo pipefail

log="${1:?usage: $0 <log file> [lines]}"
lines="${2:-80}"
[ -s "$log" ] || exit 0

# Workflow commands need %, CR and LF escaped. Blank lines are dropped.
body=$(grep -v '^\s*$' "$log" | tail -n "$lines" |
       sed -e 's/%/%25/g' -e 's/\r/%0D/g' | awk 'BEGIN { ORS = "%0A" } { print }')
echo "::error title=log tail::$body"
