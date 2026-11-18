// Shared cameras over the network: see net_feed.h.
#include "net_feed.h"

#include "net.h"
#ifdef _WIN32
#include <objbase.h>
#include <wincodec.h>
#else
#include <turbojpeg.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <sstream>
#include <thread>

namespace arx {

namespace {
int64_t steady_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string jesc(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\', o += c;
    else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
    else o += c;
  }
  return o;
}
std::string jstr_(const std::string &j, const char *key) {
  const std::string k = std::string("\"") + key + "\":\"";
  const size_t p = j.find(k);
  if (p == std::string::npos) return {};
  std::string o;
  for (size_t i = p + k.size(); i < j.size() && j[i] != '"'; i++) {
    if (j[i] == '\\' && i + 1 < j.size()) i++;
    o += j[i];
  }
  return o;
}
double jnum_(const std::string &j, const char *key) {
  const std::string k = std::string("\"") + key + "\":";
  const size_t p = j.find(k);
  return p == std::string::npos ? 0 : std::atof(j.c_str() + p + k.size());
}

struct WsaInit {
  WsaInit() {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
  }
};
void wsa() { static WsaInit once; }

void set_nonblock(SOCKET s, bool on) {
  unsigned long v = on ? 1 : 0;
#ifdef _WIN32
  ioctlsocket(s, FIONBIO, &v);
#else
  const int fl = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
  (void)v;
#endif
}

// a TCP connection with a time limit (a node that is off must not hold the caller for 20 s)
SOCKET dial(const std::string &host, int port, int timeout_ms, std::string &err) {
  wsa();
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
    err = "can't find " + host;
    return INVALID_SOCKET;
  }
  SOCKET s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s == INVALID_SOCKET) {
    freeaddrinfo(res);
    err = "no socket";
    return s;
  }
  set_nonblock(s, true);
  ::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
  freeaddrinfo(res);
  fd_set wr, ex;
  FD_ZERO(&wr);
  FD_ZERO(&ex);
  FD_SET(s, &wr);
  FD_SET(s, &ex);
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  int soerr = 0;
  socklen_t sl = sizeof soerr;
  if (select(static_cast<int>(s) + 1, nullptr, &wr, &ex, &tv) <= 0 || FD_ISSET(s, &ex) ||
      getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&soerr), &sl) != 0 || soerr != 0) {
    closesocket(s);
    err = "no answer from " + host + ":" + std::to_string(port);
    return INVALID_SOCKET;
  }
  set_nonblock(s, false);
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof one);
  return s;
}
bool send_all(SOCKET s, const std::string &d) {
  size_t o = 0;
  while (o < d.size()) {
    const int k = ::send(s, d.data() + o, static_cast<int>(d.size() - o), 0);
    if (k <= 0) return false;
    o += static_cast<size_t>(k);
  }
  return true;
}
// wait up to ms for data; -1 error / closed, 0 nothing yet, 1 readable
int readable(SOCKET s, int ms) {
  fd_set rd;
  FD_ZERO(&rd);
  FD_SET(s, &rd);
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  const int r = select(static_cast<int>(s) + 1, &rd, nullptr, nullptr, &tv);
  return r < 0 ? -1 : r;
}

