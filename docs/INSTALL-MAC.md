# Play RX on macOS

Play RX is built on your Mac from its source by one script, `mac/build.sh`. The script installs what it needs
with Homebrew, builds the app, puts the libraries it uses inside it and makes a disk image (`.dmg`). This page
takes you through it from a Mac that has never built anything, then covers the usual problems.

Reckon on 20–30 minutes the first time (mostly downloads), and 2–5 minutes for later updates.

## What you need

- A Mac with **Apple silicon** (M1, M2, M3, M4, M5 …). Intel Macs are untested: the steps are the same and may
  well work, but nobody has tried yet.
- **macOS 13 Ventura or newer** (Apple menu  → About This Mac).
- An administrator account (for the Homebrew install).
- About 3 GB of free disk space for the tools and the build.
- **RTL8812BU adapters** (TP-Link Archer T4U v3, Cudy AC1300, …) on **USB 3**. On a Mac that usually means a
  USB‑C to USB‑A adapter or a USB 3 hub: use a **good USB 3 hub**. A marginal hub drops the adapter back to
  USB 2 and the picture stays black (see [Troubleshooting](#black-picture-or-no-packet-within-timeout)).
- No driver is needed on a Mac: Play RX talks to the adapters directly.

On a Mac, Play RX has the Live view, OBS sources, FPV Trackside streams, the band scan and feed nodes. Low latency
mode, webcams for other programs and USB capture cards are Windows only for now.

## 1. Open Terminal

Everything below is typed into **Terminal**: press **⌘ Space**, type `Terminal`, press **Return**.

Paste each command (⌘V) and press **Return**. Wait for it to finish (the prompt, ending in `%`, comes back)
before the next one.

## 2. Apple's Command Line Tools (the compiler)

```bash
xcode-select --install
```

A window opens: click **Install**, accept the licence and wait (5–15 minutes). If it says *command line tools
are already installed*, that's fine: go on.

Check:

```bash
xcode-select -p
```

It prints a folder such as `/Library/Developer/CommandLineTools`. (A full Xcode from the App Store works too.)

## 3. Homebrew

[Homebrew](https://brew.sh) is the package manager the build uses to get CMake, Ninja, libusb and
libjpeg-turbo. Skip to step 4 if `brew --version` already prints a version.

Install it:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

- It asks for **your Mac password** (nothing shows while you type: that's normal) and then to press **Return**.
- It may install the Command Line Tools again if step 2 hadn't finished: let it.
- At the end it prints **Next steps** with two commands. Run them. On Apple silicon they are:

```bash
echo >> ~/.zprofile
echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zprofile
eval "$(/opt/homebrew/bin/brew shellenv)"
```

(On an Intel Mac Homebrew lives in `/usr/local` and is already on the path.)

Check, in a **new** Terminal window (⌘N):

```bash
brew --version
brew doctor
```

`brew --version` prints e.g. `Homebrew 4.x.x`. `brew doctor` should say *Your system is ready to brew*;
warnings about things you didn't install yourself can usually be ignored.

You don't need to install anything else with brew by hand: the build script runs
`brew install cmake ninja pkg-config libusb jpeg-turbo` for you.

## 4. Get the source

**Recommended: the release's source archive.** It already contains everything (kepyur, its submodules and the
patches applied), so no git is needed.

1. Open [Releases](https://github.com/scerstt/PlayRX/releases) and download **`playrx-source-<version>.tar.gz`** (the newest).
2. Unpack it into your home folder:

```bash
cd ~
tar xzf ~/Downloads/playrx-source-*.tar.gz
ls -d ~/playrx-source-*
```

The last line shows the folder, e.g. `/Users/you/playrx-source-0.9.2.80`. (Double-clicking the archive in
Finder also works; it unpacks into Downloads. Then use that folder below.)

**Or with git** (for developers; the Command Line Tools include git):

```bash
cd ~
git clone --recursive https://github.com/scerstt/PlayRX.git
cd PlayRX
(cd third_party/kepyur && git apply ../../patches/kepyur-000*.patch)
(cd third_party/kepyur/third_party/wifi-sdr && git apply ../../../../patches/wifi-sdr-0001-windows.patch)
(cd third_party/kepyur/third_party/wifi-sdr/external/devourer && git apply ../../../../../../patches/devourer-0001-windows-usb.patch)
(cd third_party/kepyur/third_party/libusb && git apply ../../../../patches/libusb-0001-winusb-raw-io.patch)
```

## 5. Build

Go into the folder from step 4 and run the build script with `bash` (replace the version with yours):

```bash
cd ~/playrx-source-0.9.2.80
bash mac/build.sh --install
```

- Always start it with **`bash`**: macOS blocks double-clicked scripts and `.command` files from the internet
  ("damaged" or "cannot be opened").
- It prints 5 steps: tools, build, libraries, sign, disk image. Everything also goes into `build-mac.log` in that
  folder.
- `--install` also copies **Play RX.app** into **Applications** at the end. Without it, the script opens the
  folder with `dist/PlayRX-<version>-mac.dmg`: open the .dmg and drag **Play RX** onto **Applications**.
- `--sim` also builds **Play RX Sim**, which simulates adapters and transmitters (see
  [Trying it without adapters](#trying-it-without-adapters)).

When it ends with **Done:**, it worked.

## 6. First start

1. Open **Applications** → **Play RX**.
2. A Play RX you built yourself opens straight away. If macOS still says it *cannot be opened because Apple cannot
   check it for malicious software* (for example a .dmg someone else built and sent you):
   - **macOS 15 Sequoia and newer:** click **Done**, then  → **System Settings** → **Privacy & Security**,
     scroll down to *"Play RX" was blocked…* and click **Open Anyway**, then **Open Anyway** again and enter
     your password.
   - **macOS 13–14:** right-click (or Control-click) **Play RX** in Applications → **Open** → **Open**.
   - Or in Terminal: `xattr -dr com.apple.quarantine "/Applications/Play RX.app"`
   You only do this once.
3. macOS asks whether Play RX may **find devices on your local network**: click **Allow**. (It serves its video
   to OBS, FPV Trackside and other computers. You can change this later in System Settings → Privacy & Security
   → Local Network.)
4. Plug the adapters in (USB 3), open the **Feeds** page and add a feed per transmitter: its picture appears on
   **Live**.

Settings, feeds and adapter names are kept in `~/Library/Application Support/Play RX`.

## Updating

Download the new `playrx-source-<version>.tar.gz`, unpack it as in step 4, **quit Play RX**, then run
`bash mac/build.sh --install` in the new folder. Brew packages are already there, so it is quick. Your settings
stay. The old source folder can be deleted.

## Uninstalling

1. Quit Play RX and drag **Applications → Play RX** to the Bin.
2. Your settings (optional): `rm -rf ~/Library/Application\ Support/Play\ RX`
3. The source folder (e.g. `rm -rf ~/playrx-source-0.9.2.80`).
4. The Homebrew packages, only if nothing else uses them: `brew uninstall ninja libusb jpeg-turbo`
   (cmake and pkg-config are often used by other things: keep them unless you know).

## Trying it without adapters

```bash
cd ~/playrx-source-0.9.2.80
bash mac/build.sh --sim
mkdir -p rec
build/mac.noindex/gen_testsignal rec/sim_R1 --mhz 5658 --label R1
build/mac.noindex/gen_testsignal rec/sim_R3 --mhz 5732 --label R3
"build/mac.noindex/apps/Play RX Sim.app/Contents/MacOS/Play RX Sim"
```

Start it from that folder like this (not from Finder): it finds the test transmitters in `rec/`. Add feeds on R1
and R3. Close the window to stop it. Quit the real Play RX first: both use port 8090.

## Troubleshooting

The **Log** page in Play RX shows what the app is doing; most adapter problems are explained there. For build
problems, the end of `build-mac.log` names the step that failed. When asking for help, attach that file (or
copy the Log page) and say which Mac and macOS you have.

### `xcrun: error: invalid active developer path` or `no developer tools were found`
The Command Line Tools are missing or were removed by a macOS update. Run `xcode-select --install` again (step 2).
If that says they are installed but the error stays:
`sudo rm -rf /Library/Developer/CommandLineTools && xcode-select --install`.

### `brew: command not found`
Homebrew is installed but not on the path in this window. Run the three `echo`/`eval` lines from step 3, then
open a new Terminal window. Check that `/opt/homebrew/bin/brew --version` works; if that file doesn't exist,
Homebrew isn't installed: run the install command in step 3 again.

### `BUILD FAILED: brew install`
Usually a stale Homebrew. Run:
```bash
brew update
brew doctor
brew install cmake ninja pkg-config libusb jpeg-turbo
```
and follow what `brew doctor` says. Behind a company proxy or firewall, brew needs internet access to github.com.
"Permission denied" in `/opt/homebrew`: `sudo chown -R $(whoami) /opt/homebrew`.

### `BUILD FAILED: cmake`: `Could NOT find PkgConfig` / `libturbojpeg` / `libusb-1.0` not found
```bash
brew reinstall pkg-config jpeg-turbo libusb
rm -rf build/mac.noindex
bash mac/build.sh --install
```

### `BUILD FAILED: third_party/kepyur is missing`
You downloaded GitHub's green **Code → Download ZIP** (or the automatic "Source code" archives on the release
page). Those leave out kepyur. Download **`playrx-source-<version>.tar.gz`** from the release instead, or use
`git clone --recursive` (step 4).

### `bad interpreter: Operation not permitted` / `Permission denied` on a `.sh` file
macOS quarantined the downloaded files. The build script clears this for its own folder; if you see it anyway:
```bash
xattr -dr com.apple.quarantine ~/playrx-source-0.9.2.80
chmod +x ~/playrx-source-0.9.2.80/fw/*.sh ~/playrx-source-0.9.2.80/mac/*.sh
```

### `BUILD FAILED: compile`
Look at the first `error:` in `build-mac.log`. After switching versions or macOS updates, start clean:
`rm -rf build/mac.noindex` and build again. Keep the Command Line Tools current
(System Settings → General → Software Update).

### "Play RX is damaged and can't be opened" / "cannot be opened"
That's macOS's quarantine on an unsigned app, not damage. See step 6.2, or:
`xattr -dr com.apple.quarantine "/Applications/Play RX.app"`.

### The window stays blank or white
Open `http://localhost:8090/` in Safari. If the page works there, quit and reopen Play RX. If Safari can't open
it either, another copy of Play RX (or Play RX Sim) is running: quit it (Activity Monitor → search "Play RX" →
Quit), then start Play RX again.

### No adapter listed
- Check the Mac sees it:  → hold **Option** → **System Information…** → **USB**. The adapter shows as
  **802.11ac NIC** (or similar). If it isn't there, it's the cable, hub or port.
- Only RTL8812BU adapters work (see the README). An RTL8812AU looks almost the same and doesn't.
- Unplug it, wait 5 seconds, plug it back in.

### Black picture, or "no packet within timeout" on the Log page
The adapter is running at **USB 2**. In System Information → USB → the adapter, **Link Speed** (or Speed) must say
**5 Gb/s**, not **480 Mb/s**. Play RX switches the adapter to USB 3 when it starts, so:
- unplug the adapter and plug it straight back in;
- try another port or **another USB 3 hub**: some hubs and USB‑C dongles are USB 3 only on paper, or drop back to
  USB 2 under load (this was the cause on the developer's Mac: a different hub fixed it);
- avoid long USB extension cables.

### The picture is there but breaks up
Too many adapters on one hub or controller (each needs about 160 MB/s): spread them out, or try 20 MS/s in
Settings. Check the transmitter's channel and power, and the antennas.

### Other computers or OBS on another computer can't open the feeds
Turn on network access in Play RX's Settings, allow Play RX in System Settings → Privacy & Security →
Local Network, and if the macOS firewall is on (System Settings → Network → Firewall), allow Play RX there.
Use the address Settings shows (`http://<your-mac>:8090/`).

### Start again from scratch
Quit Play RX, delete `~/Library/Application Support/Play RX` and `build/mac.noindex` in the source folder, then
`bash mac/build.sh --install`.
