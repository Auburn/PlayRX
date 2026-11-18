// macOS / Linux: no capture devices yet (see capture.h).
#include "capture.h"

namespace arx {

std::vector<CaptureDevice> list_capture_devices() { return {}; }
void Capture::run(std::string, Sink, int) {}
void Capture::start(const std::string &id, Sink) {
  _id = id;
  std::lock_guard<std::mutex> l(_m);
  _error = "USB video devices are only supported on Windows so far";
}
void Capture::stop() { _id.clear(); }
std::string Capture::error() const {
  std::lock_guard<std::mutex> l(_m);
  return _error;
}
std::string Capture::format() const { return ""; }

}  // namespace arx