// ---- JPEG -> BGRX
#ifdef _WIN32
bool decode_jpeg(const uint8_t *p, size_t n, std::vector<uint8_t> &out, int &w, int &h) {
  static thread_local IWICImagingFactory *f = nullptr;
  if (!f && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
  IWICStream *st = nullptr;
  IWICBitmapDecoder *dec = nullptr;
  IWICBitmapFrameDecode *fr = nullptr;
  IWICBitmapSource *bgr = nullptr;
  bool ok = false;
  if (SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory(const_cast<BYTE *>(p), static_cast<DWORD>(n))) &&
      SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
      SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGR, fr, &bgr))) {
    UINT uw = 0, uh = 0;
    bgr->GetSize(&uw, &uh);
    w = static_cast<int>(uw), h = static_cast<int>(uh);
    out.resize(static_cast<size_t>(w) * h * 4);
    ok = SUCCEEDED(bgr->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size()), out.data()));
  }
  if (bgr) bgr->Release();
  if (fr) fr->Release();
  if (dec) dec->Release();
  if (st) st->Release();
  return ok && w > 0 && h > 0;
}
#else
bool decode_jpeg(const uint8_t *p, size_t n, std::vector<uint8_t> &out, int &w, int &h) {
  static thread_local tjhandle tj = tjInitDecompress();
  int ss = 0, cs = 0;
  if (!tj || tjDecompressHeader3(tj, p, static_cast<unsigned long>(n), &w, &h, &ss, &cs) != 0 || w <= 0 || h <= 0) return false;
  out.resize(static_cast<size_t>(w) * h * 4);
  return tjDecompress2(tj, p, static_cast<unsigned long>(n), out.data(), w, w * 4, h, TJPF_BGRX, TJFLAG_FASTDCT) == 0;
}
#endif
}  // namespace

int http_request(const std::string &host, int port, const std::string &method, const std::string &path, const std::string &body,
                 std::string &out, int timeout_ms) {
  out.clear();
  std::string err;
  SOCKET s = dial(host, port, timeout_ms, err);
  if (s == INVALID_SOCKET) return 0;
  std::ostringstream r;
  r << method << " " << path << " HTTP/1.1\r\nHost: " << host << ":" << port << "\r\nConnection: close\r\n";
  if (method == "POST") r << "Content-Type: application/json\r\nContent-Length: " << body.size() << "\r\n";
  r << "\r\n" << body;
  if (!send_all(s, r.str())) {
    closesocket(s);
    return 0;
  }
  std::string resp;
  char buf[8192];
  const int64_t until = steady_ms() + timeout_ms;
  while (steady_ms() < until && resp.size() < (4u << 20)) {
    const int rd = readable(s, 200);
    if (rd < 0) break;
    if (rd == 0) continue;
    const int k = ::recv(s, buf, sizeof buf, 0);
    if (k <= 0) break;
    resp.append(buf, static_cast<size_t>(k));
  }
  closesocket(s);
  const size_t he = resp.find("\r\n\r\n");
  if (resp.compare(0, 5, "HTTP/") != 0 || he == std::string::npos) return 0;
  out = resp.substr(he + 4);
  return std::atoi(resp.c_str() + resp.find(' ') + 1);
}

bool parse_net_source(const std::string &id, std::string &host, int &port) {
  if (id.compare(0, 4, "net:") != 0) return false;
  const size_t c = id.rfind(':');
  if (c <= 4) return false;
  host = id.substr(4, c - 4);
  port = std::atoi(id.c_str() + c + 1);
  return !host.empty() && port > 0 && port < 65536;
}

// ---------------------------------------------------------------- NetFeed (race PC)
struct NetFeed::State {
  std::string host, key;
  int port = 0;
  FrameSink frames;
  LevelSink levels;
  std::atomic<bool> stop{false}, release{false}, connected{false};
  std::atomic<int64_t> last_ms{0};
  std::atomic<int> node_slot{0};
  mutable std::mutex m;  // the fields below
  std::string error, status, name, channel;
  double mhz = 0;
  uint64_t pilot_gen = 0;
};

