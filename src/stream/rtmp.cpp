// The RTMP publisher: see rtmp.h.
#include "rtmp.h"

#include "net.h"

#include <chrono>
#include <cstring>
#include <random>

namespace arx {
namespace {

// ---- AMF0 encoding (the few kinds RTMP commands use)
void amf_num(std::vector<uint8_t> &o, double v) {
  o.push_back(0x00);
  uint64_t u;
  std::memcpy(&u, &v, 8);
  for (int i = 7; i >= 0; i--) o.push_back(static_cast<uint8_t>(u >> (8 * i)));
}
void amf_key(std::vector<uint8_t> &o, const std::string &s) {
  o.push_back(static_cast<uint8_t>(s.size() >> 8));
  o.push_back(static_cast<uint8_t>(s.size()));
  o.insert(o.end(), s.begin(), s.end());
}
void amf_str(std::vector<uint8_t> &o, const std::string &s) {
  o.push_back(0x02);
  amf_key(o, s);
}
void amf_null(std::vector<uint8_t> &o) { o.push_back(0x05); }
void amf_end(std::vector<uint8_t> &o) {
  o.push_back(0);
  o.push_back(0);
  o.push_back(0x09);
}
void be32(std::vector<uint8_t> &o, uint32_t v) {
  for (int i = 3; i >= 0; i--) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void be24(std::vector<uint8_t> &o, uint32_t v) {
  for (int i = 2; i >= 0; i--) o.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
double now_ms() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// the AMF0 number right after a string value `name` in a command (e.g. the transaction id after "_result"), and the
// one after that (createStream's stream id comes after a null)
bool amf_find_after(const std::vector<uint8_t> &p, const char *name, double &first, double *stream_id) {
  const size_t n = std::strlen(name);
  for (size_t i = 0; i + 3 + n < p.size(); i++) {
    if (p[i] != 0x02 || p[i + 1] != (n >> 8) || p[i + 2] != (n & 255) || std::memcmp(&p[i + 3], name, n) != 0) continue;
    size_t k = i + 3 + n;
    auto num = [&](double &v) {
      if (k + 9 > p.size() || p[k] != 0x00) return false;
      uint64_t u = 0;
      for (int j = 1; j <= 8; j++) u = (u << 8) | p[k + j];
      std::memcpy(&v, &u, 8);
      k += 9;
      return true;
    };
    if (!num(first)) return false;
    if (stream_id) {
      if (k < p.size() && p[k] == 0x05) k++;  // the null command object
      else if (k < p.size() && p[k] == 0x03) return false;
      return num(*stream_id);
    }
    return true;
  }
  return false;
}
}  // namespace

std::vector<std::vector<uint8_t>> split_annexb(const uint8_t *p, size_t n) {
  std::vector<std::vector<uint8_t>> out;
  size_t i = 0, start = std::string::npos;
  while (i + 3 <= n) {
    if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (i + 4 <= n && p[i + 2] == 0 && p[i + 3] == 1))) {
      const size_t sc = p[i + 2] == 1 ? 3 : 4;
      if (start != std::string::npos) {
        size_t end = i;
        while (end > start && p[end - 1] == 0) end--;  // trailing zero bytes belong to the next start code
        if (end > start) out.emplace_back(p + start, p + end);
      }
      i += sc;
      start = i;
      continue;
    }
    i++;
  }
  if (start != std::string::npos && start < n) out.emplace_back(p + start, p + n);
  return out;
}

RtmpPublisher::~RtmpPublisher() { close(); }

void RtmpPublisher::close() {
  if (_s >= 0) {
    closesocket(static_cast<SOCKET>(_s));
    _s = -1;
  }
  _in.clear();
  _in_chunk = _out_chunk = 128;
}

bool RtmpPublisher::send_all(const uint8_t *p, size_t n) {
  while (n > 0) {
    const int k = ::send(static_cast<SOCKET>(_s), reinterpret_cast<const char *>(p), static_cast<int>(std::min<size_t>(n, 1 << 20)), 0);
    if (k <= 0) {
      _err = "the connection closed";
      return false;
    }
    p += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

bool RtmpPublisher::recv_all(uint8_t *p, size_t n, int timeout_ms) {
  const double until = now_ms() + timeout_ms;
  while (n > 0) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(static_cast<SOCKET>(_s), &r);
    const double left = until - now_ms();
    if (left <= 0) {
      _err = "the server did not answer";
      return false;
    }
    timeval tv{};  // field types differ (Windows long, macOS int microseconds): assign, don't brace-initialise
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(left / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>(static_cast<int64_t>(left) % 1000 * 1000);
    if (select(static_cast<int>(_s) + 1, &r, nullptr, nullptr, &tv) <= 0) continue;
    const int k = ::recv(static_cast<SOCKET>(_s), reinterpret_cast<char *>(p), static_cast<int>(n), 0);
    if (k <= 0) {
      _err = "the server closed the connection";
      return false;
    }
    p += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

bool RtmpPublisher::send_message(int csid, uint8_t type, uint32_t stream, uint32_t ts, const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> out;
  out.reserve(payload.size() + payload.size() / _out_chunk * 5 + 24);
  const bool ext = ts >= 0xFFFFFF;
  out.push_back(static_cast<uint8_t>(csid & 63));  // fmt 0: a full header every message (simple, always valid)
  be24(out, ext ? 0xFFFFFF : ts);
  be24(out, static_cast<uint32_t>(payload.size()));
  out.push_back(type);
  for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(stream >> (8 * i)));  // little-endian
  if (ext) be32(out, ts);
  for (size_t off = 0; off < payload.size(); off += _out_chunk) {
    if (off) {
      out.push_back(static_cast<uint8_t>(0xC0 | (csid & 63)));  // fmt 3: continuation
      if (ext) be32(out, ts);
    }
    const size_t k = std::min<size_t>(_out_chunk, payload.size() - off);
    out.insert(out.end(), payload.begin() + off, payload.begin() + off + k);
  }
  return send_all(out.data(), out.size());
}

bool RtmpPublisher::read_message(uint8_t &type, std::vector<uint8_t> &payload, int timeout_ms) {
  for (;;) {
    uint8_t b0;
    if (!recv_all(&b0, 1, timeout_ms)) return false;
    const int fmt = b0 >> 6;
    uint32_t csid = b0 & 63;
    if (csid == 0 || csid == 1) {
      uint8_t e[2] = {};
      if (!recv_all(e, csid == 0 ? 1 : 2, timeout_ms)) return false;
      csid = 64 + e[0] + (csid == 1 ? e[1] * 256u : 0);
    }
    if (csid >= 65600) {
      _err = "bad chunk";
      return false;
    }
    if (_in.size() <= csid) _in.resize(csid + 1);
    InChunk &c = _in[csid];
    uint8_t h[11];
    const int hl = fmt == 0 ? 11 : fmt == 1 ? 7 : fmt == 2 ? 3 : 0;
    if (hl && !recv_all(h, hl, timeout_ms)) return false;
    uint32_t tsf = hl ? (h[0] << 16 | h[1] << 8 | h[2]) : 0;
    if (fmt <= 1) {
      c.len = h[3] << 16 | h[4] << 8 | h[5];
      c.type = h[6];
    }
    if (fmt == 0) c.sid = h[7] | h[8] << 8 | h[9] << 16 | static_cast<uint32_t>(h[10]) << 24;
    if (hl && tsf == 0xFFFFFF) {
      uint8_t e[4];
      if (!recv_all(e, 4, timeout_ms)) return false;
      tsf = static_cast<uint32_t>(e[0]) << 24 | e[1] << 16 | e[2] << 8 | e[3];
    }
    if (fmt == 0) c.ts = tsf;
    else if (hl) c.ts += tsf;
    if (c.len > (16u << 20)) {
      _err = "message too large";
      return false;
    }
    const size_t want = std::min<size_t>(_in_chunk, c.len - c.buf.size());
    const size_t at = c.buf.size();
    c.buf.resize(at + want);
    if (want && !recv_all(c.buf.data() + at, want, timeout_ms)) return false;
    if (c.buf.size() < c.len) continue;  // more chunks of this message to come
    type = c.type;
    payload.swap(c.buf);
    c.buf.clear();
    if (type == 1 && payload.size() >= 4)  // the server's chunk size from now on
      _in_chunk = (static_cast<uint32_t>(payload[0]) << 24 | payload[1] << 16 | payload[2] << 8 | payload[3]) & 0x7FFFFFFF;
    return true;
  }
}

void RtmpPublisher::drain() {
  for (int i = 0; i < 32 && _s >= 0; i++) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(static_cast<SOCKET>(_s), &r);
    timeval tv{0, 0};
    if (select(static_cast<int>(_s) + 1, &r, nullptr, nullptr, &tv) <= 0) return;
    uint8_t type;
    std::vector<uint8_t> p;
    if (!read_message(type, p, 200)) return;
  }
}

bool RtmpPublisher::wait_for(const char *what, int timeout_ms) {
  const double until = now_ms() + timeout_ms;
  const size_t n = std::strlen(what);
  while (now_ms() < until) {
    uint8_t type;
    std::vector<uint8_t> p;
    if (!read_message(type, p, static_cast<int>(until - now_ms()) + 1)) return false;
    if (type != 20) continue;  // only AMF0 commands carry results and status
    for (size_t i = 0; i + n <= p.size(); i++)
      if (std::memcmp(&p[i], what, n) == 0) {
        if (std::string(what) == "_result") {
          double txn = 0, sid = 0;
          if (amf_find_after(p, "_result", txn, &sid) && txn == 4 && sid >= 1) _stream_id = static_cast<uint32_t>(sid);
        }
        return true;
      }
    // refused: say why
    static const char *bad[] = {"_error", "NetStream.Publish.BadName", "NetConnection.Connect.Rejected"};
    for (const char *e : bad)
      for (size_t i = 0; i + std::strlen(e) <= p.size(); i++)
        if (std::memcmp(&p[i], e, std::strlen(e)) == 0) {
          _err = std::string("the server refused (") + e + ")";
          return false;
        }
  }
  _err = std::string("no answer from the server (waiting for ") + what + ")";
  return false;
}

bool RtmpPublisher::connect(const std::string &url, int timeout_ms) {
  close();
  _err.clear();
  // rtmp://host[:port]/app/stream
  if (url.compare(0, 7, "rtmp://") != 0) {
    _err = "not an rtmp:// address";
    return false;
  }
  const std::string rest = url.substr(7);
  const size_t slash = rest.find('/');
  std::string hostport = rest.substr(0, slash), path = slash == std::string::npos ? "" : rest.substr(slash + 1);
  std::string host = hostport, port = "1935";
  if (const size_t c = hostport.rfind(':'); c != std::string::npos) host = hostport.substr(0, c), port = hostport.substr(c + 1);
  const size_t s2 = path.find('/');
  _app = s2 == std::string::npos ? (path.empty() ? "live" : path) : path.substr(0, s2);
  _stream = s2 == std::string::npos ? "stream" : path.substr(s2 + 1);
  _tc_url = "rtmp://" + hostport + "/" + _app;

  WSADATA wd;
  WSAStartup(MAKEWORD(2, 2), &wd);
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
    _err = "cannot find " + host;
    return false;
  }
  SOCKET s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if (s == INVALID_SOCKET) {
    freeaddrinfo(res);
    _err = "no socket";
    return false;
  }
  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);  // connect with a timeout
  ::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
  freeaddrinfo(res);
  fd_set w;
  FD_ZERO(&w);
  FD_SET(s, &w);
  timeval tv{timeout_ms / 1000, timeout_ms % 1000 * 1000};
  int err = 0;
  socklen_t el = sizeof err;
  if (select(static_cast<int>(s) + 1, nullptr, &w, nullptr, &tv) <= 0 ||
      getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&err), &el) || err) {
    closesocket(s);
    _err = "nothing listening on " + hostport;
    return false;
  }
  nb = 0;
  ioctlsocket(s, FIONBIO, &nb);
  int one = 1;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof one);
