#include "timing.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace arx {

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- Timing
void Timing::add_levels(int slot, const std::vector<LevelPoint> &pts) {
  if (slot < 1 || slot > kMaxSlots || pts.empty()) return;
  Slot &s = _s[slot];
  std::lock_guard<std::mutex> l(s.m);
  for (const auto &p : pts)
    if (s.hist.empty() || p.t > s.hist.back().t) s.hist.push_back(p);
  const double keep = s.hist.back().t - 20;
  while (!s.hist.empty() && s.hist.front().t < keep) s.hist.pop_front();
}

double Timing::suggest_gain(int slot) {
  if (slot < 1 || slot > kMaxSlots) return 30;
  Slot &s = _s[slot];
  std::lock_guard<std::mutex> l(s.m);
  if (s.cfg.gain_db < 0 || s.hist.empty()) return s.cfg.gain_db < 0 ? 30 : s.cfg.gain_db;
  // strongest of both inputs over the last ~20 s (99th percentile: one spike should not decide it)
  std::vector<float> v;
  v.reserve(s.hist.size() * 2);
  for (const auto &p : s.hist) v.push_back(std::max(p.a, p.b));
  const size_t k = v.size() * 99 / 100;
  std::nth_element(v.begin(), v.begin() + k, v.end());
  // nothing much there (quad off or far away): its "strongest" is the noise floor, don't chase that
  if (v[k] < -35) return s.cfg.gain_db;
  const double g = s.cfg.gain_db + (-8.0 - v[k]);
  return std::clamp(std::round(g), 0.0, 45.0);
}


void Timing::clear(int slot) {
  if (slot < 1 || slot > kMaxSlots) return;
  Slot &s = _s[slot];
  std::lock_guard<std::mutex> l(s.m);
  s.hist.clear();
}

GateConfig Timing::config(int slot) {
  if (slot < 1 || slot > kMaxSlots) return {};
  std::lock_guard<std::mutex> l(_s[slot].m);
  return _s[slot].cfg;
}

void Timing::set_config(int slot, const GateConfig &c) {
  if (slot < 1 || slot > kMaxSlots) return;
  Slot &s = _s[slot];
  std::lock_guard<std::mutex> l(s.m);
  s.cfg = c;
  s.cfg_ver++;
}

uint64_t Timing::config_version(int slot) {
  if (slot < 1 || slot > kMaxSlots) return 0;
  std::lock_guard<std::mutex> l(_s[slot].m);
  return _s[slot].cfg_ver;
}

std::string Timing::levels_json(int slot, double since_s, int max_points) {
  if (slot < 1 || slot > kMaxSlots) return "{}";
  Slot &s = _s[slot];
  std::lock_guard<std::mutex> l(s.m);
  std::ostringstream o, a, b;
  o.setf(std::ios::fixed);
  a.setf(std::ios::fixed);
  b.setf(std::ios::fixed);
  o.precision(3);
  a.precision(1);
  b.precision(1);
  o << "{\"now\":" << now_s() << ",\"t\":[";
  // points newer than since_s, decimated to at most max_points (keep each bucket's max)
  auto it = std::upper_bound(s.hist.begin(), s.hist.end(), since_s, [](double t, const LevelPoint &p) { return t < p.t; });
  const size_t first = static_cast<size_t>(it - s.hist.begin()), n = s.hist.size() - first;
  const size_t step = std::max<size_t>(1, (n + max_points - 1) / std::max(1, max_points));
  for (size_t i = first; i < s.hist.size(); i += step) {
    float ma = -200, mb = -200;
    const size_t end = std::min(s.hist.size(), i + step);
    for (size_t k = i; k < end; k++) ma = std::max(ma, s.hist[k].a), mb = std::max(mb, s.hist[k].b);
    const char *c = i == first ? "" : ",";
    o << c << s.hist[end - 1].t;
    a << c << ma;
    b << c << mb;
  }
  o << "],\"a\":[" << a.str() << "],\"b\":[" << b.str() << "]}";
  return o.str();
}

