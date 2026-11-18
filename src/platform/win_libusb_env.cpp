// Linked into every program that streams from an adapter on Windows: before libusb opens anything, ask our
// patched libusb (scripts/build-libusb.sh) for WinUSB RAW_IO on the RTL8812BU's bulk-IN endpoint 0x84.
// Without RAW_IO, WinUSB hands queued reads to the host controller one at a time and a 40 MS/s stream
// (~320 MB/s) drops 16 KiB parts. Every read on that pipe is 16 KiB, a multiple of the max packet size, as
// RAW_IO requires. An LIBUSB_WINUSB_RAW_IO already in the environment wins (set it to "" to turn this off).
#ifdef _WIN32
#include <cstdlib>
namespace {
struct LibusbEnv {
  LibusbEnv() {
    if (!std::getenv("LIBUSB_WINUSB_RAW_IO")) _putenv("LIBUSB_WINUSB_RAW_IO=0x84");
  }
} g_libusb_env;
}  // namespace
#endif
