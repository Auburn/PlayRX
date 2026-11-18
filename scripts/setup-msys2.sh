#!/usr/bin/env bash
# One-time toolchain setup for building Play RX, run in an MSYS2 UCRT64 shell (https://www.msys2.org, or
# winget install MSYS2.MSYS2): GCC, CMake, Ninja, pkg-config, libusb, webview (the WebView2 window), xxd (vim),
# NSIS (the installer).
#
# The adapter's on-chip programs come prebuilt (fw/prebuilt-fw). To assemble them from wifi-sdr's source instead
# (cmake -DARX_ASSEMBLE_FW=ON), also build mipsel binutils:  scripts/setup-msys2.sh --mipsel
set -euo pipefail
pacman -S --needed --noconfirm git make vim diffutils \
  mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-libusb mingw-w64-ucrt-x86_64-webview \
  mingw-w64-ucrt-x86_64-nsis

if [ "${1:-}" = "--mipsel" ] && [ ! -x /opt/mipsel/bin/mipsel-linux-gnu-as ]; then
  pacman -S --needed --noconfirm bison flex texinfo
  V=2.45
  W=$(mktemp -d)
  cd "$W"
  curl -sSLO "https://ftp.gnu.org/gnu/binutils/binutils-$V.tar.xz"
  tar xf "binutils-$V.tar.xz"
  mkdir build && cd build
  "../binutils-$V/configure" --target=mipsel-linux-gnu --prefix=/opt/mipsel --disable-nls --disable-werror \
    --disable-gdb --disable-gprof --disable-gold --disable-sim --disable-libctf --disable-gprofng >/dev/null
  make -j"$(nproc)" >/dev/null
  make install >/dev/null
  cd / && rm -rf "$W"
  /opt/mipsel/bin/mipsel-linux-gnu-as --version | head -1
fi
echo "toolchain ready"
