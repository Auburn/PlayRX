// AnalogRX low latency mode (Windows): one pilot's picture in its own Direct3D 11 window, drawn line by line.
//
// The usual path waits for a whole field, converts it, and sends it to the app window as a picture (a frame or
// more of delay before anything shows). Here every line goes into the window's picture the moment the decoder has
// drawn it (kp::LineSink), and the window presents many times per field with tearing allowed: the screen shows the
// newest lines as soon as it scans them out. Full screen (F11 / double-click) lets Windows hand the window straight
// to the display (no compositor frame in between): the lowest latency.
//
// One window, for one slot at a time. line() is called from the decoder threads; everything else from any thread.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "video.h"

namespace arx {

class LowLatWindow {
public:
  LowLatWindow() = default;
  ~LowLatWindow();
  LowLatWindow(const LowLatWindow &) = delete;
  LowLatWindow &operator=(const LowLatWindow &) = delete;

  // Show slot n (opens the window if it is closed). n = 0: close it.
  void show(int n, const std::string &title);
  void set_title(const std::string &title);  // "Pilot 1 · R4": the window's caption
  int slot() const { return _slot.load(std::memory_order_relaxed); }  // 0 = closed (also after the user closes it)
  void line(int n, const kp::Line &l);  // a decoder thread: a line of slot n's picture
  void clear();                         // the picture to black (a new channel, no signal)

  struct Stats {
    bool tearing = false;     // the display takes presents mid-scan (DXGI tearing support)
    bool direct = false;      // full screen: no compositor in the way
    float presents_s = 0;     // presents per second
    float lines_s = 0;        // picture lines per second arriving
    // in the app: from a line's samples arriving over USB to the present that first shows it (the oldest line
    // waiting at each present), over the last second; 0 = no lines. Not counted: USB transfer, the display's scan.
    float age_ms = 0, age_max_ms = 0;
    std::string error;        // the window could not start ("" = fine)
  };
  Stats stats();

private:
  void run();  // the window's thread

  std::atomic<int> _slot{0};
  std::thread _th;
  std::atomic<bool> _quit{false}, _running{false};
  void *_hwnd = nullptr;  // HWND (the window thread's)
  void *_event = nullptr; // HANDLE: lines are waiting

  // each field's newest lines (RGBA, W x active), written by line(); the window thread builds the picture from them
  // (each line in its own place, the line between it and the one above filled along edges: smooth lines)
  std::mutex _m;
  std::vector<uint8_t> _fld[2];
  std::vector<uint32_t> _stamp[2];  // the pass of its field each row came in with
  uint32_t _pass[2] = {1, 1};
  int _lastf = -1, _lastk = 0;
  struct Pend {
    int f, k;
  };
  std::vector<Pend> _pend;  // lines in since the last frame
  int _w = 0, _h = 0;
  bool _resized = false, _clear_req = false;
  uint32_t _since_signal = 0;
  std::string _title, _err;
  bool _title_dirty = false;
  uint64_t _lines = 0;
  double _pend_old = 0;  // arrival of the oldest line not yet presented (0 = none)
  Stats _st;
};

}  // namespace arx
