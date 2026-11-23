#!/usr/bin/env bash
# Play RX on Linux: build, install to /opt/play-rx and run it as a service. Headless: the UI is a web page.
# Full instructions and troubleshooting: docs/INSTALL-LINUX.md
#
# Run as your normal user (not with sudo; it asks for your password for the system parts):
#   bash linux/install.sh               install / update Play RX and start it (it then starts at every boot)
#   bash linux/install.sh --no-service  install only; start it yourself with /opt/play-rx/play-rx
#   bash linux/install.sh --sim         also build the simulated version (no adapters needed: build/linux/play-rx-sim)
# Afterwards open  http://localhost:8090/  on this computer, or  http://<this-computer>:8090/  from another one.
#
# Works on Debian 12+, Ubuntu 22.04+, Raspberry Pi OS (64-bit) and their relatives (apt); Fedora (dnf) and
# Arch (pacman) are tried too.
set -uo pipefail
cd "$(dirname "$0")/.."
SRC=$(pwd)
VERSION=$(tr -d ' \r\n' < VERSION)
SIM=0
SERVICE=1
for a in "$@"; do
  case $a in
    --sim) SIM=1 ;;
    --no-service) SERVICE=0 ;;
    *) echo "unknown option: $a (use --sim and/or --no-service)"; exit 2 ;;
  esac
done
ME=$(id -un)
if [ "$ME" = root ]; then
  echo "Run this as your normal user (not with sudo): it asks for your password when it needs it."
  exit 1
fi
LOG="$SRC/install.log"
: > "$LOG"
say() { echo "$@" | tee -a "$LOG"; }
fail() {
  say ""
  say "INSTALL FAILED: $*"
  say "The last lines of install.log:"
  tail -40 "$LOG"
  echo
  echo "See docs/INSTALL-LINUX.md > Troubleshooting. When asking for help, attach install.log."
  exit 1
}
DEST=/opt/play-rx
[ -f third_party/kepyur/native/CMakeLists.txt ] || fail "third_party/kepyur is missing: use the playrx-source-<version>.tar.gz from Releases, or clone with --recursive (docs/INSTALL-LINUX.md)"

say "== Play RX $VERSION for Linux ($(uname -m))"
say "== 1/6 packages (compiler, CMake, Ninja, libusb, libjpeg-turbo)"
if command -v apt-get >/dev/null 2>&1; then
  sudo apt-get update -qq >>"$LOG" 2>&1 || fail "apt-get update"
  sudo apt-get install -y -qq build-essential cmake ninja-build pkg-config libusb-1.0-0-dev libturbojpeg0-dev xxd \
    >>"$LOG" 2>&1 || fail "apt-get install"
elif command -v dnf >/dev/null 2>&1; then
  sudo dnf install -y gcc-c++ make cmake ninja-build pkgconf-pkg-config libusb1-devel turbojpeg-devel vim-common \
    >>"$LOG" 2>&1 || fail "dnf install"
elif command -v pacman >/dev/null 2>&1; then
  sudo pacman -S --needed --noconfirm base-devel cmake ninja pkgconf libusb libjpeg-turbo xxd >>"$LOG" 2>&1 ||
    sudo pacman -S --needed --noconfirm base-devel cmake ninja pkgconf libusb libjpeg-turbo vim >>"$LOG" 2>&1 ||
    fail "pacman -S"
else
  fail "no apt-get, dnf or pacman: install a C++20 compiler, cmake, ninja, pkg-config, libusb-1.0 (dev), libturbojpeg (dev) and xxd yourself, then run this again"
fi
command -v xxd >/dev/null 2>&1 || fail "xxd is missing (Debian/Ubuntu: sudo apt install xxd; older: vim-common)"

say "== 2/6 the adapters: USB access, and Linux's own Wi-Fi driver kept off them"
sudo install -m 644 linux/60-play-rx-rtl8812bu.rules /etc/udev/rules.d/ || fail "udev rule"
sudo install -m 644 linux/play-rx-blacklist.conf /etc/modprobe.d/ || fail "driver blacklist"
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb
# if a Wi-Fi driver already holds a plugged-in adapter, let go of it now (the blacklist covers the next boot)
sudo modprobe -r rtw88_8822bu 2>/dev/null || true
sudo modprobe -r rtw_8822bu 2>/dev/null || true
sudo modprobe -r 88x2bu 2>/dev/null || true
sudo usermod -aG plugdev "$ME" >>"$LOG" 2>&1 || true

