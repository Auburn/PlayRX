#!/usr/bin/env bash
# Builds Play RX for macOS (Apple silicon) from this source folder and makes dist/PlayRX-<version>-mac.dmg.
# Full instructions, including Homebrew and troubleshooting: docs/INSTALL-MAC.md
#
#   bash mac/build.sh            the app
#   bash mac/build.sh --sim      also "Play RX Sim" (simulated adapters and transmitters: try it without hardware)
#   bash mac/build.sh --install  also copy "Play RX.app" into /Applications when done
#
# Needs Apple's Command Line Tools and Homebrew; installs cmake, ninja, pkg-config, libusb and jpeg-turbo with brew.
# Everything it prints also goes to build-mac.log (next to this folder's README.md).
# Unsigned (ad-hoc): the first open needs right-click > Open, or System Settings > Privacy & Security > Open Anyway.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
VERSION=$(tr -d ' \r\n' < VERSION)
SIM=0
INSTALL=0
for a in "$@"; do
  case $a in
    --sim) SIM=1 ;;
    --install) INSTALL=1 ;;
    *) echo "unknown option: $a (use --sim and/or --install)"; exit 2 ;;
  esac
done
LOG="$ROOT/build-mac.log"
: > "$LOG"
say() { echo "$@" | tee -a "$LOG"; }
fail() {
  say ""
  say "BUILD FAILED: $*"
  say "The last lines of build-mac.log:"
  tail -40 "$LOG"
  echo
  echo "See docs/INSTALL-MAC.md > Troubleshooting. When asking for help, attach build-mac.log."
  exit 1
}

say "== Play RX $VERSION for macOS ($(uname -m), macOS $(sw_vers -productVersion 2>/dev/null))"
[ "$(uname -s)" = Darwin ] || fail "this script is for macOS (on Linux use linux/install.sh)"
# A downloaded folder is quarantined (com.apple.quarantine on every file): macOS then refuses to run its scripts
# ("bad interpreter: Operation not permitted" from fw/mips-stub.sh, which the build runs as a program).
# Clear it for this folder only, and make sure the scripts the build runs are executable.
xattr -dr com.apple.quarantine "$ROOT" 2>/dev/null || true
chmod +x "$ROOT"/fw/*.sh "$ROOT"/mac/*.sh 2>/dev/null || true

say "== 1/5 tools"
if ! xcode-select -p >/dev/null 2>&1; then
  say "Apple's Command Line Tools are needed (the compiler). A window opens: click Install, wait for it to finish,"
  say "then run this script again."
  xcode-select --install
  exit 1
fi
if ! command -v brew >/dev/null 2>&1; then
  for b in /opt/homebrew/bin/brew /usr/local/bin/brew; do [ -x "$b" ] && eval "$("$b" shellenv)"; done
fi
command -v brew >/dev/null 2>&1 || fail "Homebrew is needed: install it from https://brew.sh (docs/INSTALL-MAC.md step 2), then run this again"
say "   Homebrew: $(brew --prefix)"
say "   installing / updating cmake ninja pkg-config libusb jpeg-turbo (a few minutes the first time)"
brew install cmake ninja pkg-config libusb jpeg-turbo >>"$LOG" 2>&1 || fail "brew install (try: brew update && brew doctor)"
BREW=$(brew --prefix)
[ -f third_party/kepyur/native/CMakeLists.txt ] || fail "third_party/kepyur is missing: use the playrx-source-<version>.tar.gz from Releases, or clone with --recursive (docs/INSTALL-MAC.md)"

say "== 2/5 build (2-5 minutes)"
TARGETS="PlayRX"
[ "$SIM" = 1 ] && TARGETS="$TARGETS PlayRX_sim gen_testsignal"
cmake -S . -B build/mac.noindex -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" >>"$LOG" 2>&1 || fail "cmake"
# shellcheck disable=SC2086
ninja -C build/mac.noindex $TARGETS >>"$LOG" 2>&1 || fail "compile"

say "== 3/5 libraries into the app"
# Homebrew's dylibs (libusb, libjpeg-turbo) go inside each .app (Contents/Frameworks), so the app runs on a Mac
# without Homebrew; their references are rewritten to @rpath.
bundle_libs() {
  local app="$1" exe fw changed=1
  exe="$app/Contents/MacOS/$(/usr/libexec/PlistBuddy -c "Print CFBundleExecutable" "$app/Contents/Info.plist")"
  fw="$app/Contents/Frameworks"
  mkdir -p "$fw"
  install_name_tool -add_rpath "@executable_path/../Frameworks" "$exe" 2>/dev/null || true
  while [ "$changed" = 1 ]; do
    changed=0
    for bin in "$exe" "$fw"/*.dylib; do
      [ -f "$bin" ] || continue
      for dep in $(otool -L "$bin" | awk 'NR>1 {print $1}' | grep -E "^($BREW|/usr/local|/opt/homebrew)/"); do
        local name
        name=$(basename "$dep")
        if [ ! -f "$fw/$name" ]; then
          cp -L "$dep" "$fw/$name"
          chmod u+w "$fw/$name"
          install_name_tool -id "@rpath/$name" "$fw/$name"
          changed=1
        fi
        install_name_tool -change "$dep" "@rpath/$name" "$bin"
      done
    done
  done
}
mkdir -p dist
OUT="$ROOT/build/mac.noindex/apps"   # ".noindex": Spotlight skips it, so only the installed app shows up
mkdir -p "$OUT"
APPS=()
for t in $TARGETS; do
  case $t in
    PlayRX) name="Play RX" ;;
    PlayRX_sim) name="Play RX Sim" ;;
    *) continue ;;
  esac
  src="build/mac.noindex/$name.app"
  [ -d "$src" ] || fail "$src was not built"
  rm -rf "${OUT:?}/${name:?}.app"
  cp -R "$src" "$OUT/$name.app"
  bundle_libs "$OUT/$name.app" >>"$LOG" 2>&1 || fail "bundling libraries into $name"
  APPS+=("$OUT/$name.app")
done

say "== 4/5 sign (ad-hoc)"
for a in "${APPS[@]}"; do
  codesign --force --deep --sign - "$a" >>"$LOG" 2>&1 || fail "codesign $a"
done

say "== 5/5 disk image"
STAGE=$(mktemp -d)
cp -R "$OUT/Play RX.app" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
DMG="dist/PlayRX-$VERSION-mac.dmg"
rm -f "$DMG"
hdiutil create -volname "Play RX $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >>"$LOG" 2>&1 || fail "hdiutil"
rm -rf "$STAGE"

if [ "$INSTALL" = 1 ]; then
  say "== installing to /Applications"
  if pgrep -x "Play RX" >/dev/null 2>&1; then
    say "   Play RX is running: quit it first (Play RX menu > Quit), then run this again with --install"
  else
    rm -rf "/Applications/Play RX.app"
    cp -R "$OUT/Play RX.app" /Applications/ || fail "copying to /Applications"
    xattr -dr com.apple.quarantine "/Applications/Play RX.app" 2>/dev/null || true
    say "   /Applications/Play RX.app"
  fi
fi

say ""
say "Done:"
say "  $ROOT/$DMG"
say "  Open it and drag Play RX to Applications (or run this again with --install)."
if [ "$SIM" = 1 ]; then
  say "  Play RX Sim (no adapters needed), from this folder in Terminal:"
  say "    mkdir -p rec && build/mac.noindex/gen_testsignal rec/sim_R1 --mhz 5658 --label R1"
  say "    \"$OUT/Play RX Sim.app/Contents/MacOS/Play RX Sim\""
fi
open -R "$DMG" 2>/dev/null || true
