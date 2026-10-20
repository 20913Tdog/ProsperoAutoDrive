#!/usr/bin/env bash
# Fails if code built for the PS5 calls one of the SDK's bare syscall stubs
# that libkernel does not export. The SDK's libc.a (syscalls.o) has a stub
# for every FreeBSD syscall number. The stubs leave errno alone, and on the
# console some numbers mean a different call: utimensat() did nothing
# useful in S3 and its result came back in place of errno. libkernel
# exports are the console's own wrappers.
# syscalls.o is linked whole, so the payloads themselves cannot show which
# stub is used; the check looks at what our objects and libsmb2 reference.
# Usage: check-raw-syscalls.sh <PS5 build dir>   (needs LLVM_ROOT, PS5_PAYLOAD_SDK)
set -euo pipefail

BUILD="${1:?usage: $0 <PS5 build dir>}"
NM="$LLVM_ROOT/bin/llvm-nm"
SDK="$PS5_PAYLOAD_SDK"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

(cd "$tmp" && "$LLVM_ROOT/bin/llvm-ar" x "$SDK/target/lib/libc.a" syscalls.o)
"$NM" "$tmp/syscalls.o" | awk '$2 == "T" {print $3}' | sort -u >"$tmp/raw"
"$NM" -D "$SDK/target/lib/libkernel.so" | awk '{print $NF}' | sort -u >"$tmp/kernel"
comm -23 "$tmp/raw" "$tmp/kernel" >"$tmp/bad"
[ -s "$tmp/bad" ] || { echo "no stub list - SDK layout changed?" >&2; exit 1; }

files="$(find "$BUILD" -name '*.o' -o -name '*.a' | sort)"
[ -n "$files" ] || { echo "no objects in $BUILD" >&2; exit 1; }
fail=0
for f in $files; do
  used="$("$NM" -u "$f" 2>/dev/null | awk '{print $NF}' | sort -u | comm -12 - "$tmp/bad" | tr '\n' ' ')"
  if [ -n "$used" ]; then
    echo "${f#"$BUILD"/} calls bare syscall stubs: $used" >&2
    fail=1
  fi
done
[ "$fail" = 0 ] && echo "no bare syscall stubs in $(echo "$files" | wc -l) objects"
exit "$fail"
