#!/usr/bin/env bash
# E1 test of prospero-smbd (P2.1, P2.2) on Linux: configuration, ring log,
# service channel on TCP 4445 with pairing and PING, discovery on UDP
# 4445, one instance, and the share through smbclient. Writes only into a
# new _ProsperoAutoDrive_Test_* folder below a temporary share root. Needs
# python3, smbclient and curl. Runs without
# root (SMB port 4454).
# Usage: host-test.sh <path to the host build of prospero-smbd>
set -euo pipefail

BIN="$(realpath "${1:?usage: $0 <prospero-smbd host binary>}")"
PORT=4454
WORK="$(mktemp -d)"
ROOT="$WORK/root"
STATE="$WORK/state"
mkdir "$ROOT"
server=

start() {
  # The trailing slash makes the share root end in "/" like "/" on the
  # console, so the path joins in the backend take that branch.
  "$BIN" "$ROOT/" "$STATE" "$PORT" >>"$WORK/server.log" 2>&1 & server=$!
  for _ in $(seq 300); do
    status 2>/dev/null | grep -q '^PROSPERO-SMBD OK$' && return 0
    sleep 0.1
  done
  echo "server did not come up" >&2
  exit 1
}
cleanup() {
  [ -n "$server" ] && kill "$server" 2>/dev/null || true
  echo "--- server output"; cat "$WORK/server.log"
  echo "--- log.txt"; cat "$STATE/log.txt" 2>/dev/null || true
  rm -rf "$WORK"
}
trap cleanup EXIT

