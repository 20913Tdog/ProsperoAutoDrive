#!/usr/bin/env bash
# E1 test of the S1 port probe on Linux: same socket code as the PS5 build,
# but not the PS5 itself. Needs sudo (port 445) and curl.
# Usage: s1-host-test.sh <path to the host build of s1-portprobe>
set -euo pipefail

BIN="$(realpath "${1:?usage: $0 <s1-portprobe host binary>}")"

wait_http() {
  for _ in $(seq 100); do
    curl -s -o /dev/null --max-time 1 "http://127.0.0.1:$1/" && return 0
    sleep 0.1
  done
  echo "port $1 did not come up" >&2
  return 1
}

expect() {
  if ! grep -q -- "$2" <<<"$1"; then
    echo "expected '$2' in:" >&2
    echo "$1" >&2
    exit 1
  fi
}

echo "== case 1: both ports free"
sudo "$BIN" & probe=$!
wait_http 4445
body445=$(curl -fsS --max-time 3 http://127.0.0.1:445/)
body4445=$(curl -fsS --max-time 3 http://127.0.0.1:4445/)
expect "$body445" '^PROSPERO-S1 OK'
expect "$body445" '^connected_port=445'
expect "$body445" '^bind 445: OK'
expect "$body4445" '^bind 4445: OK'
expect "$body4445" '^uid=0 euid=0'
curl -fsS --max-time 3 http://127.0.0.1:4445/quit >/dev/null
wait "$probe"
echo "case 1 passed"

echo "== case 2: 445 already taken by a foreign service"
sudo python3 -m http.server 445 --bind 0.0.0.0 >/dev/null 2>&1 &
wait_http 445
sudo "$BIN" & probe=$!
wait_http 4445
body4445=$(curl -fsS --max-time 3 http://127.0.0.1:4445/)
expect "$body4445" '^bind 445: FAIL errno='
expect "$body4445" '^bind 4445: OK'
curl -fsS --max-time 3 http://127.0.0.1:4445/quit >/dev/null
wait "$probe"
sudo pkill -f 'http.server 445' || true
echo "case 2 passed"

echo "== case 3: a second start replaces the first instance"
sudo "$BIN" & first=$!
wait_http 4445
sudo "$BIN" & second=$!
wait "$first"
wait_http 4445
body445=$(curl -fsS --max-time 3 http://127.0.0.1:445/)
expect "$body445" '^bind 445: OK'
curl -fsS --max-time 3 http://127.0.0.1:4445/quit >/dev/null
wait "$second"
echo "case 3 passed"
