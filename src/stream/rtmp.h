// A minimal RTMP publisher: H.264 video to an RTMP server (FPV Trackside's "RTMP Server" video source, OBS's
// media source, nginx-rtmp, ffmpeg -listen). Plain RTMP over TCP: the simple handshake, connect / createStream /
// publish, then FLV video tags (an AVC sequence header from SPS + PPS, then one tag per access unit). Blocking;
// one publisher per thread.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace arx {

class RtmpPublisher {
public:
  ~RtmpPublisher();
  // "rtmp://host[:port]/app/stream". False (error() says why) if the server is not there or refuses.
  bool connect(const std::string &url, int timeout_ms = 3000);
  void close();
  bool connected() const { return _s >= 0; }
  const std::string &error() const { return _err; }

  // onMetaData: width, height, frame rate, H.264
  bool send_metadata(int width, int height, double fps, int kbps);
  // the AVC sequence header (before the first frame and whenever SPS / PPS change)
  bool send_avc_config(const std::vector<uint8_t> &sps, const std::vector<uint8_t> &pps);
  // one access unit: its NAL units (no start codes), at `ms` since the stream began
  bool send_frame(const std::vector<std::vector<uint8_t>> &nals, uint32_t ms, bool keyframe);

private:
  bool send_all(const uint8_t *p, size_t n);
  bool recv_all(uint8_t *p, size_t n, int timeout_ms);
  bool send_message(int csid, uint8_t type, uint32_t stream, uint32_t ts, const std::vector<uint8_t> &payload);
  bool read_message(uint8_t &type, std::vector<uint8_t> &payload, int timeout_ms);  // one whole message
  void drain();  // whatever the server sent meanwhile (acks, status): read and drop, never block
  bool wait_for(const char *what, int timeout_ms);  // until a command / status containing `what` arrives
  intptr_t _s = -1;
  std::string _err, _app, _stream, _tc_url;
  uint32_t _out_chunk = 128, _in_chunk = 128, _stream_id = 1;
  struct InChunk {
    uint32_t ts = 0, len = 0, sid = 0;
    uint8_t type = 0;
    std::vector<uint8_t> buf;
  };
  std::vector<InChunk> _in;  // per chunk stream id
};

// Annex-B H.264 (00 00 01 / 00 00 00 01 start codes) -> NAL units
std::vector<std::vector<uint8_t>> split_annexb(const uint8_t *p, size_t n);

}  // namespace arx
