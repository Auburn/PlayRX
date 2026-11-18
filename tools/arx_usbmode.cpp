// arx_usbmode: list the RTL8812BU adapters and switch them between USB 2 and USB 3 (what AnalogRX does
// before opening them). The adapter drops off the bus and comes back in the new mode within a few seconds.
//   arx_usbmode            list
//   arx_usbmode --to 3     switch every adapter still at USB 2 to USB 3
//   arx_usbmode --to 2     back to USB 2 (testing)
#include <cstdio>
#include <cstring>
#include <string>

#include "adapters.h"
#include "receiver.h"
#include "usbmode.h"

int main(int argc, char **argv) {
  int to = 0;
  for (int i = 1; i < argc; i++)
    if (!std::strcmp(argv[i], "--to") && i + 1 < argc) to = std::atoi(argv[++i]);
  for (const auto &a : arx::list_adapters()) {
    std::printf("%-8s %04x:%04x  %s  %s\n", a.id().c_str(), a.vid, a.pid, a.superspeed ? "USB 3" : "USB 2", a.name.c_str());
    if ((to == 3 && !a.superspeed) || (to == 2 && a.superspeed)) {
      std::string detail;
      const auto r = to == 3 ? wsdr::request_usb3(wsdr::kPid8812bu, a.bus, a.port, &detail)
                             : wsdr::request_usb2(wsdr::kPid8812bu, a.bus, a.port, &detail);
      std::printf("  -> USB %d: %s%s%s\n", to, wsdr::usb3_switch_name(r), detail.empty() ? "" : " - ", detail.c_str());
    }
  }
  return 0;
}