say "== 3/6 build (2-5 minutes on a PC, 10-20 minutes on a Raspberry Pi)"
MEM=$(awk '/MemTotal/ {print int($2 / 1024)}' /proc/meminfo)
JOBS=$(nproc 2>/dev/null || echo 2)
[ "$JOBS" -gt 8 ] && JOBS=8
[ "$MEM" -lt 3000 ] && JOBS=2   # 1-2 GB: fewer compilers at once, or it runs out of memory
[ "$MEM" -lt 1500 ] && JOBS=1
TARGETS="PlayRX"
[ "$SIM" = 1 ] && TARGETS="$TARGETS PlayRX_sim gen_testsignal"
# a Raspberry Pi: compile for its exact processor (the decoder runs noticeably faster than a generic ARM build)
MODEL=$(tr -d '\0' < /proc/device-tree/model 2>/dev/null || echo "")
CPU=""
case "$MODEL" in
  *"Raspberry Pi 5"* | *"Compute Module 5"* | *"Raspberry Pi 500"*) CPU="cortex-a76" ;;
  *"Raspberry Pi 4"* | *"Compute Module 4"* | *"Raspberry Pi 400"*) CPU="cortex-a72" ;;
esac
FLAGS=""
[ -n "$CPU" ] && FLAGS="-mcpu=$CPU"
say "   ${MODEL:-$(uname -m)}; compiling for ${CPU:-this kind of processor} with $JOBS job(s)"
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
  >>"$LOG" 2>&1 || fail "cmake"
# shellcheck disable=SC2086
ninja -C build/linux -j "$JOBS" $TARGETS 2>&1 | tee -a "$LOG" | grep -E "^\[[0-9]+/[0-9]+\]" | awk 'NR % 10 == 0 {print "   " $1}'
[ "${PIPESTATUS[0]}" = 0 ] || fail "compile"

say "== 4/6 install to $DEST"
if systemctl is-active --quiet play-rx 2>/dev/null; then sudo systemctl stop play-rx; fi
sudo mkdir -p "$DEST"
sudo install -m 755 build/linux/play-rx "$DEST/play-rx"   # (the simulator stays in build/linux: it keeps its data next to itself)
sudo rm -rf "$DEST/web.new"
sudo cp -r web "$DEST/web.new"
sudo rm -rf "$DEST/web"
sudo mv "$DEST/web.new" "$DEST/web"
sudo install -m 644 VERSION "$DEST/VERSION"
sudo install -m 644 NOTICE.txt "$DEST/NOTICE.txt" 2>/dev/null || true
sudo install -m 644 LICENSE "$DEST/LICENSE" 2>/dev/null || true
sudo install -m 755 linux/uninstall.sh "$DEST/uninstall.sh"

if [ "$SERVICE" = 1 ]; then
  say "== 5/6 start at boot (systemd service play-rx, running as $ME)"
  command -v systemctl >/dev/null 2>&1 || fail "systemd not found: run with --no-service and start $DEST/play-rx yourself"
  sed "s/@USER@/$ME/" linux/play-rx.service.in | sudo tee /etc/systemd/system/play-rx.service >/dev/null
  sudo systemctl daemon-reload
  sudo systemctl enable play-rx >>"$LOG" 2>&1
  sudo systemctl restart play-rx || fail "starting the service (see: journalctl -u play-rx -n 50)"
  sleep 3
  systemctl is-active --quiet play-rx || fail "the service stopped (see: journalctl -u play-rx -n 50)"
else
  say "== 5/6 no service (--no-service): start it with  $DEST/play-rx"
fi

say "== 6/6 done: Play RX $VERSION is installed"
say ""
say "Open it in a browser:"
say "   on this computer:   http://localhost:8090/"
for ip in $(hostname -I 2>/dev/null); do
  case $ip in *:*) ;; *) say "   from another one:   http://$ip:8090/" ;; esac
done
say ""
say "Plug the adapters into USB 3 ports (blue), then add a feed per transmitter on the Feeds page."
say "If an adapter was plugged in before this install, unplug it and plug it back in once."
if [ "$SERVICE" = 1 ]; then
  say ""
  say "Useful:  journalctl -u play-rx -f        (its log)"
  say "         sudo systemctl restart play-rx   sudo systemctl stop play-rx   sudo systemctl disable play-rx"
fi
say "Data:    ~/.local/share/play-rx        Uninstall:  bash $DEST/uninstall.sh"
if [ "$SIM" = 1 ]; then
  say ""
  say "Simulator (no adapters), from this folder ($SRC):"
  say "   mkdir -p rec && build/linux/gen_testsignal rec/sim_R1 --mhz 5658 --label R1"
  [ "$SERVICE" = 1 ] && say "   sudo systemctl stop play-rx      (both use port 8090)"
  say "   build/linux/play-rx-sim          then http://localhost:8090/  (Ctrl+C to stop)"
fi
