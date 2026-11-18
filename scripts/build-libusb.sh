#!/usr/bin/env bash
# Build libusb from kepyur's submodule with AnalogRX's patch (WinUSB RAW_IO on request, see
# patches/libusb-0001-winusb-raw-io.patch) into /opt/arx-libusb. scripts/build.sh puts it first on
# PKG_CONFIG_PATH, so devourer, wifi-sdr and the apps link it instead of MSYS2's libusb.
# Autotools cannot work in a path with spaces, so it builds from a copy in a temp folder.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/third_party/kepyur/third_party/libusb
OUT=/opt/arx-libusb
git -C "$ROOT/third_party/kepyur" submodule update --init third_party/libusb
if ! grep -q "LIBUSB_WINUSB_RAW_IO" "$SRC/libusb/os/windows_winusb.c"; then
  git -C "$SRC" apply "$ROOT/patches/libusb-0001-winusb-raw-io.patch"
fi
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cp -r "$SRC" "$W/libusb"
cd "$W/libusb"
./bootstrap.sh >/dev/null 2>&1
./configure --prefix="$OUT" --disable-static --enable-shared >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "libusb $(PKG_CONFIG_PATH=$OUT/lib/pkgconfig pkg-config --modversion libusb-1.0) (patched) in $OUT"
