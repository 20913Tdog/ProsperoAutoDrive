#!/usr/bin/env bash
# E1 test of prospero-smbd (P2.1 to P2.3) on Linux: configuration, ring log,
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
# Signing: AES-NI CMAC passed RFC 4493 and matches the reference code (the
# CI runners have AES-NI, as the console's Zen 2 has).
expect "$out" '^signing_cmac=aes-ni$'
grep "signing: AES-CMAC" "$STATE/log.txt"
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
rep=$(svc 127.0.0.1 "RESET-STATS 1")
expect "$rep" '^RESET-STATS 1$'
expect "$(status)" '^diag_outq_bytes=[0-9]* (max [0-9]*, avg [0-9]*)$'
expect "$(status)" '^diag_sock_unsent_bytes=[0-9]* (max '   # SIOCOUTQ works on Linux
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
rep=$(svc 127.0.0.1 "PING 1 seq=1 utc_ms=@NOW-3600000 mac=@MAC" \
  "HELLO 1" "PING 1 seq=1 utc_ms=@NOW-3600000 mac=@MAC" "PING 1 seq=2 utc_ms=$T mac=@MAC" \
  "PING 1 seq=2 utc_ms=$T mac=@MAC" "PING 1 utc_ms=@NOW" \
  "PING 1 seq=3 utc_ms=@NOW mac=0000000000000000000000000000000000000000000000000000000000000000" \
  "PING 1 utc_ms=x")
echo "$rep"
mapfile -t r <<<"$rep"
expect "${r[0]}" '^PONG 1 utc_ms=[0-9]* skew_ms=[0-9]* trusted=0$'   # before HELLO
expect "${r[2]}" ' trusted=1$'
skew=$(sed -n 's/.* skew_ms=\([0-9-]*\) .*/\1/p' <<<"${r[2]}")
need [ "$skew" -ge 3595000 ]
need [ "$skew" -le 3605000 ]
expect "${r[3]}" ' trusted=1$'     # an older time is fine: the PC clock may go back
expect "${r[4]}" ' trusted=0$'     # replayed: same seq
expect "${r[5]}" ' trusted=0$'     # no mac
expect "${r[6]}" ' trusted=0$'     # wrong mac
expect "${r[7]}" '^ERROR 1 reason=bad-time$'
rep=$(KEY=wrong-password svc 127.0.0.1 "HELLO 1" "PING 1 seq=1 utc_ms=@NOW mac=@MAC")
expect "$rep" ' trusted=0$'
out=$(status)
expect "$out" '^skew_samples=2$'
need grep -q "clock: console runs" "$STATE/log.txt"
echo "case 2c passed"

echo "== case 2d: a paired PC opens pairing for another one"
rep=$(svc 127.0.0.2 "OPEN-PAIRING 1 seq=1 mac=@MAC")
expect "$rep" '^ERROR 1 reason=bad-mac$'    # no HELLO, no nonce
rep=$(KEY=wrong-password svc 127.0.0.2 "HELLO 1" "OPEN-PAIRING 1 seq=1 mac=@MAC")
expect "$rep" '^ERROR 1 reason=bad-mac$'
rep=$(svc 127.0.0.2 "HELLO 1" "OPEN-PAIRING 1 seq=1 mac=@MAC" "HELLO 1")
expect "$rep" '^PAIRING-OPEN 1 seconds=600$'
expect "$rep" ' pairing=open nonce='
need grep -q "Koppeln fuer 10 Minuten offen (angefordert von 127.0.0.2)" "$STATE/log.txt"
rep=$(svc 127.0.0.2 "PAIR 1 pc=second" "PAIR 1 pc=third")
expect "$rep" "^PAIRED 1 user=prospero password=$PASS$"
expect "$rep" '^PAIR-REFUSED 1 reason=closed$'
need grep -q '^paired=second$' "$STATE/config.ini"
expect "$(status)" '^paired_pcs=2$'
echo "case 2d passed"

echo "== case 2e: time stamps are offset by the measured skew"
# Case 2c measured about 3600 s: Windows sees times 1 h earlier than on
# disk and sets them 1 h later on disk.
out=$(status)
expect "$out" '^clock_skew_s=3600$'
need grep -q '^skew_s=3600$' "$STATE/config.ini"
CLK="_ProsperoAutoDrive_Test_clock-$RANDOM"
mkdir "$ROOT/$CLK"
printf 'clock\r\n' > "$ROOT/$CLK/t.txt"
touch -d @1700000000 "$ROOT/$CLK/t.txt"
wt=$(TZ=UTC smb -c "allinfo $CLK/t.txt" | sed -n 's/^write_time:[[:space:]]*//p')
echo "write_time as seen by the client: $wt"
need [ "$(TZ=UTC date -d "$wt" +%s)" = 1699996400 ]
# smbclient takes the time as local time, so TZ=UTC: 2023-11-14 22:13:20
# UTC is 1700000000.
TZ=UTC smb -c "utimes $CLK/t.txt -1 -1 23:11:14-22:13:20 -1" >/dev/null
need [ "$(stat -c %Y "$ROOT/$CLK/t.txt")" = 1700003600 ]
# Hysteresis: 1 s more is not taken, 3 s more is.
svc 127.0.0.1 "HELLO 1" "PING 1 seq=1 utc_ms=@NOW-3601000 mac=@MAC" >/dev/null
expect "$(status)" '^clock_skew_s=3600$'
svc 127.0.0.1 "HELLO 1" "PING 1 seq=1 utc_ms=@NOW-3603000 mac=@MAC" >/dev/null
expect "$(status)" '^clock_skew_s=3603$'
need grep -q '^skew_s=3603$' "$STATE/config.ini"
echo "case 2e passed"

