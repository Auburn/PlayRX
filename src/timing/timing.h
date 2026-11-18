// Each feed's signal: the levels its adapter measures on both antenna paths (the last ~20 s), its receiver
// gain, and the latest spectrum.
//
// Timing is thread-safe: the UI thread feeds it levels from the radios; the web pages read it and change
// gate settings; the UI thread applies settings that touch a radio (gain) when it sees a new version.
#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "feed_server.h"  // kMaxSlots

namespace arx {

double now_s();  // steady clock, seconds (the clock Radio::Level uses)

struct LevelPoint {
  double t;   // steady clock, s
  float a, b; // dBFS, antenna path A / B
};

struct GateConfig {
  int path = 2;           // 0 = A (video antenna), 1 = B (gate antenna), 2 = the stronger of A and B at each moment
  int mode = 1;           // 0 = fixed thresholds (enter_db / exit_db), 1 = relative to the background (rise_db / hyst_db)
  float enter_db = -20;   // fixed: a crossing starts above this ...
  float exit_db = -26;    // ... and ends below this (keep it a few dB under enter)
  float rise_db = 10;     // relative: a crossing starts this far above the background ...
  float hyst_db = 4;      // ... and ends this far below that again
  double min_lap_s = 4;   // passes closer than this to the previous one are ignored
  double smooth_ms = 10;  // smoothing of the level before the thresholds
  // receiver gain: fixed by default. < 0 = the chip's AGC, which flattens gate passes and, at 20 MS/s, makes the
  // 8812BU send short runs of frozen samples (black streaks in the picture)
  double gain_db = 30;
};


class Timing {
public:
  void add_levels(int slot, const std::vector<LevelPoint> &pts);
  void clear(int slot);

  GateConfig config(int slot);
  void set_config(int slot, const GateConfig &c);
  uint64_t config_version(int slot);

  // A fixed gain that would put the strongest signal of the last ~20 s at about -8 dBFS (clear of overload,
  // well above the converter's floor); `current` when there is nothing to go on. Under AGC: 30 dB.
  double suggest_gain(int slot);

  // The latest spectrum of the slot's adapter (dBFS per bin, fftshifted) and the pilot's frequency.
  void set_spectrum(int slot, double centre_hz, double fs, const std::vector<float> &db, double pilot_hz);
  std::string spectrum_json(int slot);

  // JSON for the web pages
  std::string levels_json(int slot, double since_s, int max_points);
  // Feed node: the raw levels newer than since_s (at most the last ~40 s), oldest first
  std::vector<LevelPoint> levels_since(int slot, double since_s);
  std::string config_json(int slot);
  bool config_from_json(int slot, const std::string &body);  // partial updates: only the keys present

private:
  struct Slot {
    std::mutex m;
    GateConfig cfg;
    uint64_t cfg_ver = 0;
    std::deque<LevelPoint> hist;   // the last ~40 s, for the graph (shows up to 30 s)
    double sp_centre = 0, sp_fs = 0, sp_pilot = 0;
    std::vector<float> sp_db;
    uint64_t sp_seq = 0;
  };
  Slot _s[kMaxSlots + 1];
};

}  // namespace arx
