#!/usr/bin/env bash
# E1 test of the S4 server prototype (throughput spike) on Linux, with
# Samba's smbclient as the client. It checks what S4-Check.cmd relies on:
# the 256 MiB file, reading and overwriting it over SMB, the switch to
# profile c (SMB 2.1) and the counters. It measures nothing; speed is E3.
# Needs smbclient, curl and sha256sum. Runs without root (port 4452).
# Usage: s4-host-test.sh <path to the host build of s4-smbserver>
set -euo pipefail

BIN="$(realpath "${1:?usage: $0 <s4-smbserver host binary>}")"
# SHA-256 of the 256 MiB xorshift stream (testdata.c); its first 64 MiB are
# pattern-64MiB.bin in s2-expected.sha256.
BIG_SHA=3cb78104e219bba5bcb358a737572d2b3be5bebfe38b96f60973d2033f0526f7
PORT=4452
PASS="s4-$RANDOM-$RANDOM"
WORK="$(mktemp -d)"
mkdir "$WORK/base"

"$BIN" "$WORK/base" "$PORT" "$PASS" >"$WORK/server.log" 2>&1 & server=$!
trap 'kill $server 2>/dev/null || true; echo "--- server log"; cat "$WORK/server.log"; rm -rf "$WORK"' EXIT

status() { curl -fsS --max-time 3 http://127.0.0.1:4445/; }
for _ in $(seq 600); do
  status 2>/dev/null | grep -q '^folder=/' && break
  sleep 0.1
done
folder="$(status | sed -n 's/^folder=//p')"
[ -d "$folder" ] || { echo "no test folder reported" >&2; exit 1; }

# Signing required. force_old_spnego: see s2-host-test.sh.
smb() {
  smbclient //127.0.0.1/s4 -p "$PORT" -U "prospero%$PASS" -m SMB3_11 \
    --client-protection=sign --option='ntlmssp_client:force_old_spnego=yes' "$@"
}

# need <test...>: like a plain test, but also stops the script inside
# && lists and after "!", where set -e does not.
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

sha() { sha256sum "$1" | cut -d' ' -f1; }

echo "== case 1: share s4, read-write, profile b"
out=$(status)
expect "$out" '^PROSPERO-S4 OK$'
expect "$out" '^share=s4$'
expect "$out" '^writable=1$'
expect "$out" '^profile=b '
echo "case 1 passed"

echo "== case 2: the 256 MiB file on disk"
need [ "$(stat -c %s "$folder/pattern-256MiB.bin")" -eq 268435456 ]
need [ "$(sha "$folder/pattern-256MiB.bin")" = "$BIG_SHA" ]
echo "case 2 passed"

echo "== case 3: read it over SMB"
curl -fsS --max-time 3 http://127.0.0.1:4445/reset >/dev/null
smb -c "get pattern-256MiB.bin $WORK/lesen.bin" >/dev/null
need [ "$(sha "$WORK/lesen.bin")" = "$BIG_SHA" ]
out=$(status)
expect "$out" '^bytes_read=268435456$'
expect "$out" '^cpu_user_ms=[0-9]'
echo "case 3 passed"

echo "== case 4: write it twice to the same name (overwrite)"
for _ in 1 2; do
  smb -c "put $WORK/lesen.bin schreiben-smb.bin" >/dev/null
  need [ "$(stat -c %s "$folder/schreiben-smb.bin")" -eq 268435456 ]
done
cmp "$WORK/lesen.bin" "$folder/schreiben-smb.bin"
expect "$(status)" '^bytes_written=536870912$'
echo "case 4 passed"

echo "== case 5: profile c negotiates SMB 2.1"
expect "$(curl -fsS --max-time 3 http://127.0.0.1:4445/profile/c)" 'PROFILE'
smb -c "get pattern-1MiB.bin $WORK/klein.bin" >/dev/null
cmp "$WORK/klein.bin" "$folder/pattern-1MiB.bin"
out=$(status)
expect "$out" '^profile=c '
expect "$out" '^dialect=0x0210$'
expect "$out" '^lost_replies=0$'
echo "$out"
curl -fsS --max-time 3 http://127.0.0.1:4445/quit >/dev/null
wait "$server"
trap - EXIT
rm -rf "$WORK"
echo "case 5 passed"
