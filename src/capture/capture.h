// Video from a capture device: a USB video receiver (5.8 GHz receiver with a USB plug), an HDMI/AV capture
// card, anything the operating system lists as a camera. A pilot's slot can take its pictures from one instead
// of from an adapter (the adapter then only times laps, if one is free). Windows: Media Foundation. Elsewhere
// the list is empty for now.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace arx {

struct CaptureDevice {
  std::string id;    // the device's symbolic link (stable while it stays in the same USB port)
  std::string name;  // what Windows calls it
};

// The devices plugged in now (a few hundred ms: call off the UI's critical path). Our own virtual cameras
// ("Play League Pilot N") are left out.
std::vector<CaptureDevice> list_capture_devices();

class Capture {
 public:
  using Sink = std::function<void(const uint8_t *bgrx, int w, int h)>;
  ~Capture() { stop(); }
  // Opens device `id` on a thread of its own and hands every picture (BGRX, top row first) to `sink`.
  void start(const std::string &id, Sink sink);
  void stop();
  const std::string &id() const { return _id; }
  bool running() const { return _running.load(); }  // false: never started, or the device failed / went away
  int64_t last_frame_ms() const { return _last_ms.load(); }  // steady clock ms of the newest picture (0 none)
  std::string error() const;
  std::string format() const;  // "720x480 @ 30 fps" once open

 private:
  void run(std::string id, Sink sink, int gen);
  // each start() is a generation; a reader thread stuck in the driver after stop() is left behind (detached)
  // and touches nothing once its generation is over. Capture objects live as long as the program.
  std::string _id;
  std::atomic<int> _gen{0};
  std::atomic<bool> _running{false};
  std::atomic<int64_t> _last_ms{0};
  mutable std::mutex _m;
  std::string _error, _format;
};

}  // namespace arx
