// Shared cameras: one PC's adapters feeding another PC's race over the network (USB 3 bandwidth limits how
// many adapters one PC can run; a second PC as a "feed node" adds more).
//
//   Feed node (the PC with the extra adapters): runs Play League / Play RX in feed node mode. Its slots are
//   handed out to race PCs (POST /api/node/feed), and each one streams on GET /node/stream/<slot>: messages
//   [type u8][length u32 little-endian][payload]:
//     1  a picture, JPEG
//     2  gate levels: [double node now][n x (double t, float a, float b)], t on the node's steady clock
//     3  status, JSON text: {"name","channel","mhz","live","state"}
//   and it announces itself on the network (UDP broadcast, port kBeaconPort) every 2 s.
//
//   Race PC: a slot's video source "net:<host>:<port>" (NetFeed) asks the node for a feed for that pilot, takes
//   the pictures into the slot like a local adapter's and the gate levels into its own lap timing, mapped onto
//   its clock (offset = the smallest node->here difference seen lately: on a LAN within about a millisecond).
//   The node is told the pilot's name and channel whenever they change, and lets the slot go when it is closed.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "timing.h"

namespace arx {

constexpr int kBeaconPort = 8091;

// A small blocking HTTP/1.1 request (Connection: close). Returns the status code (0: no connection) and the body.
int http_request(const std::string &host, int port, const std::string &method, const std::string &path, const std::string &body,
                 std::string &out, int timeout_ms = 2500);

// "net:192.168.1.20:8090" -> host, port. False if it is not a network source.
bool parse_net_source(const std::string &id, std::string &host, int &port);

class NetFeed {
 public:
  using FrameSink = std::function<void(const uint8_t *bgrx, int w, int h)>;
  using LevelSink = std::function<void(const std::vector<LevelPoint> &pts)>;  // times on this PC's steady clock
  ~NetFeed() { stop(false); }
  // Feed `key` (this PC + slot, so the node gives the same slot back after a reconnect) from the node at
  // host:port, for the pilot given. Runs on a thread of its own and reconnects by itself.
  void start(const std::string &id, const std::string &key, const std::string &name, double mhz, const std::string &channel, double gain_db,
             FrameSink frames, LevelSink levels);
  // release: tell the node it may give the slot to someone else
  void stop(bool release);
  void set_pilot(const std::string &name, double mhz, const std::string &channel, double gain_db);  // gain_db < 0: the chip's AGC
  const std::string &id() const { return _id; }
  bool connected() const;
  int64_t last_frame_ms() const;  // steady ms of the newest picture (0 none)
  std::string error() const;
  std::string node_status() const;  // the node's last status JSON ("" none)
  int node_slot() const;            // the node's slot feeding us (0 none yet)

 private:
  struct State;
  std::shared_ptr<State> _st;
  std::string _id;
};

// Feed node: announce this PC on the network every 2 s (on / off; name = what race PCs list it as).
void node_beacon(bool on, int http_port, const std::string &name);

// Race PC: the feed nodes heard on the network lately (and the ones added by hand), as {id "net:host:port", name}.
struct NodeInfo {
  std::string id, name, host;
  int port = 0;
  int64_t seen_ms = 0;
};
void node_listen(bool on);
std::vector<NodeInfo> nodes_heard();
// this PC's IPv4 addresses on the network (not loopback)
std::vector<std::string> local_ips();

}  // namespace arx