void Timing::set_spectrum(int slot, double centre_hz, double fs, const std::vector<float> &db, double pilot_hz) {
  if (slot < 1 || slot > kMaxSlots) return;
  Slot &s = _s[slot];
  std::lock_guard<std::mutex> l(s.m);
  s.sp_centre = centre_hz;
  s.sp_fs = fs;
  s.sp_pilot = pilot_hz;
  s.sp_db = db;
  s.sp_seq++;
}

std::string Timing::spectrum_json(int slot) {
  if (slot < 1 || slot > kMaxSlots) return "{}";
  Slot &s = _s[slot];
  std::lock_guard<std::mutex> l(s.m);
  std::ostringstream o;
  o.setf(std::ios::fixed);
  o.precision(3);
  o << "{\"seq\":" << s.sp_seq << ",\"centre_mhz\":" << s.sp_centre / 1e6 << ",\"fs_mhz\":" << s.sp_fs / 1e6
    << ",\"pilot_mhz\":" << s.sp_pilot / 1e6 << ",\"db\":[";
  o.precision(1);
  for (size_t i = 0; i < s.sp_db.size(); i++) o << (i ? "," : "") << s.sp_db[i];
  o << "]}";
  return o.str();
}

std::string Timing::config_json(int slot) {
  const GateConfig c = config(slot);
  char b[320];
  std::snprintf(b, sizeof b,
                "{\"path\":%d,\"mode\":%d,\"enter_db\":%.1f,\"exit_db\":%.1f,\"rise_db\":%.1f,\"hyst_db\":%.1f,"
                "\"min_lap_s\":%.2f,\"smooth_ms\":%.1f,\"gain_db\":%.1f,\"gain_suggest\":%.0f}",
                c.path, c.mode, c.enter_db, c.exit_db, c.rise_db, c.hyst_db, c.min_lap_s, c.smooth_ms, c.gain_db,
                suggest_gain(slot));
  return b;
}

bool Timing::config_from_json(int slot, const std::string &body) {
  if (slot < 1 || slot > kMaxSlots) return false;
  GateConfig c = config(slot);
  auto num = [&](const char *key, double &out) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = body.find(k);
    if (p == std::string::npos) return;
    p = body.find(':', p);
    if (p == std::string::npos) return;
    char *end = nullptr;
    const double v = std::strtod(body.c_str() + p + 1, &end);
    if (end != body.c_str() + p + 1 && std::isfinite(v)) out = v;
  };
  double path = c.path, mode = c.mode, en = c.enter_db, ex = c.exit_db, ri = c.rise_db, hy = c.hyst_db,
         ml = c.min_lap_s, sm = c.smooth_ms, g = c.gain_db;
  num("path", path);
  num("mode", mode);
  num("enter_db", en);
  num("exit_db", ex);
  num("rise_db", ri);
  num("hyst_db", hy);
  num("min_lap_s", ml);
  num("smooth_ms", sm);
  num("gain_db", g);
  c.path = std::clamp(static_cast<int>(std::lround(path)), 0, 2);
  c.mode = mode >= 0.5 ? 1 : 0;
  c.enter_db = static_cast<float>(std::clamp(en, -120.0, 10.0));
  c.exit_db = static_cast<float>(std::clamp(ex, -120.0, static_cast<double>(c.enter_db)));
  c.rise_db = static_cast<float>(std::clamp(ri, 2.0, 60.0));
  c.hyst_db = static_cast<float>(std::clamp(hy, 1.0, static_cast<double>(c.rise_db)));
  c.min_lap_s = std::clamp(ml, 0.5, 600.0);
  c.smooth_ms = std::clamp(sm, 0.0, 500.0);
  c.gain_db = g < 0 ? -1 : std::clamp(g, 0.0, 60.0);
  set_config(slot, c);
  return true;
}


}  // namespace arx

namespace arx {
std::vector<LevelPoint> Timing::levels_since(int slot, double since_s) {
  std::vector<LevelPoint> v;
  if (slot < 1 || slot > kMaxSlots) return v;
  Slot &s = _s[slot];
  std::lock_guard<std::mutex> l(s.m);
  auto it = std::upper_bound(s.hist.begin(), s.hist.end(), since_s, [](double t, const LevelPoint &p) { return t < p.t; });
  v.assign(it, s.hist.end());
  return v;
}
}  // namespace arx