status() { curl -fsS --max-time 3 http://127.0.0.1:4445/; }

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

# svc <source address> <line>...: sends the lines on one connection and
# prints every reply line until the server has answered all of them (a
# STATUS reply ends with END 1) or closes. In a line, @NOW becomes the
# time in ms (@NOW-5000 earlier), and a final " mac=@MAC" the HMAC-SHA256
# with $KEY over "<nonce from HELLO> <line before mac=>", as the agent
# will compute it.
svc() {
  KEY="${KEY:-}" python3 - "$@" <<'PY'
import hashlib, hmac, os, re, socket, sys, time
src, lines = sys.argv[1], sys.argv[2:]
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind((src, 0))
s.settimeout(3.0)
s.connect(("127.0.0.1", 4445))
f = s.makefile("rb")
nonce = ""
for line in lines:
    now = int(time.time() * 1000)
    line = re.sub(r"@NOW([+-][0-9]+)?", lambda m: str(now + int(m.group(1) or 0)), line)
    if line.endswith(" mac=@MAC"):
        body = line[:-len(" mac=@MAC")]
        mac = hmac.new(os.environ["KEY"].encode(), (nonce + " " + body).encode(),
                       hashlib.sha256).hexdigest()
        line = body + " mac=" + mac
    s.sendall((line + "\n").encode())
    while True:
        rep = f.readline().decode()
        if not rep:
            sys.exit(0)
        rep = rep.rstrip("\n")
        print(rep)
        m = re.search(r" nonce=([0-9a-f]+)", rep)
        if rep.startswith("HELLO 1 ") and m:
            nonce = m.group(1)
        if not line.startswith("STATUS 1") or rep == "END 1":
            break
PY
}

# udp <request line>: prints the reply, or nothing after 1 s.
udp() {
  python3 - "$1" <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(1.0)
s.sendto((sys.argv[1] + "\n").encode(), ("127.0.0.1", 4445))
try:
    print(s.recv(1024).decode().rstrip("\n"))
except socket.timeout:
    pass
PY
}

smb() {
  smbclient //127.0.0.1/prospero -p "$PORT" -U "prospero%$PASS" -m SMB3_11 \
    --client-protection=sign --option='ntlmssp_client:force_old_spnego=yes' "$@"
}

echo "== case 1: first start creates config.ini and the log"
start
need test -f "$STATE/config.ini"
need test "$(stat -c %a "$STATE/config.ini")" = 600
cfg=$(cat "$STATE/config.ini")
expect "$cfg" '^server_id=[0-9a-f]\{16\}$'
expect "$cfg" '^user=prospero$'
expect "$cfg" '^password=[A-Za-z0-9]\{24\}$'
expect "$cfg" '^name=PS5$'
ID=$(sed -n 's/^server_id=//p' "$STATE/config.ini")
PASS=$(sed -n 's/^password=//p' "$STATE/config.ini")
out=$(status)
expect "$out" "^server_id=$ID$"
expect "$out" '^share=prospero$'
expect "$out" "^smb_port=$PORT$"
need grep -q "config.ini created (first start)" "$STATE/log.txt"
if { echo "$out"; cat "$STATE/log.txt" "$WORK/server.log"; } | grep -q "$PASS"; then
  echo "the password must not be in the status or the log" >&2
  exit 1
fi
echo "case 1 passed"

echo "== case 2: service channel"
rep=$(svc 127.0.0.1 "HELLO 1 utc_ms=1 pc=ci" "STATUS 1" "NOPE 1" "HELLO 2")
echo "$rep"
expect "$rep" "^HELLO 1 server_id=$ID name=PS5 share=prospero smb_port=$PORT pairing=open nonce=[0-9a-f]\{16\}$"
expect "$rep" '^STATUS 1$'
expect "$rep" '^PROSPERO-SMBD OK$'
expect "$rep" '^END 1$'
need [ "$(grep -c '^ERROR 1 reason=unknown-command$' <<<"$rep")" = 2 ]
rep=$(svc 127.0.0.2 "QUIT 1")
expect "$rep" '^ERROR 1 reason=not-local$'
# The source address must be set: towards 127.0.0.2 Linux still sends
# from 127.0.0.1.
need curl -fsS --max-time 3 --interface 127.0.0.2 http://127.0.0.1:4445/quit -o "$WORK/quit.txt"
expect "$(cat "$WORK/quit.txt")" 'QUIT REFUSED not-local'
need kill -0 "$server"
echo "case 2 passed"

echo "== case 2a: discovery on UDP 4445"
rep=$(udp "PROSPERO-DISCOVER 1 nonce=00ff12ab")
echo "$rep"
expect "$rep" "^PROSPERO-HERE 1 nonce=00ff12ab id=$ID service_port=4445 smb_port=$PORT name=PS5$"
rep=$(udp "PROSPERO-DISCOVER 1 nonce=abc utc_ms=1 later=field")
expect "$rep" "^PROSPERO-HERE 1 nonce=abc id=$ID "
for bad in "PROSPERO-DISCOVER 2 nonce=00ff" "PROSPERO-DISCOVER 1 nonce=00FF" \
           "PROSPERO-DISCOVER 1 nonce=" "PROSPERO-DISCOVER 1" \
           "PROSPERO-DISCOVER 1 nonce=0123456789abcdef0123456789abcdef0" "hello"; do
  need test -z "$(udp "$bad")"
done
out=$(status)
expect "$out" '^discovery_replies=2$'
expect "$out" '^discovery_invalid=6$'
echo "case 2a passed"

echo "== case 2b: the first PC pairs, then pairing is closed"
rep=$(svc 127.0.0.1 "PAIR 1 pc=a/b" "PAIR 1" "PAIR 1 pc=ci-pc.1" "PAIR 1 pc=other" "HELLO 1")
echo "$rep" | sed 's/password=.*/password=.../'
expect "$rep" '^PAIR-REFUSED 1 reason=bad-name$'
need [ "$(grep -c '^PAIR-REFUSED 1 reason=bad-name$' <<<"$rep")" = 2 ]
expect "$rep" "^PAIRED 1 user=prospero password=$PASS$"
expect "$rep" '^PAIR-REFUSED 1 reason=closed$'
expect "$rep" ' pairing=closed nonce='
need grep -q '^paired=ci-pc.1$' "$STATE/config.ini"
need grep -q "gekoppelt mit ci-pc.1 (127.0.0.1)" "$STATE/log.txt"
expect "$(status)" '^paired_pcs=1$'
echo "case 2b passed"

echo "== case 2c: PING, trusted only with a valid mac"
export KEY="$PASS"
T=$(( $(date +%s) * 1000 - 3600000 ))
rep=$(svc 127.0.0.1 "PING 1 utc_ms=@NOW-3600000 mac=@MAC" \
  "HELLO 1" "PING 1 utc_ms=@NOW-3600000 mac=@MAC" "PING 1 utc_ms=$T mac=@MAC" \
  "PING 1 utc_ms=$T mac=@MAC" "PING 1 utc_ms=@NOW" \
  "PING 1 utc_ms=@NOW+5 mac=0000000000000000000000000000000000000000000000000000000000000000" \
  "PING 1 utc_ms=x")
echo "$rep"
mapfile -t r <<<"$rep"
expect "${r[0]}" '^PONG 1 utc_ms=[0-9]* skew_ms=[0-9]* trusted=0$'   # before HELLO
expect "${r[2]}" ' trusted=1$'
skew=$(sed -n 's/.* skew_ms=\([0-9-]*\) .*/\1/p' <<<"${r[2]}")
need [ "$skew" -ge 3595000 ]
need [ "$skew" -le 3605000 ]
expect "${r[3]}" ' trusted=0$'     # older utc_ms than the last trusted one
expect "${r[4]}" ' trusted=0$'     # replayed
expect "${r[5]}" ' trusted=0$'     # no mac
expect "${r[6]}" ' trusted=0$'     # wrong mac
expect "${r[7]}" '^ERROR 1 reason=bad-time$'
rep=$(KEY=wrong-password svc 127.0.0.1 "HELLO 1" "PING 1 utc_ms=@NOW mac=@MAC")
expect "$rep" ' trusted=0$'
out=$(status)
expect "$out" '^skew_samples=1$'
need grep -q "clock: console runs" "$STATE/log.txt"
echo "case 2c passed"

echo "== case 2d: a paired PC opens pairing for another one"
rep=$(svc 127.0.0.2 "OPEN-PAIRING 1 utc_ms=@NOW mac=@MAC")
expect "$rep" '^ERROR 1 reason=bad-mac$'    # no HELLO, no nonce
rep=$(KEY=wrong-password svc 127.0.0.2 "HELLO 1" "OPEN-PAIRING 1 utc_ms=@NOW mac=@MAC")
expect "$rep" '^ERROR 1 reason=bad-mac$'
rep=$(svc 127.0.0.2 "HELLO 1" "OPEN-PAIRING 1 utc_ms=@NOW mac=@MAC" "HELLO 1")
expect "$rep" '^PAIRING-OPEN 1 seconds=600$'
expect "$rep" ' pairing=open nonce='
need grep -q "Koppeln fuer 10 Minuten offen (angefordert von 127.0.0.2)" "$STATE/log.txt"
rep=$(svc 127.0.0.2 "PAIR 1 pc=second" "PAIR 1 pc=third")
expect "$rep" "^PAIRED 1 user=prospero password=$PASS$"
expect "$rep" '^PAIR-REFUSED 1 reason=closed$'
need grep -q '^paired=second$' "$STATE/config.ini"
expect "$(status)" '^paired_pcs=2$'
echo "case 2d passed"

echo "== case 3: the share, read and write"
TEST="_ProsperoAutoDrive_Test_$(date -u +%Y%m%d-%H%M%S)-$RANDOM"
printf 'Prospero AutoDrive P2.1\r\n' > "$WORK/up.txt"
smb -c "mkdir $TEST; cd $TEST; put $WORK/up.txt up.txt; get up.txt $WORK/down.txt; ls" >/dev/null
cmp "$WORK/up.txt" "$WORK/down.txt"
need cmp "$WORK/up.txt" "$ROOT/$TEST/up.txt"
smb -c 'ls' > "$WORK/ls.txt"
need grep -q "$TEST" "$WORK/ls.txt"
echo "case 3 passed"

echo "== case 4: a FIFO is refused and does not block the server"
mkfifo "$ROOT/$TEST/fifo"
smb -c "get $TEST/fifo $WORK/fifo.out" > "$WORK/fifo.txt" 2>&1 || true
if ! grep -q NT_STATUS_ACCESS_DENIED "$WORK/fifo.txt"; then
  cat "$WORK/fifo.txt"
  echo "reading the FIFO was not refused" >&2
  exit 1
fi
expect "$(status)" '^special_refused=1$'
echo "case 4 passed"

echo "== case 5: a new instance replaces the running one, same identity"
first=$server
start
for _ in $(seq 100); do kill -0 "$first" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$first" 2>/dev/null; then
  echo "the first instance is still running" >&2
  exit 1
fi
need wait "$first"
out=$(status)
expect "$out" "^server_id=$ID$"
expect "$out" '^paired_pcs=2$'
expect "$out" '^pairing=closed$'
smb -c "get $TEST/up.txt $WORK/down2.txt" >/dev/null
cmp "$WORK/up.txt" "$WORK/down2.txt"
echo "case 5 passed"

echo "== case 6: QUIT from this machine ends the server"
rep=$(svc 127.0.0.1 "QUIT 1")
expect "$rep" '^BYE 1$'
need wait "$server"
server=
echo "case 6 passed"

echo "== case 7: an invalid config.ini is kept aside and replaced"
printf 'server_id=nothex\n' > "$STATE/config.ini"
start
need test -f "$STATE/config.ini.bad"
out=$(status)
expect "$out" '^server_id=[0-9a-f]\{16\}$'
expect "$out" '^pairing=open$'
need [ "$(sed -n 's/^server_id=//p' "$STATE/config.ini")" != "$ID" ]
need grep -q "config.ini war fehlerhaft" "$STATE/log.txt"
svc 127.0.0.1 "QUIT 1" >/dev/null
need wait "$server"
server=
echo "case 7 passed"

trap - EXIT
rm -rf "$WORK"
echo "SMBD-HOST-TEST: OK"