#ifdef SO_NOSIGPIPE
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  _s = static_cast<intptr_t>(s);

  // handshake: C0 C1 -> S0 S1 S2 -> C2 (= S1)
  std::vector<uint8_t> c01(1537, 0);
  c01[0] = 3;
  std::mt19937 rng(static_cast<uint32_t>(now_ms()));
  for (size_t i = 9; i < c01.size(); i++) c01[i] = static_cast<uint8_t>(rng());
  std::vector<uint8_t> s012(3073);
  if (!send_all(c01.data(), c01.size()) || !recv_all(s012.data(), s012.size(), timeout_ms) || !send_all(s012.data() + 1, 1536)) {
    if (_err.empty()) _err = "handshake failed";
    close();
    return false;
  }
  // our chunk size: whole video frames in fewer pieces
  std::vector<uint8_t> cs;
  be32(cs, 4096);
  if (!send_message(2, 1, 0, 0, cs)) return close(), false;
  _out_chunk = 4096;
  std::vector<uint8_t> m;
  amf_str(m, "connect");
  amf_num(m, 1);
  m.push_back(0x03);
  amf_key(m, "app"), amf_str(m, _app);
  amf_key(m, "type"), amf_str(m, "nonprivate");
  amf_key(m, "flashVer"), amf_str(m, "FMLE/3.0 (compatible; Play League)");
  amf_key(m, "tcUrl"), amf_str(m, _tc_url);
  amf_end(m);
  if (!send_message(3, 20, 0, 0, m) || !wait_for("_result", timeout_ms)) return close(), false;
  for (const auto &[name, txn] : {std::pair<const char *, int>{"releaseStream", 2}, {"FCPublish", 3}}) {
    m.clear();
    amf_str(m, name), amf_num(m, txn), amf_null(m), amf_str(m, _stream);
    if (!send_message(3, 20, 0, 0, m)) return close(), false;
  }
  m.clear();
  amf_str(m, "createStream"), amf_num(m, 4), amf_null(m);
  if (!send_message(3, 20, 0, 0, m) || !wait_for("_result", timeout_ms)) return close(), false;
  m.clear();
  amf_str(m, "publish"), amf_num(m, 5), amf_null(m), amf_str(m, _stream), amf_str(m, "live");
  if (!send_message(8, 20, _stream_id, 0, m)) return close(), false;
  // most servers confirm the publish; some (ffmpeg -listen) just start reading: don't insist
  const std::string keep = _err;
  wait_for("NetStream.Publish.Start", 1500);
  if (_err.find("refused") != std::string::npos) return close(), false;
  _err = keep;
  return _s >= 0;
}

