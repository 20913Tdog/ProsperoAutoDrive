#!/usr/bin/env bash
# Runs a subset of Samba's smbtorture against the server and compares the
# passing subtests with a baseline: a subtest that passed before and fails
# now fails the job; newly passing subtests are reported for the baseline.
# Usage: smbtorture-subset.sh <port> <password> <baseline file>
# Writes torture.log (summary) and torture-pass.txt (passing subtests).
set -uo pipefail

port="${1:?usage: $0 <port> <password> <baseline>}"
pass="${2:?}"
baseline="${3:?}"
suites="smb2.connect smb2.read smb2.dir smb2.rename smb2.setinfo smb2.getinfo smb2.delete-on-close-perms"

: > torture.log
: > torture-pass.txt
for t in $suites; do
  # force_old_spnego: see tests/spikes/s2-host-test.sh.
  timeout 300 smbtorture "//127.0.0.1/s3" -p "$port" -U "prospero%$pass" \
    --option=ntlmssp_client:force_old_spnego=yes --option=clientsigning=required \
    "$t" > "torture-$t.log" 2>&1
  rc=$?
  ok=$(grep -c '^success:' "torture-$t.log")
  bad=$(grep -c -E '^(failure|error):' "torture-$t.log")
  echo "$t: exit $rc, $ok success, $bad failure/error" >> torture.log
  sed -n "s/^success: \(.*\)/$t \1/p" "torture-$t.log" >> torture-pass.txt
  grep -E '^(failure|error):' "torture-$t.log" | head -n 8 | sed 's/^/    /' >> torture.log
done
sort -o torture-pass.txt torture-pass.txt

# Lines starting with # are comments.
grep -v -e '^#' -e '^[[:space:]]*$' "$baseline" | sort > torture-baseline.txt
lost=$(comm -23 torture-baseline.txt torture-pass.txt)
new=$(comm -13 torture-baseline.txt torture-pass.txt)
if [ -n "$new" ]; then
  { echo "newly passing, add to $baseline:"; echo "$new"; } >> torture.log
fi
if [ -n "$lost" ]; then
  { echo "REGRESSION, passed before:"; echo "$lost"; } >> torture.log
  cat torture.log
  exit 1
fi
cat torture.log
