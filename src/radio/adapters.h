// The RTL8812BU adapters on the USB bus, and getting them ready (USB 3) before they are opened.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace arx {

struct AdapterInfo {
  int bus = -1;
  std::string port;  // port path, "3" or "3.1"
  uint16_t vid = 0, pid = 0;
  bool superspeed = false;
  bool speed_known = true;  // false: libusb could not tell (Apple silicon ports); superspeed is then a guess
  std::string name;  // for the UI: "TP-Link Archer T4U v3" / "RTL8812BU"
  // Stable while it stays in the same socket: "bus-port".
  std::string id() const { return std::to_string(bus) + "-" + port; }
};

// Every 8812BU on the bus (or the simulated ones in analogrx_sim).
std::vector<AdapterInfo> list_adapters();

// Switch each adapter still at USB 2 to USB 3 (they enumerate at USB 2 after a cold plug). Returns how many
// were asked to switch; they re-enumerate over the next ~1-3 s, so list again after a pause.
int request_usb3_all(std::string *log = nullptr);

}  // namespace arx
