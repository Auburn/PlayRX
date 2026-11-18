// analogrx_sim's adapters.cpp: the virtual adapters instead of the USB bus.
#include "adapters.h"
#include "sim.h"

namespace arx {
std::vector<AdapterInfo> list_adapters() { return sim::list_adapters(); }
int request_usb3_all(std::string *) { return 0; }
}  // namespace arx
