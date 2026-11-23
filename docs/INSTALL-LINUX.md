# Play RX on Linux

On Linux, Play RX runs **headless**: there is no app window. It runs in the background (as a system service) and
you use it from a web browser, on the same computer (`http://localhost:8090/`) or from any other computer,
tablet or phone on the network. One script, `linux/install.sh`, installs the packages it needs, builds Play RX,
installs it to `/opt/play-rx` and starts it.

Reckon on 10 minutes on a PC (20–30 on a Raspberry Pi) the first time.

## What you need

- **64-bit Linux** with `apt`: **Debian 12** or newer, **Ubuntu 22.04** or newer (and Linux Mint, Pop!_OS …),
  or **Raspberry Pi OS 64-bit** (Bookworm or newer). Fedora (`dnf`) and Arch (`pacman`) are tried by the script
  but less tested. Older systems (Ubuntu 20.04, Debian 11) have a compiler and CMake that are too old.
- `sudo` rights (the script asks for your password for the system parts).
- About 2 GB of free disk space, and an internet connection for the packages.
- **RTL8812BU adapters** (TP-Link Archer T4U v3, Cudy AC1300, …) on **USB 3** ports (usually blue). At USB 2 the
  picture stays black.
- **Raspberry Pi:** a **Pi 5** is recommended. A Pi 4 manages **one** adapter at best (its CPU is the limit),
  and it needs good cooling. Use the blue USB 3 ports.

On Linux, Play RX has the Live view, OBS sources (Browser Source / MJPEG URLs), the band scan and feed nodes.
Low latency mode, FPV Trackside streams, webcams and USB capture cards are not available on Linux yet. A Linux
box makes a good **feed node**: its adapters' pictures go over the network to Play RX on a Windows PC or Mac.

## 1. Open a terminal

Desktop: open **Terminal** from the applications menu (often **Ctrl+Alt+T**). A Raspberry Pi without a screen:
connect with SSH from another computer, e.g. `ssh you@raspberrypi.local`.

Type or paste each command and press **Enter**. Run them as **your normal user**, not as root and not with
`sudo` in front (the script uses `sudo` itself when needed).

## 2. Get the source

