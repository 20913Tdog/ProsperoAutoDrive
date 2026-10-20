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

# smbc <share> <user%password> <args...>: signing required, SMB 3.1.1 unless
# a test asks for another dialect.
# libsmb2 sends no SPNEGO mechListMIC. Samba's client insists on one as soon
# as NTLM carries a MIC; force_old_spnego makes it leave the MIC out. Whether
# Windows needs the mechListMIC is part of the live test (E3).
smbc() {
  local share="$1" cred="$2"
  shift 2
  smbclient "//127.0.0.1/$share" -p "$PORT" -U "$cred" --client-protection=sign \
    --option='ntlmssp_client:force_old_spnego=yes' "$@"
}

smb() { smbc s2 "prospero%$PASS" "$@"; }

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

echo "== case 3: dialects 2.0.2 to 3.0.2"
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
expect_fail NT_STATUS_LOGON_FAILURE smbc s2 'prospero%falsch' -c ls
expect_fail NT_STATUS_BAD_NETWORK_NAME smbc gibtsnicht "prospero%$PASS" -c ls
echo "case 8 passed"

echo "== case 9: file and volume information"
out=$(smb -c 'allinfo hello.txt' 2>&1)
expect "$out" 'attributes: A'
expect "$out" 'stream: \[::\$DATA\], 23 bytes'
out=$(smb -c 'volume' 2>&1)
expect "$out" 'Volume: |PS5|'
echo "case 9 passed"

echo "== case 10: speed-diagnosis profiles (S2b)"
for p in b c a; do
  curl -fsS --max-time 3 "http://127.0.0.1:4445/profile/$p" | grep -q 'PROSPERO-S2 PROFILE'
  curl -fsS --max-time 3 http://127.0.0.1:4445/reset >/dev/null
  smb -c "get pattern-1MiB.bin $WORK/profile-$p.bin" >/dev/null
  cmp "$WORK/profile-$p.bin" "$folder/pattern-1MiB.bin"
  out=$(status)
  expect "$out" "^profile=$p "
  expect "$out" '^bytes_read=1048576$'
  if [ "$p" = c ]; then
    expect "$out" '^dialect=0x0210$'
  else
    expect "$out" '^dialect=0x0311$'
  fi
done
curl -fsS --max-time 3 http://127.0.0.1:4445/profile/x | grep -q 'UNKNOWN PROFILE'
echo "case 10 passed"

echo "== case 11: password handed out exactly once (Run-Test.cmd)"
expect "$(status)" '^password_given=0$'
out=$(curl -fsS --max-time 3 http://127.0.0.1:4445/password)
expect "$out" "^password=$PASS\$"
out=$(curl -fsS --max-time 3 http://127.0.0.1:4445/password)
expect "$out" '^PROSPERO-S2 PASSWORD ALREADY GIVEN$'
if grep -q -- "$PASS" <<<"$out"; then
  echo "password handed out twice" >&2
  exit 1
fi
expect "$(status)" '^password_given=1$'
echo "case 11 passed"

echo "== case 12: status page and stop via control port"
out=$(status)
expect "$out" '^sessions=[1-9]'
expect "$out" '^lost_replies=0'
expect "$out" "^now_utc=$(date -u +%Y-%m-%d)T"
echo "$out"
curl -fsS --max-time 3 http://127.0.0.1:4445/quit >/dev/null
wait "$server"
trap - EXIT
echo "case 12 passed"
