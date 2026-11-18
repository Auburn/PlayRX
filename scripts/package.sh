#!/usr/bin/env bash
# The Windows installer: dist/PlayRX-Setup-<version>.exe. Run in an MSYS2 UCRT64 shell from the repository, after
# building (cmake -S . -B build -G Ninja && cmake --build build). With build/wdi/wdi-simple.exe present
# (scripts/build-libwdi.sh), the installer also sets up the WinUSB driver for the adapters.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build"
VERSION=$(tr -d ' \r\n' < "$ROOT/VERSION")
WDI="$ROOT/build/wdi/wdi-simple.exe"
command -v makensis >/dev/null || { echo "makensis not found: pacman -S mingw-w64-ucrt-x86_64-nsis" >&2; exit 1; }
cmake --build "$BUILD" --target PlayRX PlayLeagueCam

stage="$ROOT/dist/stage/PlayRX"
rm -rf "$stage"
mkdir -p "$stage/web" "$stage/licenses"
cp "$BUILD/PlayRX.exe" "$BUILD/PlayLeagueCam.dll" "$BUILD/libusb-1.0.dll" "$BUILD/libwinpthread-1.dll" "$stage/"
cp "$ROOT"/web/*.html "$ROOT"/web/*.js "$ROOT"/web/*.css "$stage/web/"
cp "$ROOT/NOTICE.txt" "$stage/"
cp "$ROOT/LICENSE" "$stage/licenses/GPL-2.0.txt"
cp "$ROOT/third_party/kepyur/third_party/libusb/COPYING" "$stage/licenses/LGPL-2.1-libusb.txt"
for lic in /ucrt64 /c/msys64/ucrt64; do  # MSYS2 shell: /ucrt64; another bash with MSYS2 on PATH: /c/msys64/ucrt64
  for d in winpthreads libwinpthread; do
    [ -f "$lic/share/licenses/$d/COPYING" ] && cp "$lic/share/licenses/$d/COPYING" "$stage/licenses/winpthreads.txt" && break 2
  done
done
[ -f "$stage/licenses/winpthreads.txt" ] || { echo "winpthreads licence not found" >&2; exit 1; }
extra=()
if [ -f "$WDI" ]; then
  mkdir -p "$stage/driver"
  cp "$WDI" "$ROOT/installer/setup-adapters.cmd" "$stage/driver/"
  cp "$ROOT/build/libwdi-src/COPYING-LGPL" "$stage/licenses/LGPL-3.0-libwdi.txt"
  cp "$ROOT/build/libwdi-src/COPYING" "$stage/licenses/GPL-3.0.txt"  # (LGPL-3.0 adds to GPL-3.0)
  extra+=(-DDRIVER)
fi
out="$ROOT/dist/PlayRX-Setup-$VERSION.exe"
makensis -V2 -DPRODUCT="Play RX" -DEXE="PlayRX.exe" -DKEY="PlayRX" -DVERSION="$VERSION" \
  -DSTAGE="$(cygpath -w "$stage")" -DOUTFILE="$(cygpath -w "$out")" "${extra[@]}" "$(cygpath -w "$ROOT/installer/app.nsi")"
echo "built $out ($(du -h "$out" | cut -f1))"
