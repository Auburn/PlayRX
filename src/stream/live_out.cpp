// Live video out (FPV Trackside / RTMP): see live_out.h.
#include "live_out.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>

#include "log.h"
#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include "live_encoder.h"
#include "rtmp.h"
#elif defined(__APPLE__)
#include "live_encoder.h"  // VideoToolbox (live_encoder_mac.mm)
#include "rtmp.h"
#else
#include "child_process.h"
#endif

namespace arx {
namespace {
using clk = std::chrono::steady_clock;

std::string jq(const std::string &s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o + "\"";
}

// The picture (any size: 720 x 480 NTSC, 720 x 576 PAL) scaled into the kW x kH frame (bilinear): filling it, or
// (bars) kept 4:3 in the middle with black bars either side
void compose(const uint8_t *src, int sw, int sh, std::vector<uint8_t> &dst, bool bars) {
  constexpr int W = LiveOut::kW, H = LiveOut::kH;
  const int PW = bars ? H * 4 / 3 : W, X0 = (W - PW) / 2;
  dst.assign(static_cast<size_t>(W) * H * 4, 0);
  if (!src || sw < 2 || sh < 2) return;
  for (int y = 0; y < H; y++) {
    const float fy = (y + 0.5f) * sh / H - 0.5f;
    const int y0 = std::clamp(static_cast<int>(fy), 0, sh - 2);
    const float wy = std::clamp(fy - y0, 0.f, 1.f);
    const uint8_t *r0 = src + static_cast<size_t>(y0) * sw * 4, *r1 = r0 + static_cast<size_t>(sw) * 4;
    uint8_t *d = dst.data() + (static_cast<size_t>(y) * W + X0) * 4;
    for (int x = 0; x < PW; x++, d += 4) {
      const float fx = (x + 0.5f) * sw / PW - 0.5f;
      const int x0 = std::clamp(static_cast<int>(fx), 0, sw - 2);
      const float wx = std::clamp(fx - x0, 0.f, 1.f);
      const uint8_t *a = r0 + x0 * 4, *b = r1 + x0 * 4;
      for (int c = 0; c < 3; c++) {
        const float top = a[c] + (a[c + 4] - a[c]) * wx, bot = b[c] + (b[c + 4] - b[c]) * wx;
        d[c] = static_cast<uint8_t>(top + (bot - top) * wy + 0.5f);
      }
    }
  }
}
}  // namespace

struct LiveOut::Out {
  int slot = 0;
  std::mutex m;
  std::condition_variable cv;
  std::vector<uint8_t> back;  // the newest picture from the decoder
  int w = 0, h = 0;
  bool fresh = false, active = false;
  // status (under m)
  std::string state = "off", error;
  double fps_out = 0;
  std::thread t;
};

LiveOut::LiveOut() {
  for (int i = 1; i <= kSlots; i++) {
    _out[i] = std::make_unique<Out>();
    _out[i]->slot = i;
    _out[i]->t = std::thread([this, i] { run(*_out[i]); });
  }
}

LiveOut::~LiveOut() { stop(); }

void LiveOut::stop() {
  if (_stop.exchange(true)) return;
  for (int i = 1; i <= kSlots; i++) _out[i]->cv.notify_all();
  for (int i = 1; i <= kSlots; i++)
    if (_out[i]->t.joinable()) _out[i]->t.join();
}

void LiveOut::configure(bool on, const std::string &host, int first_port, int fps, int kbps, bool bars) {
  {
    std::lock_guard<std::mutex> l(_m);
    const int f = fps >= 50 ? 60 : 30, k = std::clamp(kbps, 300, 20000), p = std::clamp(first_port, 1, 65535 - kSlots);
    const std::string h = host.empty() ? "127.0.0.1" : host;
    if (on == _on && h == _host && p == _port && f == _fps && k == _kbps && bars == _bars) return;
    _on = on, _host = h, _port = p, _fps = f, _kbps = k, _bars = bars;
  }
  _gen++;
  for (int i = 1; i <= kSlots; i++) _out[i]->cv.notify_all();
}

std::string LiveOut::url(int slot) {
  std::lock_guard<std::mutex> l(_m);
  return "rtmp://" + _host + ":" + std::to_string(_port + slot - 1) + "/live/pilot" + std::to_string(slot);
}

void LiveOut::frame(int slot, const uint8_t *bgrx, int w, int h) {
  if (slot < 1 || slot > kSlots || _stop) return;
  Out &o = *_out[slot];
  std::unique_lock<std::mutex> l(o.m, std::try_to_lock);
  if (!l.owns_lock() || !o.active) return;  // busy taking the last one: skip this picture, never wait
  o.back.assign(bgrx, bgrx + static_cast<size_t>(w) * h * 4);
  o.w = w, o.h = h;
  o.fresh = true;
}

void LiveOut::set_active(int slot, bool active) {
  if (slot < 1 || slot > kSlots) return;
  Out &o = *_out[slot];
  {
    std::lock_guard<std::mutex> l(o.m);
    if (o.active == active) return;
    o.active = active;
  }
  o.cv.notify_all();
}

std::string LiveOut::status_json() {
  bool on;
  {
    std::lock_guard<std::mutex> l(_m);
    on = _on;
  }
  std::ostringstream s;
  s << "{\"on\":" << (on ? "true" : "false")
    << ",\"supported\":true"
    << ",\"slots\":[";
  for (int i = 1; i <= kSlots; i++) {
    Out &o = *_out[i];
    std::lock_guard<std::mutex> l(o.m);
    char f[16];
    std::snprintf(f, sizeof f, "%.0f", o.fps_out);
    s << (i > 1 ? "," : "") << "{\"slot\":" << i << ",\"state\":" << jq(o.state) << ",\"url\":" << jq(url(i))
      << ",\"fps\":" << f << ",\"error\":" << jq(o.error) << "}";
  }
  s << "]}";
  return s.str();
}

void LiveOut::run(Out &o) {
#ifdef _WIN32
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
  auto set_state = [&](const char *st, const std::string &err = {}) {
    std::lock_guard<std::mutex> l(o.m);
    o.state = st;
    o.error = err;
    if (std::strcmp(st, "live") != 0) o.fps_out = 0;
  };
  // sleep up to `ms`, waking early on stop or a settings change
  auto nap = [&](int ms, uint64_t gen) {
    std::unique_lock<std::mutex> l(o.m);
    o.cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return _stop.load() || _gen != gen; });
  };
  std::vector<uint8_t> pic, canvas;
  int pw = 0, ph = 0;
  while (!_stop) {
    bool on, active, bars;
    std::string host;
    int fps, kbps;
    const uint64_t gen = _gen;
    {
      std::lock_guard<std::mutex> l(_m);
      on = _on, host = _host, fps = _fps, kbps = _kbps, bars = _bars;
    }
    {
      std::lock_guard<std::mutex> l(o.m);
      active = o.active;
    }
    if (!on || !active) {
      set_state(on ? "idle" : "off");
      nap(1000, gen);
      continue;
    }
    const std::string url = this->url(o.slot);
    set_state("connecting");
    // the newest picture (or black until there is one), composed into the frame that is sent
    auto take = [&]() {
      std::lock_guard<std::mutex> l(o.m);
      if (o.fresh) {
        pic.swap(o.back);
        pw = o.w, ph = o.h;
        o.fresh = false;
        return true;
      }
      return false;
    };
    const auto tick = std::chrono::nanoseconds(1000000000LL / fps);
#if defined(_WIN32) || defined(__APPLE__)  // our own encoder (Media Foundation / VideoToolbox) and RTMP sender
    RtmpPublisher rtmp;
    if (!rtmp.connect(url)) {
      set_state("waiting", rtmp.error());
      nap(3000, gen);
      continue;
    }
    LiveEncoder enc;
    if (!enc.open(kW, kH, fps, kbps, fps)) {
      KP_LOGW("trackside: pilot %d: %s", o.slot, enc.error().c_str());
      set_state("error", enc.error());
      nap(5000, gen);
      continue;
    }
    rtmp.send_metadata(kW, kH, fps, kbps);
    KP_LOGI("trackside: pilot %d streaming to %s", o.slot, url.c_str());
    set_state("live");
    const auto t0 = clk::now();
    auto next = t0;
    uint64_t sent = 0;
    auto fps_t = t0;
    uint64_t fps_n = 0;
    bool have_config = false;
    std::vector<std::vector<uint8_t>> nals;
    while (!_stop && _gen == gen) {
      {
        std::lock_guard<std::mutex> l(o.m);
        if (!o.active) break;
      }
      std::this_thread::sleep_until(next);
      next += tick;
      if (clk::now() > next + tick * 4) next = clk::now() + tick;  // fell behind: don't burst
      if (take() || canvas.empty()) compose(pic.empty() ? nullptr : pic.data(), pw, ph, canvas, bars);
      const int64_t t100 = std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count() / 100;
      bool key = false;
      if (!enc.encode(canvas.data(), t100, nals, key)) {
        set_state("error", enc.error());
        break;
      }
      if (enc.config_changed || !have_config) {
        if (enc.sps.empty() || enc.pps.empty()) continue;  // not yet
        if (!rtmp.send_avc_config(enc.sps, enc.pps)) break;
        have_config = true;
      }
      if (nals.empty()) continue;
      if (!rtmp.send_frame(nals, static_cast<uint32_t>(t100 / 10000), key)) break;
      sent++, fps_n++;
      const double el = std::chrono::duration<double>(clk::now() - fps_t).count();
      if (el >= 1) {
        std::lock_guard<std::mutex> l(o.m);
        o.fps_out = fps_n / el;
        fps_t = clk::now(), fps_n = 0;
      }
    }
    const std::string why = rtmp.error();
    rtmp.close();
    enc.close();
    if (!_stop && _gen == gen) {
      std::lock_guard<std::mutex> l(o.m);
      if (o.active) {
        KP_LOGW("trackside: pilot %d stream stopped (%s): reconnecting", o.slot, why.c_str());
        o.state = "waiting", o.error = why.empty() ? "the stream stopped" : why;
        o.fps_out = 0;
      }
    }
    if (!_stop && _gen == gen) nap(2000, gen);
    (void)sent;
#else  // Linux: ffmpeg encodes (the Pi 4's hardware encoder when there is one) and sends
    static const bool hw = std::filesystem::exists("/dev/video11") &&
                           run_capture({"ffmpeg", "-hide_banner", "-encoders"}).find("h264_v4l2m2m") != std::string::npos;
    std::vector<std::string> a = {"ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgr0", "-s",
                                  std::to_string(kW) + "x" + std::to_string(kH), "-framerate", std::to_string(fps), "-i", "pipe:0",
                                  "-vf", "format=yuv420p", "-c:v", hw ? "h264_v4l2m2m" : "libx264", "-b:v", std::to_string(kbps) + "k",
                                  "-g", std::to_string(fps)};
    if (!hw) a.insert(a.end(), {"-preset", "ultrafast", "-tune", "zerolatency", "-bf", "0"});
    a.insert(a.end(), {"-f", "flv", url});
    Child c = spawn(a, true);
    if (c.pid <= 0) {
      set_state("error", "ffmpeg is not installed (sudo apt install ffmpeg)");
      nap(5000, gen);
      continue;
    }
    set_state("live");
    auto next = clk::now(), fps_t = next;
    uint64_t fps_n = 0;
    bool ok = true;
    while (!_stop && _gen == gen && ok) {
      {
        std::lock_guard<std::mutex> l(o.m);
        if (!o.active) break;
      }
      std::this_thread::sleep_until(next);
      next += tick;
      if (clk::now() > next + tick * 4) next = clk::now() + tick;
      if (take() || canvas.empty()) compose(pic.empty() ? nullptr : pic.data(), pw, ph, canvas, bars);
      const uint8_t *p = canvas.data();
      size_t n = canvas.size();
      while (n > 0) {
        const ssize_t k = ::write(c.stdin_fd, p, n);
        if (k <= 0) {
          ok = false;  // ffmpeg ended: nothing listening, or the receiver went away
          break;
        }
        p += k, n -= static_cast<size_t>(k);
      }
      fps_n++;
      const double el = std::chrono::duration<double>(clk::now() - fps_t).count();
      if (el >= 1) {
        std::lock_guard<std::mutex> l(o.m);
        o.fps_out = fps_n / el;
        fps_t = clk::now(), fps_n = 0;
      }
    }
    ::close(c.stdin_fd);
    reap(c.pid, 2000);
    if (!_stop && _gen == gen) {
      set_state("waiting", "nothing listening at " + url + ", or the receiver stopped");
      nap(3000, gen);
    }
#endif
  }
#ifdef _WIN32
  CoUninitialize();
#endif
}

}  // namespace arx