bool RtmpPublisher::send_metadata(int width, int height, double fps, int kbps) {
  std::vector<uint8_t> m;
  amf_str(m, "@setDataFrame");
  amf_str(m, "onMetaData");
  m.push_back(0x08);  // ECMA array
  be32(m, 5);
  amf_key(m, "width"), amf_num(m, width);
  amf_key(m, "height"), amf_num(m, height);
  amf_key(m, "framerate"), amf_num(m, fps);
  amf_key(m, "videocodecid"), amf_num(m, 7);  // H.264
  amf_key(m, "videodatarate"), amf_num(m, kbps);
  amf_end(m);
  return send_message(4, 18, _stream_id, 0, m);
}

bool RtmpPublisher::send_avc_config(const std::vector<uint8_t> &sps, const std::vector<uint8_t> &pps) {
  if (sps.size() < 4 || pps.empty()) {
    _err = "no SPS / PPS";
    return false;
  }
  std::vector<uint8_t> p = {0x17, 0x00, 0, 0, 0};  // key frame, AVC; sequence header
  p.insert(p.end(), {1, sps[1], sps[2], sps[3], 0xFF, 0xE1});
  p.push_back(static_cast<uint8_t>(sps.size() >> 8)), p.push_back(static_cast<uint8_t>(sps.size()));
  p.insert(p.end(), sps.begin(), sps.end());
  p.push_back(1);
  p.push_back(static_cast<uint8_t>(pps.size() >> 8)), p.push_back(static_cast<uint8_t>(pps.size()));
  p.insert(p.end(), pps.begin(), pps.end());
  return send_message(6, 9, _stream_id, 0, p);
}

bool RtmpPublisher::send_frame(const std::vector<std::vector<uint8_t>> &nals, uint32_t ms, bool keyframe) {
  drain();  // acks and status from the server: never let them pile up
  std::vector<uint8_t> p = {static_cast<uint8_t>(keyframe ? 0x17 : 0x27), 0x01, 0, 0, 0};
  for (const auto &n : nals) {
    be32(p, static_cast<uint32_t>(n.size()));
    p.insert(p.end(), n.begin(), n.end());
  }
  return send_message(6, 9, _stream_id, ms, p);
}

}  // namespace arx