void NetFeed::start(const std::string &id, const std::string &key, const std::string &name, double mhz, const std::string &channel,
                    FrameSink frames, LevelSink levels) {
  stop(false);
  _id = id;
  auto st = std::make_shared<State>();
  if (!parse_net_source(id, st->host, st->port)) {
    st->error = "not a network address";
    _st = st;
    return;
  }
  st->key = key, st->frames = std::move(frames), st->levels = std::move(levels);
  st->name = name, st->mhz = mhz, st->channel = channel;
  _st = st;
  std::thread([st] {
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC (JPEG)
#endif
    auto fail = [&](const std::string &e) {
      std::lock_guard<std::mutex> l(st->m);
      st->error = e;
    };
    std::vector<uint8_t> bgrx;
    while (!st->stop) {
      st->connected = false;
      // 1. ask the node for a slot for this pilot (the same one again after a reconnect)
      std::string name, channel, out;
      double mhz;
      uint64_t gen;
      {
        std::lock_guard<std::mutex> l(st->m);
        name = st->name, channel = st->channel, mhz = st->mhz, gen = st->pilot_gen;
      }
      std::ostringstream b;
      b << "{\"key\":\"" << jesc(st->key) << "\",\"name\":\"" << jesc(name) << "\",\"mhz\":" << mhz << ",\"channel\":\"" << jesc(channel) << "\"}";
      const int code = http_request(st->host, st->port, "POST", "/api/node/feed", b.str(), out);
      const int slot = static_cast<int>(jnum_(out, "slot"));
      if (code != 200 || slot < 1) {
        fail(code == 0 ? "no answer from " + st->host + ":" + std::to_string(st->port) + " (is it on, in feed node mode, with other devices allowed?)"
                       : jstr_(out, "error").empty() ? "the node said " + std::to_string(code) : jstr_(out, "error"));
        for (int i = 0; i < 30 && !st->stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      st->node_slot = slot;
      // 2. its stream
      std::string err;
      SOCKET s = dial(st->host, st->port, 2500, err);
      if (s == INVALID_SOCKET) {
        fail(err);
        continue;
      }
      if (!send_all(s, "GET /node/stream/" + std::to_string(slot) + " HTTP/1.1\r\nHost: " + st->host + "\r\n\r\n")) {
        closesocket(s);
        continue;
      }
      std::string in;
      bool head = true;
      std::deque<std::pair<double, double>> offs;  // (here, here - node) samples, the last 30 s
      int64_t last_rx = steady_ms();
      char buf[65536];
      while (!st->stop) {
        // the pilot changed (name, channel): tell the node (same key: same slot, retuned)
        {
          std::lock_guard<std::mutex> l(st->m);
          if (st->pilot_gen != gen) {
            gen = st->pilot_gen;
            name = st->name, channel = st->channel, mhz = st->mhz;
            std::ostringstream u;
            u << "{\"key\":\"" << jesc(st->key) << "\",\"name\":\"" << jesc(name) << "\",\"mhz\":" << mhz << ",\"channel\":\"" << jesc(channel) << "\"}";
            std::thread([h = st->host, p = st->port, body = u.str()] { std::string o; http_request(h, p, "POST", "/api/node/feed", body, o); }).detach();
          }
        }
        const int rd = readable(s, 200);
        if (rd < 0) break;
        if (rd == 0) {
          if (steady_ms() - last_rx > 4000) { fail("the node stopped sending"); break; }
          continue;
        }
        const int k = ::recv(s, buf, sizeof buf, 0);
        if (k <= 0) { fail("the node closed the connection"); break; }
        last_rx = steady_ms();
        in.append(buf, static_cast<size_t>(k));
        if (head) {
          const size_t he = in.find("\r\n\r\n");
          if (he == std::string::npos) continue;
          if (in.compare(0, 12, "HTTP/1.1 200") != 0) { fail("the node refused the stream"); break; }
          in.erase(0, he + 4);
          head = false;
          st->connected = true;
          fail("");
        }
        // messages: [type][len u32 LE][payload]
        size_t o = 0;
        while (in.size() - o >= 5) {
          const uint8_t type = static_cast<uint8_t>(in[o]);
          uint32_t len;
          std::memcpy(&len, in.data() + o + 1, 4);
          if (len > (8u << 20)) { o = in.size(); break; }
          if (in.size() - o - 5 < len) break;
          const uint8_t *p = reinterpret_cast<const uint8_t *>(in.data() + o + 5);
          if (type == 1) {
            int w = 0, h = 0;
            if (decode_jpeg(p, len, bgrx, w, h)) {
              st->last_ms = steady_ms();
              if (st->frames) st->frames(bgrx.data(), w, h);
            }
          } else if (type == 2 && len >= 8) {
            double node_now;
            std::memcpy(&node_now, p, 8);
            const double here = now_s();
            // the clock offset: the smallest difference seen lately (network delay only ever adds to it)
            offs.push_back({here, here - node_now});
            while (!offs.empty() && here - offs.front().first > 30) offs.pop_front();
            double off = offs.front().second;
            for (const auto &x : offs) off = std::min(off, x.second);
            const size_t n = (len - 8) / 16;
            std::vector<LevelPoint> pts(n);
            for (size_t i = 0; i < n; i++) {
              double t;
              float a, bb;
              std::memcpy(&t, p + 8 + i * 16, 8);
              std::memcpy(&a, p + 16 + i * 16, 4);
              std::memcpy(&bb, p + 20 + i * 16, 4);
              pts[i] = {t + off, a, bb};
            }
            if (n && st->levels) st->levels(pts);
          } else if (type == 3) {
            std::lock_guard<std::mutex> l(st->m);
            st->status.assign(reinterpret_cast<const char *>(p), len);
          }
          o += 5 + len;
        }
        in.erase(0, o);
      }
      closesocket(s);
      st->connected = false;
      if (!st->stop)
        for (int i = 0; i < 10 && !st->stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (st->release) {  // the node may give the slot (and its adapter) to someone else
      std::string o;
      http_request(st->host, st->port, "POST", "/api/node/release", "{\"key\":\"" + jesc(st->key) + "\"}", o, 1500);
    }
#ifdef _WIN32
    CoUninitialize();
#endif
  }).detach();
}

void NetFeed::stop(bool release) {
  if (_st) {
    _st->release = release;
    _st->stop = true;
  }
  _st.reset();
  _id.clear();
}

void NetFeed::set_pilot(const std::string &name, double mhz, const std::string &channel) {
  if (!_st) return;
  std::lock_guard<std::mutex> l(_st->m);
  if (_st->name == name && _st->mhz == mhz && _st->channel == channel) return;
  _st->name = name, _st->mhz = mhz, _st->channel = channel;
  _st->pilot_gen++;
}
bool NetFeed::connected() const { return _st && _st->connected; }
int64_t NetFeed::last_frame_ms() const { return _st ? _st->last_ms.load() : 0; }
int NetFeed::node_slot() const { return _st ? _st->node_slot.load() : 0; }
std::string NetFeed::error() const {
  if (!_st) return {};
  std::lock_guard<std::mutex> l(_st->m);
  return _st->error;
}
std::string NetFeed::node_status() const {
  if (!_st) return {};
  std::lock_guard<std::mutex> l(_st->m);
  return _st->status;
}

// ---------------------------------------------------------------- discovery (UDP broadcast)
namespace {
std::mutex g_bm;
bool g_b_on = false;
int g_b_port = 0;
std::string g_b_name;
bool g_b_thread = false;

std::mutex g_lm;
bool g_l_on = false, g_l_thread = false;
std::vector<NodeInfo> g_heard;
}  // namespace

void node_beacon(bool on, int http_port, const std::string &name) {
  std::lock_guard<std::mutex> l(g_bm);
  g_b_on = on, g_b_port = http_port, g_b_name = name;
  if (!on || g_b_thread) return;
  g_b_thread = true;
  std::thread([] {
    wsa();
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&one), sizeof one);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kBeaconPort);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sockaddr_in lo = to;
    lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // a race PC on this same PC (testing) hears it too
    for (;;) {
      std::string msg;
      {
        std::lock_guard<std::mutex> l(g_bm);
        if (g_b_on) msg = "PLAYNODE 1 " + std::to_string(g_b_port) + " " + g_b_name;
      }
      if (!msg.empty() && s != INVALID_SOCKET) {
        sendto(s, msg.data(), static_cast<int>(msg.size()), 0, reinterpret_cast<sockaddr *>(&to), sizeof to);
        sendto(s, msg.data(), static_cast<int>(msg.size()), 0, reinterpret_cast<sockaddr *>(&lo), sizeof lo);
      }
      std::this_thread::sleep_for(std::chrono::seconds(2));
    }
  }).detach();
}

void node_listen(bool on) {
  std::lock_guard<std::mutex> l(g_lm);
  g_l_on = on;
  if (!on || g_l_thread) return;
  g_l_thread = true;
  std::thread([] {
    wsa();
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kBeaconPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s == INVALID_SOCKET || ::bind(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) return;
    char buf[512];
    for (;;) {
      if (readable(s, 1000) <= 0) continue;
      sockaddr_in from{};
      socklen_t fl = sizeof from;
      const int k = recvfrom(s, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&from), &fl);
      if (k <= 0) continue;
      buf[k] = 0;
      int port = 0;
      char name[256] = {0};
      if (std::sscanf(buf, "PLAYNODE 1 %d %255[^\n]", &port, name) < 1 || port <= 0) continue;
      char ip[64];
      inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
      NodeInfo n;
      n.host = ip, n.port = port, n.name = name[0] ? name : ip, n.seen_ms = steady_ms();
      n.id = "net:" + n.host + ":" + std::to_string(port);
      std::lock_guard<std::mutex> l(g_lm);
      if (!g_l_on) continue;
      auto it = std::find_if(g_heard.begin(), g_heard.end(), [&](const NodeInfo &x) { return x.id == n.id; });
      if (it != g_heard.end()) *it = n;
      else g_heard.push_back(n);
    }
  }).detach();
}

