// A live H.264 encoder for streaming (FPV Trackside / RTMP): one picture in, its NAL units out, no B-frames and
// no lookahead, so each picture comes straight back. Windows: Media Foundation's H.264 encoder (live_encoder_win.cpp);
// macOS: VideoToolbox (live_encoder_mac.mm).
// Linux streams through ffmpeg instead (live_out.cpp) and does not use this class.
// Not thread-safe: one encoder per thread (on Windows that thread must have COM initialised).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace arx {

class LiveEncoder {
public:
  LiveEncoder() = default;
  ~LiveEncoder();
  LiveEncoder(const LiveEncoder &) = delete;
  LiveEncoder &operator=(const LiveEncoder &) = delete;

  // width, height even; bitrate in kbit/s; a key frame every `gop` pictures
  bool open(int width, int height, int fps, int kbps, int gop);
  // One picture (B, G, R, x rows, width * 4 bytes each) at `t_100ns`. `nals`: the NAL units of the access unit that
  // came out (none while the encoder holds it back); `key`: it is a key frame. SPS / PPS are taken out into sps / pps
  // (`config_changed` set when they are new).
  bool encode(const uint8_t *bgrx, int64_t t_100ns, std::vector<std::vector<uint8_t>> &nals, bool &key);
  void close();
  bool is_open() const { return _m != nullptr; }
  const std::string &error() const { return _err; }

  std::vector<uint8_t> sps, pps;
  bool config_changed = false;

private:
  struct Impl;
  Impl *_m = nullptr;
  std::string _err;
  int _w = 0, _h = 0, _fps = 30;
};

}  // namespace arx
