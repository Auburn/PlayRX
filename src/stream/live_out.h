// Live video out for FPV Trackside (and anything else that takes RTMP): every pilot's picture as its own H.264
// RTMP stream, pushed to rtmp://<host>:<port + slot - 1>/live/pilot<slot>. FPV Trackside's "RTMP Server" video
// sources listen on 19301, 19302, ... in the order they are added, so with the defaults pilot 1 goes to the first
// such source, pilot 2 to the second.
// Like the DVR, live video never waits on it: frame() only copies the newest picture; each slot's own thread scales
// it into the 854 x 480 frame, encodes and sends. Trackside scales that frame into its 4:3 pilot views, so by default
// the 4:3 picture fills the whole frame (Trackside's squeeze brings it back to 4:3); `bars` keeps it 4:3 inside the
// frame with black bars either side instead (for a 16:9 view).
// Windows: Media Foundation's encoder + our RTMP publisher; macOS: VideoToolbox + the same publisher. Linux: ffmpeg
// does both (the Pi's hardware encoder).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace arx {

class LiveOut {
public:
  static constexpr int kSlots = 8;
  static constexpr int kW = 854, kH = 480;  // the frame sent (16:9, Trackside's RTMP size)

  LiveOut();
  ~LiveOut();
  // On / off, where to, frame rate, bitrate (kbit/s). A change reconnects every stream.
  void configure(bool on, const std::string &host, int first_port, int fps, int kbps, bool bars = false);
  void stop();
  // From the decoder threads: slot n's newest picture (B, G, R, x), copied (never waits).
  void frame(int slot, const uint8_t *bgrx, int w, int h);
  // Only slots with a pilot (and video) stream.
  void set_active(int slot, bool active);
  std::string url(int slot);       // where slot n's stream goes
  std::string status_json();       // per slot: state, url, fps sent, error

private:
  struct Out;
  void run(Out &o);
  std::mutex _m;  // the settings below
  bool _on = false;
  std::string _host = "127.0.0.1";
  int _port = 19301, _fps = 30, _kbps = 2500;
  bool _bars = false;
  std::atomic<uint64_t> _gen{0};  // bumped on every change: streams reconnect
  std::unique_ptr<Out> _out[kSlots + 1];
  std::atomic<bool> _stop{false};
};

}  // namespace arx
