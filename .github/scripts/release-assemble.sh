#!/usr/bin/env bash
# Assembles a release from the tested dev build. It builds nothing: each
# program is the file of dev-build that the project owner tested, with its
# checksum (CLAUDE.md: hash-identical artifacts of the tested dev build with
# the same tree). Before anything is copied it refuses when
#   - the version is not x.y.z, or CMakeLists.txt or app.rc say another one,
#   - the tag v<version> exists already,
#   - a file of dev-build does not match its checksum,
#   - dev-build was built from another tree than HEAD, or from a commit that
#     is not part of HEAD (something changed after the test).
# The output: the programs, the license texts, THIRD-PARTY-NOTICES.txt,
# BUILD-INFO.txt, a source archive with the libsmb2 submodule, SHA256SUMS.
# Usage: release-assemble.sh <version> <new out dir>
#   in the repository root, with the full history, the tags and the
#   submodule; GH_TOKEN for gh (read access is enough).
set -euo pipefail

VERSION="${1:?usage: $0 <version> <new out dir>}"
OUT="${2:?usage: $0 <version> <new out dir>}"
PRODUCT=(ProsperoAutoDrive.exe prospero-smbd.elf)

fail() {
  echo "::error title=release::$*" >&2
  exit 1
}

# 1. Version and tag, before anything is downloaded.
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "bad version '$VERSION', expected x.y.z"
IFS=. read -r major minor patch <<<"$VERSION"
grep -q "^project(ProsperoAutoDrive VERSION $VERSION " CMakeLists.txt ||
  fail "CMakeLists.txt is not version $VERSION"
for want in "FILEVERSION $major,$minor,$patch,0" "PRODUCTVERSION $major,$minor,$patch,0" \
            "VALUE \"FileVersion\", \"$VERSION\"" "VALUE \"ProductVersion\", \"$VERSION\""; do
  grep -qF "$want" windows/app/app.rc || fail "windows/app/app.rc is not version $VERSION (no $want)"
done
git rev-parse -q --verify "refs/tags/v$VERSION" >/dev/null && fail "tag v$VERSION exists already"
rc=0
git ls-remote --exit-code --tags origin "refs/tags/v$VERSION" >/dev/null || rc=$?
case "$rc" in
  0) fail "tag v$VERSION exists already on origin" ;;
  2) ;;
  *) fail "cannot ask origin for its tags (git ls-remote exit code $rc)" ;;
esac
[ ! -e "$OUT" ] || fail "$OUT exists already"
[ -f third_party/libsmb2/LICENCE-LGPL-2.1.txt ] || fail "the submodule third_party/libsmb2 is not checked out"
sub=$(git rev-parse HEAD:third_party/libsmb2)
[ "$(git -C third_party/libsmb2 rev-parse HEAD)" = "$sub" ] ||
  fail "third_party/libsmb2 is not at the commit HEAD pins ($sub)"

# 2. The tested build, checked file by file.
dl="$(mktemp -d)"
trap 'rm -rf "$dl"' EXIT
gh release download dev-build --dir "$dl/dev"
(cd "$dl/dev" && sha256sum --strict --quiet -c SHA256SUMS) || fail "a file of dev-build does not match its SHA256SUMS"
info() { sed -n "s/^$1=//p" "$dl/dev/BUILD-INFO.txt"; }
commit=$(info commit)
tree=$(info tree)
[ -n "$commit" ] && [ -n "$tree" ] || fail "BUILD-INFO.txt of dev-build has no commit or tree"
head_tree=$(git rev-parse 'HEAD^{tree}')
[ "$tree" = "$head_tree" ] ||
  fail "dev-build was built from tree ${tree:0:12}, which is not the tree of HEAD (${head_tree:0:12}): was something pushed after the tested build?"
git merge-base --is-ancestor "$commit" HEAD || fail "the dev-build commit ${commit:0:12} is not part of HEAD"

# 3. The programs with their tested checksums, then everything else.
mkdir -p "$OUT"
: >"$OUT/SHA256SUMS"
for f in "${PRODUCT[@]}"; do
  line=$(awk -v f="$f" '$2 == f' "$dl/dev/SHA256SUMS")
  [ -n "$line" ] || fail "dev-build has no $f"
  cp "$dl/dev/$f" "$OUT/$f"
  echo "$line" >>"$OUT/SHA256SUMS"
done
cp LICENSE "$OUT/LICENSE.txt"
cp ps5/LICENSE "$OUT/LICENSE-ps5.txt"
cp third_party/libsmb2/LICENCE-LGPL-2.1.txt "$OUT/LICENSE-libsmb2-LGPL-2.1.txt"
cp THIRD-PARTY-NOTICES.txt "$OUT/THIRD-PARTY-NOTICES.txt"
{
  echo "version=$VERSION"
  echo "commit=$commit"
  echo "tree=$tree"
  echo "dev_run=$(info run)"
  echo "built=$(info built)"
  echo "release_commit=$(git rev-parse HEAD)"
  echo "release_run=${GITHUB_RUN_ID:-local}"
} >"$OUT/BUILD-INFO.txt"

# The complete source (GPL-3.0 of the payload): the repository at HEAD and
# libsmb2 at its pinned commit, which GitHub's own archives leave out.
src="prospero-autodrive-$VERSION-source"
git archive --format=tar --prefix="$src/" -o "$dl/source.tar" HEAD
git -C third_party/libsmb2 archive --format=tar --prefix="$src/third_party/libsmb2/" -o "$dl/libsmb2.tar" "$sub"
tar --concatenate -f "$dl/source.tar" "$dl/libsmb2.tar"
gzip -n -9 <"$dl/source.tar" >"$OUT/$src.tar.gz"

(cd "$OUT" && sha256sum -- LICENSE.txt LICENSE-ps5.txt LICENSE-libsmb2-LGPL-2.1.txt THIRD-PARTY-NOTICES.txt \
  BUILD-INFO.txt "$src.tar.gz" >>SHA256SUMS)
(cd "$OUT" && sha256sum --strict --quiet -c SHA256SUMS) || fail "the assembled files do not match SHA256SUMS"
echo "release $VERSION from dev-build ${commit:0:12} (tree ${tree:0:12}):"
cat "$OUT/BUILD-INFO.txt" "$OUT/SHA256SUMS"
