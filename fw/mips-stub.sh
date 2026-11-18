#!/bin/sh
# Stands in for the mipsel binutils (as, ld, objcopy) when building on macOS, where Homebrew has none. The
# adapter's on-chip programs come prebuilt instead (mac/prebuilt-fw/*.bin, assembled from wifi-sdr's src/fw/*.S
# on Windows / Linux): "objcopy -O binary ... X.elf X.bin" copies the prebuilt X.bin; "as" and "ld" only make
# their output file so the build's steps line up. wifi-sdr's CMake then turns X.bin into X.h with xxd as usual.
here=$(cd "$(dirname "$0")" && pwd)
out=""
prev=""
binary=0
for a in "$@"; do
  [ "$prev" = "-o" ] && out="$a"
  [ "$prev" = "-O" ] && [ "$a" = "binary" ] && binary=1
  prev="$a"
done
if [ "$binary" = 1 ]; then
  for last in "$@"; do :; done  # objcopy's last argument: the .bin to write
  name=$(basename "$last")
  if [ ! -f "$here/prebuilt-fw/$name" ]; then
    echo "mips-stub: no prebuilt $name in $here/prebuilt-fw" >&2
    exit 1
  fi
  cp "$here/prebuilt-fw/$name" "$last"
  exit 0
fi
[ -n "$out" ] && : > "$out"
exit 0
