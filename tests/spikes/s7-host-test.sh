#!/usr/bin/env bash
# E1 test of the S7 server prototype on Linux: the UDP discovery protocol
# (discovery.c) next to the read-only share s7. A small Python client sends
# the requests. Needs python3, smbclient and curl. Runs without root (SMB
# port 4453, UDP 4446).
# Usage: s7-host-test.sh <path to the host build of s7-smbserver>
set -euo pipefail

BIN="$(realpath "${1:?usage: $0 <s7-smbserver host binary>}")"
PORT=4453
PASS="s7-$RANDOM-$RANDOM"
WORK="$(mktemp -d)"
mkdir "$WORK/base"

"$BIN" "$WORK/base" "$PORT" "$PASS" >"$WORK/server.log" 2>&1 & server=$!
trap 'kill $server 2>/dev/null || true; echo "--- server log"; cat "$WORK/server.log"; rm -rf "$WORK"' EXIT

status() { curl -fsS --max-time 3 http://127.0.0.1:4445/; }
for _ in $(seq 300); do
  status 2>/dev/null | grep -q '^folder=/' && break
  sleep 0.1
done

need() {
  if ! "$@"; then
    echo "check failed: $*" >&2
    exit 1
  fi
}

expect() {
  if ! grep -q -- "$2" <<<"$1"; then
    echo "expected '$2' in:" >&2
    echo "$1" >&2
    exit 1
  fi
}

# udp <request line, \n added>: prints the reply, or nothing after 1 s.
udp() {
  python3 - "$1" <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(1.0)
s.sendto((sys.argv[1] + "\n").encode(), ("127.0.0.1", 4446))
try:
    print(s.recv(1024).decode().rstrip("\n"))
except socket.timeout:
    pass
PY
}
now_ms() { python3 -c 'import time; print(int(time.time() * 1000))'; }

echo "== case 1: status reports the discovery"
out=$(status)
expect "$out" '^PROSPERO-S7 OK$'
expect "$out" '^share=s7$'
expect "$out" '^writable=0$'
expect "$out" '^discovery_id=[0-9a-f]\{16\}$'
id=$(sed -n 's/^discovery_id=//p' <<<"$out")
echo "case 1 passed"

echo "== case 2: a valid request gets the reply, skew measured"
pc=$(( $(now_ms) - 3600000 ))
rep=$(udp "PROSPERO-DISCOVER 1 nonce=00ff12ab utc_ms=$pc")
echo "$rep"
expect "$rep" "^PROSPERO-HERE 1 nonce=00ff12ab id=$id smb_port=$PORT utc_ms=[0-9]* skew_ms=[0-9]* name=Prospero S7$"
skew=$(sed -n 's/.* skew_ms=\([0-9-]*\) .*/\1/p' <<<"$rep")
need [ "$skew" -ge 3595000 ]
need [ "$skew" -le 3605000 ]
echo "case 2 passed"

echo "== case 3: anything else gets no reply"
for bad in "PROSPERO-DISCOVER 2 nonce=00ff utc_ms=1" \
           "PROSPERO-DISCOVER 1 nonce=00FF utc_ms=1" \
           "PROSPERO-DISCOVER 1 nonce= utc_ms=1" \
           "PROSPERO-DISCOVER 1 nonce=00ff" \
           "PROSPERO-DISCOVER 1 nonce=00ff utc_ms=12x" \
           "PROSPERO-DISCOVER 1 nonce=00ff utc_ms=1 extra=1" \
           "PROSPERO-DISCOVER 1 nonce=0123456789abcdef0123456789abcdef0 utc_ms=1" \
           "hello"; do
  rep=$(udp "$bad")
  need test -z "$rep"
done
out=$(status)
expect "$out" '^discovery_requests=9$'
expect "$out" '^discovery_replies=1$'
expect "$out" '^discovery_invalid=8$'
expect "$out" '^discovery_last_peer=127\.0\.0\.1:'
echo "case 3 passed"

echo "== case 4: the share still works"
smbclient //127.0.0.1/s7 -p "$PORT" -U "prospero%$PASS" -m SMB3_11 \
  --client-protection=sign --option='ntlmssp_client:force_old_spnego=yes' \
  -c 'get hello.txt '"$WORK/hello.txt" >/dev/null
cmp "$WORK/hello.txt" <(printf 'Prospero AutoDrive S2\r\n')
echo "$(status)" | grep '^discovery'
curl -fsS --max-time 3 http://127.0.0.1:4445/quit >/dev/null
wait "$server"
trap - EXIT
rm -rf "$WORK"
echo "case 4 passed"
