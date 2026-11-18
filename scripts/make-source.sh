#!/usr/bin/env bash
# The complete source of a release, as one file: dist/playrx-source-<version>.tar.gz. It is this repository with
# the kepyur submodule (and its own submodules) checked out and patches/ already applied, so it builds without git
# and is exactly the source the release's PlayRX.exe was built from. Attach it to the GitHub release.
# Run from a checkout where `git submodule update --init --recursive` and the patches (README.md) are done.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(tr -d ' \r\n' < "$ROOT/VERSION")
NAME="playrx-source-$VERSION"
[ -f "$ROOT/third_party/kepyur/native/CMakeLists.txt" ] || { echo "kepyur missing: git submodule update --init --recursive" >&2; exit 1; }
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/$NAME"
# everything git tracks here, then kepyur as checked out (patched), without git data, builds or the Android app
git -C "$ROOT" archive --format=tar HEAD | tar -C "$W/$NAME" -xf -
rm -rf "$W/$NAME/third_party/kepyur"
mkdir -p "$W/$NAME/third_party"
tar -C "$ROOT/third_party" --exclude=.git --exclude='kepyur/android' --exclude='*/build' --exclude='*.cs16' -cf - kepyur |
  tar -C "$W/$NAME/third_party" -xf -
mkdir -p "$ROOT/dist"
tar -C "$W" -czf "$ROOT/dist/$NAME.tar.gz" "$NAME"
echo "built $ROOT/dist/$NAME.tar.gz ($(du -h "$ROOT/dist/$NAME.tar.gz" | cut -f1))"
