#!/usr/bin/env bash
# Builds the two files a release is supposed to contain, and attaches them.
#
# The readme tells people to download zelr.exe to try it on Windows and
# zelr.iso to boot a real machine. For fifteen releases neither was attached
# to anything. The notes were written by hand, the artifacts were never
# built, and anyone who followed the readme arrived at a page offering them
# a source tarball and nothing else.
#
#   pipeline/release.sh v0.16.0 notes.md
#
# The tag and the version compiled into the kernel have to agree. A release
# whose exe reports a different number than the page it came from is worse
# than no release, and it is the one mistake nobody notices until somebody
# files a bug against a version that was never shipped.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

TAG="${1:?usage: release.sh <tag> [notes.md]}"
NOTES="${2:-}"
VER="${TAG#v}"

have="$(grep KERNEL_VERSION include/types.h | cut -d'"' -f2)"
if [ "$have" != "$VER" ]; then
  echo "the kernel says $have and the tag says $VER"
  exit 1
fi

ISO="build/zelr.iso"
EXE="build/launcher/zelr.exe"

echo "building $VER"
bash build.sh > /dev/null
python tools/mkiso.py > /dev/null

# The launcher is the one part of this project that is not from scratch: it
# is a WPF window that starts QEMU, and it carries the kernel inside itself
# so the download is a single file.
dotnet publish launcher/ZelrLauncher.csproj -c Release -o build/launcher > /dev/null

# Both carry the version string, because the kernel is inside both. This
# catches an artifact built from an older tree, which is what happens when
# somebody tags first and builds afterwards.
for f in "$ISO" "$EXE"; do
  if ! grep -q "$VER" "$f"; then
    echo "$f was not built from $VER"
    exit 1
  fi
done

if gh release view "$TAG" > /dev/null 2>&1; then
  if [ -n "$NOTES" ]; then
    gh release edit "$TAG" -F "$NOTES"
  fi
else
  if [ -n "$NOTES" ]; then
    gh release create "$TAG" --title "zelr $VER" -F "$NOTES"
  else
    gh release create "$TAG" --title "zelr $VER" --generate-notes
  fi
fi

gh release upload "$TAG" "$ISO" "$EXE" --clobber

echo "released $TAG"
gh release view "$TAG" --json assets --jq '.assets[] | "  \(.name)  \(.size) bytes"'
