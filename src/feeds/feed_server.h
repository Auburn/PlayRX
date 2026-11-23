// AnalogRX feeds: the latest picture of every pilot slot, and a small HTTP server that hands them to OBS.
//
//   /feeds            index: every slot with its OBS URL
//   /pilot/N          a page for an OBS Browser Source: the video, the pilot's name, NO SIGNAL when there is none
//                     (?name=0 hides the name tag)
//   /feed/N.mjpg      the video as MJPEG (multipart/x-mixed-replace), for any player that takes a URL
//   /snapshot/N.jpg   one frame
//   /api/slots        JSON: slot, name, channel, MHz, live, locked
//
// Slots are numbered 1..kMaxSlots and keep their number while the pilot in them changes, so OBS is set up
// once per slot and the race director swaps pilots between heats.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace arx {

constexpr int kMaxSlots = 8;

struct SlotInfo {
  bool used = false;
  std::string name, channel;
  double mhz = 0;
  bool locked = false;
};

class FeedServer {
public:
  FeedServer();
  ~FeedServer();

  // Listen on 127.0.0.1:port (lan = true: every interface, for OBS on another PC). Throws on failure.
  void start(int port, bool lan);
  void stop();
  int port() const { return _port; }
  bool lan() const { return _lan; }
  std::string url(int slot) const;  // http://localhost:PORT/pilot/N

  // A decoded picture for a slot (B, G, R, x per pixel), from any thread. Copies it.
  void push(int slot, const uint8_t *bgrx, int w, int h);
  void set_info(int slot, const SlotInfo &info);
  void clear(int slot);  // the pilot left: no picture, no info

  // Live events for the UI and overlays: /ws/events, one text message (JSON) per event, from any thread.
  // A client that connects gets the events from then on; the last few are kept for slow readers.
  void publish(const std::string &json);

  // Extra routes (the app's API and pages), tried before the built-in ones on the server's threads.
  // Return true when handled; fill status ("200 OK"), content type and body.
  using Handler = std::function<bool(const std::string &method, const std::string &path, const std::string &body,
                                     std::string &status, std::string &type, std::string &out)>;
  void set_handler(Handler h) { _handler = std::move(h); }  // before start()
  // Feed node (src/capture/net_feed.h): GET /node/stream/N streams slot N's pictures, gate levels and status to a
  // race PC. levels(slot, cursor, out) appends the levels newer than cursor (cursor < 0: from now on) as
  // 16-byte points (double t, float a, float b) and moves the cursor; status(slot) is a JSON text. Unset: 404.
  using NodeLevels = std::function<void(int slot, double &cursor, std::vector<uint8_t> &out)>;
  using NodeStatus = std::function<std::string(int slot)>;
  void set_node(NodeLevels levels, NodeStatus status, std::function<double()> now) {
    _node_levels = std::move(levels), _node_status = std::move(status), _node_now = std::move(now);
  }
  int node_clients(int slot) const { return slot >= 1 && slot <= kMaxSlots ? _node_streams[slot].load() : 0; }

private:
  NodeLevels _node_levels;
  NodeStatus _node_status;
  std::function<double()> _node_now;
  std::atomic<int> _node_streams[kMaxSlots + 1] = {};
  void serve_node(uintptr_t sock, int slot);
  Handler _handler;
  struct Slot {
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint8_t> bgrx;
    int w = 0, h = 0;
    uint64_t seq = 0;
    int64_t t_ms = 0;
    std::vector<uint8_t> jpeg;  // of frame `jpeg_seq`, encoded on demand
    uint64_t jpeg_seq = 0;
    SlotInfo info;
  };
  void accept_loop();
  void serve(uintptr_t sock);
  void serve_mjpeg(uintptr_t sock, int slot);
  // /ws/feed?slots=1,2,3: WebSocket, one binary message per new picture: [slot byte][JPEG]. One connection
  // carries any number of pilots (browsers allow only 6 plain HTTP streams per server, WebSockets many more).
  void serve_ws(uintptr_t sock, const std::string &key, const std::vector<int> &slots);
  void serve_events(uintptr_t sock, const std::string &key);
  const std::vector<uint8_t> &blank();  // a black JPEG, encoded on first use (on a server thread: COM)
  // The slot's current frame as JPEG (encoding it once per frame, whoever asks first); false if none.
  bool jpeg_of(int slot, uint64_t &seq, std::vector<uint8_t> &out);
  std::string slots_json();
  std::string index_html();
  std::string pilot_html(int slot, bool show_name);

  Slot _slots[kMaxSlots + 1];
  std::vector<uint8_t> _blank_jpeg;  // sent while a slot has no picture, so players keep the stream open
  std::once_flag _blank_once;
  // "some slot has a new picture" (WebSocket senders wait on it)
  std::mutex _any_m;
  std::condition_variable _any_cv;
  uint64_t _any_seq = 0;
  void notify_any();
  std::mutex _ev_m;
  std::condition_variable _ev_cv;
  std::deque<std::pair<uint64_t, std::string>> _ev;  // (number, message), the last 64
  uint64_t _ev_seq = 0;
  int _port = 0;
  bool _lan = false;
  uintptr_t _listen = ~uintptr_t(0);
  std::atomic<bool> _stop{false};
  std::thread _acceptor;
  std::mutex _cm;
  std::vector<std::thread> _threads;
  std::vector<uintptr_t> _socks;
};

// JPEG-encode B, G, R, x pixels with Windows' own codec (WIC). quality 0..1.
bool encode_jpeg(const uint8_t *bgrx, int w, int h, float quality, std::vector<uint8_t> &out);

}  // namespace arx
