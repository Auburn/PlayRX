#!/usr/bin/env bash
# wdi-simple.exe from libwdi (github.com/pbatard/libwdi, LGPL-3.0, the library behind Zadig): the installers use
# it to set the WinUSB driver on the RTL8812BU adapters. Run in an MSYS2 UCRT64 shell. Needs:
#   build/libwdi-src   git clone https://github.com/pbatard/libwdi.git
#   build/wdk8         Microsoft's "WDK 8 Redistributable Components" (wdfcoinstaller.msi, fwlink 253170),
#                      extracted with: msiexec /a wdfcoinstaller.msi /qn TARGETDIR=<build\wdk8>
#                      (libwdi's configure embeds its WinUSB co-installers)
# Output: build/wdi/wdi-simple.exe (64-bit only; Windows 10 / 11).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/build/libwdi-src"
WDK="$ROOT/build/wdk8/Windows Kits/8.0"
[ -d "$SRC" ] || { echo "missing $SRC (git clone https://github.com/pbatard/libwdi.git)" >&2; exit 1; }
[ -f "$WDK/redist/wdf/x64/winusbcoinstaller2.dll" ] || { echo "missing the WDK 8 redist in build/wdk8" >&2; exit 1; }
pacman -S --needed --noconfirm autoconf automake libtool make >/dev/null

# autotools cannot handle spaces in paths: build in a temporary copy, with the redist beside it
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cp -r "$SRC" "$W/libwdi"
mkdir -p "$W/wdk"
cp -r "$WDK/redist" "$W/wdk/"
cd "$W/libwdi"
# (autogen.sh carries the author's own paths: bootstrap and configure directly; WinUSB only, 64-bit only)
# (the embedder is a native Windows tool: give it a Windows path, not MSYS's /tmp)
{ ./bootstrap.sh && ./configure --disable-32bit --enable-examples-build --disable-debug --with-wdkdir="$(cygpath -m "$W/wdk")" --with-wdfver=1011; } \
  > "$W/configure.log" 2>&1 || { tail -30 "$W/configure.log"; exit 1; }
make -j"$(nproc)" > "$W/make.log" 2>&1 || { tail -40 "$W/make.log"; exit 1; }
mkdir -p "$ROOT/build/wdi"
exe=$(find . -name 'wdi-simple.exe' -not -path '*/.libs/lt-*' | head -1)
[ -n "$exe" ] || { echo "wdi-simple.exe not built"; tail -20 "$W/make.log"; exit 1; }
cp "$exe" "$ROOT/build/wdi/wdi-simple.exe"
echo "built $ROOT/build/wdi/wdi-simple.exe"