echo "== case 3: the share, read and write"
TEST="_ProsperoAutoDrive_Test_$(date -u +%Y%m%d-%H%M%S)-$RANDOM"
printf 'Prospero AutoDrive P2.1\r\n' > "$WORK/up.txt"
smb -c "mkdir $TEST; cd $TEST; put $WORK/up.txt up.txt; get up.txt $WORK/down.txt; ls" >/dev/null
cmp "$WORK/up.txt" "$WORK/down.txt"
need cmp "$WORK/up.txt" "$ROOT/$TEST/up.txt"
smb -c 'ls' > "$WORK/ls.txt"
need grep -q "$TEST" "$WORK/ls.txt"
echo "case 3 passed"

echo "== case 3a: TUNE sets send buffer and credit limit"
rep=$(svc 127.0.0.1 "HELLO 1" "TUNE 1 seq=1 sndbuf=131072 credits=16 mac=0000" \
  "TUNE 1 seq=2 sndbuf=-1 credits=16 mac=@MAC" "TUNE 1 seq=3 sndbuf=0 credits=9000 mac=@MAC" \
  "TUNE 1 seq=4 sndbuf=131072 credits=16 mac=@MAC")
echo "$rep"
mapfile -t r <<<"$rep"
expect "${r[1]}" '^ERROR 1 reason=bad-mac$'
expect "${r[2]}" '^ERROR 1 reason=bad-value$'
expect "${r[3]}" '^ERROR 1 reason=bad-value$'
expect "${r[4]}" '^TUNED 1 sndbuf=131072 credits=16$'
out=$(status)
expect "$out" '^tune_sndbuf=131072$'
expect "$out" '^tune_credits=16$'
svc 127.0.0.1 "RESET-STATS 1" >/dev/null
head -c $((8 << 20)) /dev/urandom > "$WORK/big.bin"
smb -c "cd $TEST; put $WORK/big.bin big.bin; get big.bin $WORK/big-back.bin" >/dev/null
need cmp "$WORK/big.bin" "$WORK/big-back.bin"
out=$(status)
echo "$out" | grep '^diag_credits='
cmax=$(sed -n 's/^diag_credits=[0-9]* (max \([0-9]*\),.*/\1/p' <<<"$out")
need [ -n "$cmax" ]
need [ "$cmax" -ge 1 ]
need [ "$cmax" -le 16 ]
rep=$(svc 127.0.0.1 "HELLO 1" "TUNE 1 seq=1 sndbuf=0 credits=0 mac=@MAC")
expect "$rep" '^TUNED 1 sndbuf=0 credits=0$'
expect "$(status)" '^tune_credits=0$'
need grep -q "tune: send buffer 131072, credit limit 16" "$STATE/log.txt"
echo "case 3a passed"

echo "== case 3b: free space is that of the folder's file system"
# As /data on the console, a folder on another file system than the share
# root: /dev/shm (tmpfs) through a link in the test folder. Only read.
if [ "$(stat -f -c %i /dev/shm)" != "$(stat -f -c %i "$ROOT")" ]; then
  ln -s /dev/shm "$ROOT/$TEST/other-fs"
  du_out=$(smb -c "cd $TEST/other-fs; du" | grep 'blocks available')
  echo "smbclient du: $du_out"
  avail=$(sed -n 's/.* blocks of size \([0-9]*\)\. \([0-9]*\) blocks available.*/\1 \2/p' <<<"$du_out" |
          awk '{ printf "%.0f", $1 * $2 }')
  want=$(df -B1 --output=avail /dev/shm | tail -1 | tr -d ' ')
  root_avail=$(df -B1 --output=avail "$ROOT" | tail -1 | tr -d ' ')
  echo "reported $avail, /dev/shm $want, share root $root_avail"
  # within 5 %: tmpfs usage moves a little between the two reads
  need python3 -c "import sys; a,w=int('$avail'),int('$want'); sys.exit(0 if abs(a-w) <= w*0.05 else 1)"
  echo "case 3b passed"
else
  echo "case 3b skipped: /dev/shm is on the same file system"
fi

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
expect "$out" '^clock_skew_s=3603$'
need grep -q "offset by 3603 s (from config.ini)" "$STATE/log.txt"
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
expect "$out" '^clock_skew_s=0 (unmeasured)$'
need [ "$(sed -n 's/^server_id=//p' "$STATE/config.ini")" != "$ID" ]
need grep -q "config.ini war fehlerhaft" "$STATE/log.txt"
svc 127.0.0.1 "QUIT 1" >/dev/null
need wait "$server"
server=
echo "case 7 passed"

trap - EXIT
rm -rf "$WORK"
echo "SMBD-HOST-TEST: OK"
