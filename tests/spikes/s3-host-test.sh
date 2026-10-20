#!/usr/bin/env bash
# E1 test of the S3 server prototype (read-write share) on Linux, with
# Samba's smbclient as the client: same SMB code as the PS5 build, but
# neither the PS5 nor Windows. Every check looks at the result on disk.
# Needs smbclient, curl and sha256sum. Runs without root (port 4451).
# Usage: s3-host-test.sh <path to the host build of s3-smbserver>
set -euo pipefail

BIN="$(realpath "${1:?usage: $0 <s3-smbserver host binary>}")"
EXPECTED="$(realpath "$(dirname "$0")/s2-expected.sha256")"
PORT=4451
PASS="s3-$RANDOM-$RANDOM"
WORK="$(mktemp -d)"
mkdir "$WORK/base" "$WORK/local"

"$BIN" "$WORK/base" "$PORT" "$PASS" >"$WORK/server.log" 2>&1 & server=$!
trap 'kill $server 2>/dev/null || true; echo "--- server log"; cat "$WORK/server.log"' EXIT

status() { curl -fsS --max-time 3 http://127.0.0.1:4445/; }
for _ in $(seq 300); do
  status 2>/dev/null | grep -q '^folder=/' && break
  sleep 0.1
done
folder="$(status | sed -n 's/^folder=//p')"
[ -d "$folder" ] || { echo "no test folder reported" >&2; exit 1; }

# Signing required, SMB 3.1.1. force_old_spnego: see s2-host-test.sh.
smb() {
  smbclient //127.0.0.1/s3 -p "$PORT" -U "prospero%$PASS" -m SMB3_11 \
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

# expect_fail <NT_STATUS pattern> <command...>: smbclient must report it.
expect_fail() {
  local want="$1"; shift
  local out
  out=$("$@" 2>&1) || true
  expect "$out" "$want"
}

listed() { ls "$folder/$1" | grep -qx -- "$2"; }

echo "== case 1: the share is announced read-write"
out=$(status)
expect "$out" '^writable=1$'
expect "$out" '^profile=b '
echo "case 1 passed"

echo "== case 2: upload of the whole S2 test set (1507 files, 65 MB)"
cp -r "$folder" "$WORK/local/vorlage"
(cd "$WORK/local" && smb -c 'prompt OFF; recurse ON; mput vorlage' >/dev/null)
(cd "$folder/vorlage" && sha256sum -c --quiet "$EXPECTED")
count=$(find "$folder/vorlage" -type f | wc -l)
need [ "$count" -eq 1507 ]
echo "case 2 passed"

echo "== case 3: overwriting truncates and keeps the existing spelling"
head -c 100000 /dev/urandom >"$WORK/gross.bin"
printf 'kurz\r\n' >"$WORK/kurz.txt"
smb -c "put $WORK/gross.bin Datei.TXT" >/dev/null
smb -c "put $WORK/kurz.txt datei.txt" >/dev/null
need [ "$(ls "$folder" | grep -ci '^datei.txt$')" -eq 1 ]
cmp "$WORK/kurz.txt" "$folder/Datei.TXT"
echo "case 3 passed"

echo "== case 4: folders"
smb -c 'mkdir Neu; mkdir Neu\Unter' >/dev/null
need [ -d "$folder/Neu/Unter" ]
expect_fail NT_STATUS_OBJECT_NAME_COLLISION smb -c 'mkdir NEU'
smb -c "put $WORK/kurz.txt Neu\\Unter\\x.txt" >/dev/null
expect_fail NT_STATUS_DIRECTORY_NOT_EMPTY smb -c 'rmdir Neu\Unter'
need [ -e "$folder/Neu/Unter/x.txt" ]
smb -c 'del Neu\Unter\x.txt' >/dev/null
smb -c 'rmdir Neu\Unter' >/dev/null
need [ -d "$folder/Neu" ]
need [ ! -e "$folder/Neu/Unter" ]
smb -c 'mkdir Baum; mkdir Baum\a; mkdir Baum\a\b' >/dev/null
smb -c "put $WORK/kurz.txt Baum\\a\\b\\y.txt; put $WORK/kurz.txt Baum\\z.txt" >/dev/null
need [ -e "$folder/Baum/a/b/y.txt" ]
smb -c 'deltree Baum' >/dev/null
need [ ! -e "$folder/Baum" ]
echo "case 4 passed"

echo "== case 5: rename, move and change of case"
smb -c "put $WORK/kurz.txt alt.txt" >/dev/null
smb -c 'rename alt.txt neu.txt' >/dev/null
need [ ! -e "$folder/alt.txt" ]
cmp "$WORK/kurz.txt" "$folder/neu.txt"
smb -c 'rename neu.txt NEU.txt' >/dev/null
need listed . 'NEU.txt'
need test -z "$(ls "$folder" | grep -x 'neu.txt' || true)"
smb -c 'rename NEU.txt Neu\verschoben.txt' >/dev/null
need [ -e "$folder/Neu/verschoben.txt" ]
need [ ! -e "$folder/NEU.txt" ]
smb -c "put $WORK/gross.bin anderes.bin" >/dev/null
expect_fail NT_STATUS_OBJECT_NAME_COLLISION smb -c 'rename anderes.bin Neu\verschoben.txt'
cmp "$WORK/kurz.txt" "$folder/Neu/verschoben.txt"
smb -c 'rename anderes.bin Neu\verschoben.txt -f' >/dev/null
cmp "$WORK/gross.bin" "$folder/Neu/verschoben.txt"
need [ ! -e "$folder/anderes.bin" ]
smb -c 'rename Neu Umbenannt' >/dev/null
need [ -e "$folder/Umbenannt/verschoben.txt" ]
need [ ! -e "$folder/Neu" ]
echo "case 5 passed"

echo "== case 6: delete"
smb -c 'del Umbenannt\verschoben.txt' >/dev/null
need [ ! -e "$folder/Umbenannt/verschoben.txt" ]
expect_fail 'NT_STATUS_OBJECT_NAME_NOT_FOUND\|NT_STATUS_NO_SUCH_FILE' smb -c 'del gibtsnicht.txt'
need [ -e "$folder/hello.txt" ]
echo "case 6 passed"

echo "== case 7: timestamps set by the client"
smb -c "put $WORK/kurz.txt zeit.txt" >/dev/null
TZ=UTC smb -c 'utimes zeit.txt -1 -1 2001:02:03-04:05:06 -1' >/dev/null
want=$(date -u -d '2001-02-03 04:05:06' +%s)
got=$(stat -c %Y "$folder/zeit.txt")
need [ "$got" -eq "$want" ]
echo "case 7 passed"

# smbclient may clean ".." on its own side, and a cleaned name lands in the
# share. What counts is that nothing appears outside the test folder.
echo "== case 8: writes cannot leave the share"
smb -c "put $WORK/kurz.txt ..\\..\\..\\entkommen.txt" >/dev/null 2>&1 || true
smb -c 'mkdir ..\..\..\entkommen' >/dev/null 2>&1 || true
need test -z "$(find "$WORK" -name 'entkommen*' -not -path "$folder/*")"
echo "case 8 passed"

echo "== case 9: invalid names are refused"
expect_fail NT_STATUS_ smb -c "put $WORK/kurz.txt a*b.txt"
need [ ! -e "$folder/a*b.txt" ]
echo "case 9 passed"

echo "== case 10: owner and mode of new entries, write counters"
out=$(status)
expect "$out" "^last_created=.* uid=$(id -u) "
expect "$out" '^delete_failures=0$'
expect "$out" '^lost_replies=0$'
expect "$out" '^files_created=[1-9]'
echo "$out"
curl -fsS --max-time 3 http://127.0.0.1:4445/quit >/dev/null
wait "$server"
trap - EXIT
echo "case 10 passed"