**Recommended: the release's source archive.** It already contains everything (kepyur, its submodules and the
patches applied), so no git is needed. Replace `0.9.2.80` with the newest version on the
[Releases](https://github.com/scerstt/PlayRX/releases) page:

```bash
cd ~
V=0.9.2.80
wget https://github.com/scerstt/PlayRX/releases/download/v$V/playrx-source-$V.tar.gz
tar xzf playrx-source-$V.tar.gz
cd playrx-source-$V
```

(No `wget`? `sudo apt install wget`, or use `curl -LO <the same address>`. Or download the file in a browser and
unpack it from your Downloads folder.)

**Or with git** (for developers):

```bash
sudo apt install git
cd ~
git clone --recursive https://github.com/scerstt/PlayRX.git
cd PlayRX
(cd third_party/kepyur && git apply ../../patches/kepyur-000*.patch)
(cd third_party/kepyur/third_party/wifi-sdr && git apply ../../../../patches/wifi-sdr-0001-windows.patch)
(cd third_party/kepyur/third_party/wifi-sdr/external/devourer && git apply ../../../../../../patches/devourer-0001-windows-usb.patch)
(cd third_party/kepyur/third_party/libusb && git apply ../../../../patches/libusb-0001-winusb-raw-io.patch)
```

## 3. Install

In that folder:

```bash
bash linux/install.sh
```

It shows 6 steps, and everything also goes into `install.log` in that folder:

1. **packages:** compiler, CMake, Ninja, pkg-config, libusb, libjpeg-turbo, xxd;
2. **the adapters:** a udev rule so Play RX can use them without root, and Linux's own RTL8812BU Wi-Fi drivers
   (`rtw88_8822bu`, `88x2bu`) blocked so they don't take the adapters (other Wi-Fi cards are not affected);
3. **build;**
4. **install** to `/opt/play-rx`;
5. **service:** `play-rx`, started now and at every boot, running as you;
6. the addresses to open.

Options: `--no-service` installs without the service (start it yourself with `/opt/play-rx/play-rx`, stop it with
Ctrl+C); `--sim` also builds the simulator (see [Trying it without adapters](#trying-it-without-adapters)).

## 4. Open it

- On the same computer: **http://localhost:8090/**
- From another computer, tablet or phone on the same network: **http://&lt;address&gt;:8090/**, using one of the
  addresses the script printed (e.g. `http://192.168.1.50:8090/`), or `http://<hostname>.local:8090/` where
  `.local` names work.

Then plug the adapters into USB 3 ports (if one was already plugged in during the install, unplug it and plug it
back in once), open **Feeds** and add a feed per transmitter: its picture appears on **Live**.

> On Linux the web page is always open to the network (there is no window to use instead). Anyone on your
> network can open it, so use it on a network you trust.

Settings, feeds and adapter names are kept in `~/.local/share/play-rx`.

## Day to day

```bash
systemctl status play-rx            # running?
journalctl -u play-rx -f            # its log, live (Ctrl+C to stop watching)
sudo systemctl restart play-rx      # restart
sudo systemctl stop play-rx         # stop until the next boot
sudo systemctl disable --now play-rx   # stop and don't start at boot any more (enable --now: back on)
```

The **Log** page in the web UI shows the same log.

## Updating

Get the new source archive (step 2) and run `bash linux/install.sh` in the new folder. It stops the service,
installs the new version and starts it again. Your settings stay. The old source folder can be deleted.

## Uninstalling

```bash
bash /opt/play-rx/uninstall.sh
```

It removes the service, `/opt/play-rx`, the udev rule and the driver block (unplug and replug an adapter, or
restart, and it works as a Wi-Fi card again). Your settings stay in `~/.local/share/play-rx`; delete that folder
too for a clean slate. The packages from step 1 stay installed (they are common development packages).

## Trying it without adapters

```bash
bash linux/install.sh --sim
mkdir -p rec
build/linux/gen_testsignal rec/sim_R1 --mhz 5658 --label R1
build/linux/gen_testsignal rec/sim_R3 --mhz 5732 --label R3
sudo systemctl stop play-rx         # the simulator uses the same port, 8090
build/linux/play-rx-sim
```

Run it from the source folder (it finds the test transmitters in `rec/`), open http://localhost:8090/ and add
feeds on R1 and R3. Ctrl+C stops it; `sudo systemctl start play-rx` brings the real one back.

## Troubleshooting

First look at the log: `journalctl -u play-rx -n 100` (or the **Log** page). For install problems, the end of
`install.log` names the step that failed. When asking for help, attach `install.log` and the output of
`journalctl -u play-rx -n 200`, and say which computer and Linux version you have (`cat /etc/os-release`).

### `INSTALL FAILED: apt-get update` / `apt-get install`
Check the internet connection and that `sudo apt update` works on its own; fix what it complains about (a broken
third-party repository in `/etc/apt/sources.list.d/` is the usual cause), then run the script again.
"Unable to locate package xxd": on older Ubuntu/Debian it is in `vim-common` (`sudo apt install vim-common`).

### `INSTALL FAILED: cmake` (`CMake 3.22 or higher is required`) or `compile` with C++20 errors
The system is too old (Ubuntu 20.04, Debian 11). Use Ubuntu 22.04+, Debian 12+ or Raspberry Pi OS Bookworm.

### `INSTALL FAILED: compile` on a Raspberry Pi, or it freezes while building
Usually out of memory. The script already builds with fewer jobs on a small Pi. Close other programs, make sure
the Pi isn't overheating (`vcgencmd measure_temp`), and run the script again: it continues where it stopped.
On a 1–2 GB Pi, more swap helps: `sudo dphys-swapfile swapoff`, set `CONF_SWAPSIZE=2048` in
`/etc/dphys-swapfile`, then `sudo dphys-swapfile setup && sudo dphys-swapfile swapon`.

### `BUILD FAILED`/`INSTALL FAILED: third_party/kepyur is missing`
You downloaded GitHub's green **Code → Download ZIP** (or the automatic "Source code" archives). Those leave out
kepyur. Use **`playrx-source-<version>.tar.gz`** from the release, or `git clone --recursive` (step 2).

### `INSTALL FAILED: the service stopped`
`journalctl -u play-rx -n 50` says why. Common ones:
- *address already in use* / port 8090: something else uses it (another Play RX, or the simulator):
  `sudo ss -ltnp | grep 8090` shows what.
- Start it by hand to see everything: `sudo systemctl stop play-rx; /opt/play-rx/play-rx` (Ctrl+C to stop).

### No adapter listed
1. Is it on the USB bus?
   ```bash
   lsusb | grep -iE "0bda:b812|2357:0115"
   ```
   Nothing: cable, hub or port, or it is not an RTL8812BU (an RTL8812AU shows `0bda:8812` and doesn't work).
2. Has a Wi-Fi driver taken it?
   ```bash
   lsmod | grep -iE "8822bu|88x2bu"
   ```
   If that lists a driver: `sudo modprobe -r rtw88_8822bu 88x2bu`, unplug and replug the adapter. The blacklist
   file from the install (`/etc/modprobe.d/play-rx-blacklist.conf`) keeps it off after a restart. A driver
   installed with DKMS under another name: `lsusb -t` shows `Driver=` for the adapter; add a
   `blacklist <that name>` line to the file.
3. Permission ("LIBUSB_ERROR_ACCESS" in the log): the udev rule only applies to adapters plugged in after it was
   installed: unplug and replug, or restart. Check it is there: `cat /etc/udev/rules.d/60-play-rx-rtl8812bu.rules`.

### Black picture, or "no packet within timeout" in the log
The adapter is at **USB 2**. Check:
```bash
lsusb -t
```
The adapter's line must end in **5000M** (USB 3), not **480M**. Use a blue USB 3 port (on a Raspberry Pi the two
blue ones in the middle), avoid unpowered hubs and long extension cables, unplug and replug. Two adapters on one
USB 3 controller is about the limit.

### The picture breaks up, or the Pi gets slow
The CPU can't keep up: on a Pi 4 use one adapter, turn off Diversity and Sparkle repair in Settings, and make sure
it isn't throttling (`vcgencmd get_throttled` should say `0x0`; add a fan/heatsink, use the official power
supply). A Pi 5 or a PC does much better.

### Other computers can't open the page
- Use the address from `hostname -I` (`http://<that address>:8090/`); `.local` names don't work on every network.
- A firewall: Ubuntu `sudo ufw allow 8090/tcp`; Fedora `sudo firewall-cmd --permanent --add-port=8090/tcp && sudo firewall-cmd --reload`.

### I want the adapter back as a Wi-Fi card
Uninstall (above), or delete `/etc/modprobe.d/play-rx-blacklist.conf`, then replug the adapter or restart.

### Start again from scratch
```bash
bash /opt/play-rx/uninstall.sh
rm -rf ~/.local/share/play-rx
rm -rf build/linux
bash linux/install.sh
```
