# Play RX

Analog 5.8 GHz FPV video on a PC or Mac, received with cheap **RTL8812BU USB Wi-Fi adapters** used as
software-defined radios. Each adapter receives one transmitter; Play RX decodes the picture and shows it in its
window, as OBS sources, as streams for FPV Trackside and (on Windows) as webcams for other programs.

Built on [kepyur](https://github.com/gilankpam/kepyur), which does the radio and the video decoding through
[wifi-sdr](https://github.com/gilankpam/wifi-sdr) and [devourer](https://github.com/OpenIPC/devourer).

> **Status: experimental.** It works on the hardware it was developed with (TP-Link Archer T4U v3 and Cudy
> AC1300 adapters on USB 3); other PCs and adapters may behave differently. Not affiliated with Realtek, TP-Link,
> Cudy, Apple, OBS or FPV Trackside.

## Install

| System | How | Guide |
|---|---|---|
| **Windows** 10 / 11 (64-bit) | Installer from [Releases](../../releases) | [below](#windows) |
| **macOS** 13 or newer, Apple silicon (M1 or newer) | One build script (installs everything it needs with Homebrew) | **[docs/INSTALL-MAC.md](docs/INSTALL-MAC.md)** |
| **Linux** (Debian 12+, Ubuntu 22.04+, Raspberry Pi OS 64-bit) | One install script; runs as a service, UI in the browser | **[docs/INSTALL-LINUX.md](docs/INSTALL-LINUX.md)** |

## What it does

- **Live:** every feed's picture at once (click one to show it alone, F11 for full screen, a channel picker on
  each tile).
- **Low latency mode** (Windows): one feed in its own window, each line drawn the moment it arrives instead of a
  whole picture at a time. Click **Low latency** on the Live page (it takes the feed you clicked, else the first);
  F11 or double-click in that window for full screen, which is the lowest latency. Optionally pause the other
  feeds while it is on. The Live page shows how old the newest line is.
- **Feeds:** up to 8, each a name and a channel (Raceband by default; Fatshark, Boscam A/B/E and Lowband can be
  turned on). A feed can also take its picture from another computer running Play RX as a feed node, and on
  Windows from a USB video receiver / capture card.
- **OBS:** a Browser Source URL per feed (`/pilot/N`, 4:3 at 1440 × 1080), raw MJPEG (`/feed/N.mjpg`) and
  snapshots, with copy buttons and previews.
- **Video out:** each feed to FPV Trackside as an RTMP stream (Windows and macOS), and each feed as a Windows
  camera ("Play RX Pilot 1" …) for Discord, Zoom, OBS, Trackside and so on (Windows).
- **Adapters & scan:** which adapter feeds which slot, names for adapters, and a scan of the band for transmitters.
- **Picture:** comb filter, adaptive comb (clean OSD text), smooth lines, sparkle repair, diversity (both
  antennas of an adapter combined), tuning beside the channel. None of them adds delay.
- The same UI works in any browser at `http://localhost:8090/`; turn on network access in Settings for tablets
  and other computers (on Linux it is always on).

Play RX shows live video only: it does not record.

| | Windows | macOS | Linux |
|---|---|---|---|
| Live view, OBS sources, feed node | ✓ | ✓ | ✓ (in the browser) |
| Low latency mode | ✓ | – | – |
| FPV Trackside (RTMP) | ✓ | ✓ (new, little tested) | – |
| Webcams for other programs | ✓ | – | – |
| USB video receivers / capture cards as feeds | ✓ | – | – |

## Hardware

- **Adapters:** RTL8812BU dongles. Known to work: **TP-Link Archer T4U v3** (USB id `2357:0115`), **Cudy AC1300**,
  and Realtek's own id (`0bda:b812`). The RTL8812AU (T4U v1/v2, many AC1200 sticks), Wi-Fi 6/7 dongles and combo
  Wi-Fi/Bluetooth dongles do **not** work.
- **USB 3**, one port per adapter. At USB 2 an adapter cannot stream fast enough for video. Each needs about
  160 MB/s, so plan on at most about two adapters per USB 3 controller. On a Mac or with a hub, use a good
  USB 3 hub: a marginal one drops the adapter back to USB 2 (a black picture).
- Tested with Cudy AC1300 - UK Amazon (No affiliate link) https://www.amazon.co.uk/dp/B084DNWSXK

## Windows

Windows 10 or 11, 64-bit, with the Microsoft Edge WebView2 Runtime (part of Windows 11 and most Windows 10 PCs).

1. Download `PlayRX-Setup-<version>.exe` from [Releases](../../releases).
2. Run it. The installer isn't code-signed, so Windows SmartScreen may say *Windows protected your PC*: click
   **More info → Run anyway**.
3. Plug the adapters in **before** installing and leave **Set up the USB adapters now** ticked. Play RX drives
   the adapters itself through libusb, so they need Windows' generic **WinUSB** driver instead of their Wi-Fi
   driver. The installer sets that up for every RTL8812BU plugged in. A new adapter later: Start menu →
   **Play RX - Set up adapters**. (By hand instead: [Zadig](https://zadig.akeo.ie/) → Options → List All Devices,
   pick the adapter, WinUSB, Replace Driver.) An adapter set to WinUSB no longer works as a Wi-Fi card until its
   vendor driver is reinstalled in Device Manager.
4. Start Play RX, add a feed per transmitter on the **Feeds** page, and the pictures appear on **Live**.

Settings, feeds and adapter names are kept in `%APPDATA%\Play RX`. Uninstalling leaves them.

## Build from source (Windows)

macOS and Linux: the scripts in the guides above build from source for you.

In an [MSYS2](https://www.msys2.org) **UCRT64** shell:

```bash
git clone --recursive https://github.com/scerstt/PlayRX.git && cd PlayRX
scripts/setup-msys2.sh            # GCC, CMake, Ninja, libusb, webview, NSIS (once)
scripts/build-libusb.sh           # libusb with WinUSB RAW_IO, needed for 40 MS/s (once; optional at 20 MS/s)
# kepyur is used unchanged upstream plus the patches in patches/ (see patches/README.md):
(cd third_party/kepyur && git apply ../../patches/kepyur-000*.patch)
(cd third_party/kepyur/third_party/wifi-sdr && git apply ../../../../patches/wifi-sdr-0001-windows.patch)
(cd third_party/kepyur/third_party/wifi-sdr/external/devourer && git apply ../../../../../../patches/devourer-0001-windows-usb.patch)
(cd third_party/kepyur/third_party/libusb && git apply ../../../../patches/libusb-0001-winusb-raw-io.patch)
cmake -S . -B build -G Ninja
cmake --build build
```

That gives `build/PlayRX.exe`, `build/PlayLeagueCam.dll` (the virtual cameras; the name is historical) and
`build/PlayRX_sim.exe`. Each release also has `playrx-source-<version>.tar.gz`: the same source with the
submodules and patches already in place, so it builds without git or `git apply` (this is also what the macOS
and Linux guides use).

- **Without hardware:** `PlayRX_sim.exe` runs the whole app with simulated adapters and transmitters. Make test
  transmitters first, e.g. `mkdir rec && build/gen_testsignal.exe rec/sim_R1 --mhz 5658 --label R1` (and
  `rec/sim_R3 --mhz 5732 --label R3` …); the simulator uses every `rec/sim_*.cs16` (with its `.json`).
- **The installer:** `scripts/build-libwdi.sh` (the driver step, optional), then `scripts/package.sh` →
  `dist/PlayRX-Setup-<version>.exe`. `scripts/make-source.sh` → `dist/playrx-source-<version>.tar.gz`.
- **The adapter's on-chip programs** (small MIPS programs that run on the adapter's own CPU) come prebuilt in
  `fw/prebuilt-fw`. To assemble them from wifi-sdr's source instead: `scripts/setup-msys2.sh --mipsel`, then
  `cmake -S . -B build -G Ninja -DARX_ASSEMBLE_FW=ON -DMIPS_AS=/opt/mipsel/bin/mipsel-linux-gnu-as -DMIPS_LD=/opt/mipsel/bin/mipsel-linux-gnu-ld -DMIPS_OBJCOPY=/opt/mipsel/bin/mipsel-linux-gnu-objcopy`.

## Licence

Play RX is free software under the **GNU General Public License, version 2** ([LICENSE](LICENSE)), as required by
wifi-sdr and devourer, which it is built on. It comes with no warranty. Built with:

| Component | Licence |
|---|---|
| [kepyur](https://github.com/gilankpam/kepyur) (decoder, radio) | GPL-2.0 |
| [wifi-sdr](https://github.com/gilankpam/wifi-sdr) (raw samples from the adapter) | GPL-2.0-only |
| [devourer](https://github.com/OpenIPC/devourer) (userspace Realtek USB driver) | GPL-2.0 |
| [libusb](https://libusb.info) | LGPL-2.1 |
| [libwdi](https://github.com/pbatard/libwdi) (driver setup in the Windows installer) | LGPL-3.0 |
| [webview](https://github.com/webview/webview) (the window: Microsoft Edge WebView2 on Windows, WKWebView on macOS) | MIT |
| [libjpeg-turbo](https://libjpeg-turbo.org) (macOS, Linux) | IJG / BSD-style |
| winpthreads (MinGW-w64 runtime, Windows) | MIT-style |

devourer contains Realtek's firmware for the RTL8822B chip (as a C array), which ends up inside the Play RX
program. That firmware is Realtek's, not covered by the GPL; it is distributed as devourer distributes it. See
[NOTICE.txt](NOTICE.txt).
