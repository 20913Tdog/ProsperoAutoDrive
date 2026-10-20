#!/usr/bin/env bash
# E1 test of the S2 server prototype on Linux, with Samba's smbclient as the
# client: same SMB code as the PS5 build, but neither the PS5 nor Windows.
# Needs smbclient, curl and sha256sum. Runs without root (port 4450).
# Usage: s2-host-test.sh <path to the host build of s2-smbserver>
set -euo pipefail

BIN="$(realpath "${1:?usage: $0 <s2-smbserver host binary>}")"
EXPECTED="$(realpath "$(dirname "$0")/s2-expected.sha256")"
PORT=4450
PASS="s2-$RANDOM-$RANDOM"
WORK="$(mktemp -d)"
mkdir "$WORK/base" "$WORK/dl"

"$BIN" "$WORK/base" "$PORT" "$PASS" >"$WORK/server.log" 2>&1 & server=$!
trap 'kill $server 2>/dev/null || true; echo "--- server log"; cat "$WORK/server.log"' EXIT

status() { curl -fsS --max-time 3 http://127.0.0.1:4445/; }
for _ in $(seq 300); do
  status 2>/dev/null | grep -q '^folder=/' && break
  sleep 0.1
done
folder="$(status | sed -n 's/^folder=//p')"
[ -d "$folder" ] || { echo "no test folder reported" >&2; exit 1; }

# Signing required, SMB 3.1.1 unless a test asks for another dialect.
smb() {
  smbclient "//127.0.0.1/s2" -p "$PORT" -U "prospero%$PASS" \
    --client-protection=sign "$@"
}

# expect_fail <NT_STATUS text> <command...>: smbclient must report the error.
expect_fail() {
  local want="$1"; shift
  local out
  out=$("$@" 2>&1) || true
  expect "$out" "$want"
}

expect() {
  if ! grep -q -- "$2" <<<"$1"; then
    echo "expected '$2' in:" >&2
    echo "$1" >&2
    exit 1
  fi
}

echo "== case 1: generated content matches the expected hashes"
(cd "$folder" && sha256sum -c --quiet "$EXPECTED")
echo "case 1 passed"

echo "== case 2: full recursive download over SMB 3.1.1 with signing"
(cd "$WORK/dl" && smb -m SMB3_11 -c 'recurse ON; prompt OFF; mget *' >/dev/null)
(cd "$WORK/dl" && sha256sum -c --quiet "$EXPECTED")
diff -r "$folder" "$WORK/dl"
echo "case 2 passed"

echo "== case 3: SMB 2.0.2 and 2.1 dialects"
for proto in SMB2_02 SMB2_10 SMB3_00 SMB3_02; do
  smb --option="client min protocol=$proto" -m "$proto" \
    -c "get hello.txt $WORK/hello-$proto.txt" >/dev/null
  cmp "$WORK/hello-$proto.txt" "$folder/hello.txt"
done
echo "case 3 passed"

echo "== case 4: directory listing with 1500 entries (paging)"
count=$(smb -c 'ls viele\*' | grep -c 'datei-[0-9]\{4\}-')
[ "$count" -eq 1500 ] || { echo "listed $count of 1500" >&2; exit 1; }
echo "case 4 passed"

echo "== case 5: case-insensitive lookup"
smb -c "get mixedcase.txt $WORK/mc.txt" >/dev/null
cmp "$WORK/mc.txt" "$folder/MixedCase.txt"
smb -c "get TIEF\\1\\2\\3\\4\\5\\6\\7\\8\\ENDE.TXT $WORK/ende.txt" >/dev/null
cmp "$WORK/ende.txt" "$folder/tief/1/2/3/4/5/6/7/8/ende.txt"
echo "case 5 passed"

echo "== case 6: share is read-only"
echo x >"$WORK/neu.txt"
expect_fail NT_STATUS_ACCESS_DENIED smb -c "put $WORK/neu.txt neu.txt"
expect_fail NT_STATUS_ACCESS_DENIED smb -c 'mkdir neu'
expect_fail NT_STATUS_ACCESS_DENIED smb -c 'del hello.txt'
expect_fail NT_STATUS_ACCESS_DENIED smb -c 'rename hello.txt x.txt'
[ ! -e "$folder/neu.txt" ] && [ ! -e "$folder/neu" ] && [ -e "$folder/hello.txt" ]
echo "case 6 passed"

# smbclient may clean ".." on its own side; the backend check is the real one.
echo "== case 7: paths cannot leave the share"
expect_fail NT_STATUS_ smb -c "get ..\\..\\..\\..\\etc\\hostname $WORK/escape.txt"
[ ! -e "$WORK/escape.txt" ]
echo "case 7 passed"

echo "== case 8: wrong password and unknown share are rejected"
expect_fail NT_STATUS_LOGON_FAILURE smbclient //127.0.0.1/s2 -p "$PORT" \
  -U 'prospero%falsch' --client-protection=sign -c ls
expect_fail NT_STATUS_BAD_NETWORK_NAME smbclient //127.0.0.1/gibtsnicht -p "$PORT" \
  -U "prospero%$PASS" --client-protection=sign -c ls
echo "case 8 passed"

echo "== case 9: file and volume information"
out=$(smb -c 'allinfo hello.txt' 2>&1)
expect "$out" 'attributes: A'
expect "$out" 'stream: \[::\$DATA\], 23 bytes'
out=$(smb -c 'volume' 2>&1)
expect "$out" 'Volume: |PS5|'
echo "case 9 passed"

echo "== case 10: status page and stop via control port"
out=$(status)
expect "$out" '^sessions=[1-9]'
expect "$out" '^lost_replies=0'
echo "$out"
curl -fsS --max-time 3 http://127.0.0.1:4445/quit >/dev/null
wait "$server"
trap - EXIT
echo "case 10 passed"
