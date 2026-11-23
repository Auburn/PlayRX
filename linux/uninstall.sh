#!/usr/bin/env bash
# Removes Play RX from Linux: the service, /opt/play-rx, the udev rule and the driver blacklist.
# Your settings stay in ~/.local/share/play-rx (delete that folder too for a clean slate).
#   bash /opt/play-rx/uninstall.sh      (or: bash linux/uninstall.sh from the source folder)
set -u
if [ "$(id -un)" = root ]; then
  echo "Run this as your normal user (not with sudo): it asks for your password when it needs it."
  exit 1
fi
echo "Removing Play RX..."
if systemctl list-unit-files play-rx.service >/dev/null 2>&1; then
  sudo systemctl disable --now play-rx 2>/dev/null || true
  sudo rm -f /etc/systemd/system/play-rx.service
  sudo systemctl daemon-reload
fi
sudo rm -f /etc/udev/rules.d/60-play-rx-rtl8812bu.rules /etc/modprobe.d/play-rx-blacklist.conf
sudo udevadm control --reload-rules 2>/dev/null || true
sudo rm -rf /opt/play-rx
echo "Done. Settings kept in ~/.local/share/play-rx."
echo "The adapters work as Wi-Fi cards again after they are unplugged and plugged back in (or after a restart)."