std::vector<NodeInfo> nodes_heard() {
  std::lock_guard<std::mutex> l(g_lm);
  std::vector<NodeInfo> v;
  const int64_t t = steady_ms();
  for (const auto &n : g_heard)
    if (t - n.seen_ms < 10000) v.push_back(n);
  // a node on this same PC is heard twice (its network address and loopback): keep the network one
  std::vector<NodeInfo> out;
  for (const auto &n : v) {
    const bool lo = n.host.compare(0, 4, "127.") == 0;
    if (lo && std::any_of(v.begin(), v.end(), [&](const NodeInfo &o) { return o.port == n.port && o.name == n.name && o.host.compare(0, 4, "127.") != 0; }))
      continue;
    out.push_back(n);
  }
  return out;
}

}  // namespace arx

#ifndef _WIN32
#include <ifaddrs.h>
#endif
namespace arx {
std::vector<std::string> local_ips() {
  std::vector<std::string> v;
  wsa();
#ifdef _WIN32
  char host[256] = {0};
  if (gethostname(host, sizeof host - 1) != 0) return v;
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  if (getaddrinfo(host, nullptr, &hints, &res) != 0) return v;
  for (addrinfo *p = res; p; p = p->ai_next) {
    char ip[64];
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(p->ai_addr)->sin_addr, ip, sizeof ip);
    if (std::strncmp(ip, "127.", 4) != 0 && std::find(v.begin(), v.end(), ip) == v.end()) v.push_back(ip);
  }
  freeaddrinfo(res);
#else
  ifaddrs *ifa = nullptr;
  if (getifaddrs(&ifa) != 0) return v;
  for (ifaddrs *p = ifa; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
    char ip[64];
    inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(p->ifa_addr)->sin_addr, ip, sizeof ip);
    if (std::strncmp(ip, "127.", 4) != 0) v.push_back(ip);
  }
  freeifaddrs(ifa);
#endif
  return v;
}
}  // namespace arx
