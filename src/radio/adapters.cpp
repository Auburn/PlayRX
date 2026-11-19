#include "adapters.h"

#include <libusb.h>

#include <memory>

#include "receiver.h"  // wsdr::model_pid, kPid8812bu
#include "usbmode.h"   // wsdr::request_usb3

namespace arx {

std::vector<AdapterInfo> list_adapters() {
  std::vector<AdapterInfo> out;
  libusb_context *ctx = nullptr;
  if (libusb_init(&ctx) != 0) return out;
  std::unique_ptr<libusb_context, void (*)(libusb_context *)> guard(ctx, libusb_exit);
  libusb_device **list = nullptr;
  const ssize_t n = libusb_get_device_list(ctx, &list);
  for (ssize_t i = 0; i < n; i++) {
    libusb_device_descriptor dd{};
    if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
    if (wsdr::model_pid(dd.idVendor, dd.idProduct) != wsdr::kPid8812bu) continue;
    AdapterInfo a;
    a.bus = libusb_get_bus_number(list[i]);
    uint8_t p[8];
    const int np = libusb_get_port_numbers(list[i], p, sizeof p);
    for (int k = 0; k < np; k++) a.port += (k ? "." : "") + std::to_string(p[k]);
    a.vid = dd.idVendor;
    a.pid = dd.idProduct;
    const int speed = libusb_get_device_speed(list[i]);
    a.superspeed = speed >= LIBUSB_SPEED_SUPER;
#ifdef __APPLE__
    // Apple silicon USB4 / Thunderbolt ports can report a speed libusb does not know ("unknown"), at USB 3 and at
    // USB 2 alike. Shown as USB 3, but still asked to switch: an adapter that came up at USB 2 (480 Mb/s, every
    // power-up) never streams, and the switch reads the chip's own mode first (0xff = 0x20: already USB 3).
    if (speed == LIBUSB_SPEED_UNKNOWN) a.superspeed = true, a.speed_known = false;
#endif
    a.name = dd.idVendor == 0x2357 && dd.idProduct == 0x0115 ? "TP-Link Archer T4U v3" : "RTL8812BU";
    out.push_back(a);
  }
  if (list) libusb_free_device_list(list, 1);
  return out;
}

int request_usb3_all(std::string *log) {
  int asked = 0;
  for (const auto &a : list_adapters()) {
    if (a.superspeed && a.speed_known) continue;
    std::string detail;
    const auto r = wsdr::request_usb3(wsdr::kPid8812bu, a.bus, a.port, &detail);
    if (log) *log += a.id() + ": usb3 switch " + wsdr::usb3_switch_name(r) + (detail.empty() ? "" : " - " + detail) + "\n";
    if (r == wsdr::Usb3Switch::Requested) asked++;
  }
  return asked;
}

}  // namespace arx
