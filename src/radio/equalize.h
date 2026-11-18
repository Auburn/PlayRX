// Echo cancelling for analog FM video (a blind adaptive equaliser, CMA). Indoors, the quad's signal arrives
// with echoes off walls a few tens to hundreds of ns late. On FM video that shows as ghost edges and as wavy
// stripes in bright areas (the echo's distortion depends on the instantaneous frequency, i.e. the brightness).
// An FM carrier has a constant envelope, so a short filter that keeps the output's envelope constant (the
// constant-modulus algorithm) learns the inverse of the room's echoes without knowing the picture. kTaps taps
// at the stream rate, centred: adds kTaps / 2 samples of delay (~0.2 us at 20 MS/s).
#pragma once
#include <complex>
#include <cstddef>
#include <vector>

namespace arx {

class Equalizer {
public:
  using cf = std::complex<float>;
  static constexpr int kTaps = 9;

  void reset();
  // In place, any n (state carries over). mu: adaptation step (larger = faster, noisier).
  void process(cf *x, size_t n);
  void set_mu(float mu) { _mu = mu; }

  struct Stats {
    float ripple_in = 0, ripple_out = 0;  // envelope ripple (std / mean of |x|) before / after: echoes show as ripple
    float main_tap = 1;                   // |centre tap| over the sum of |taps|: 1 = no echo being cancelled
  };
  Stats take_stats();

private:
  std::vector<cf> _w = std::vector<cf>(kTaps), _hist = std::vector<cf>(kTaps);
  int _pos = 0;
  bool _init = false;
  cf _dc{};
  float _pow = 0;  // running input power (normalises the input so the target modulus is 1)
  float _mu = 2e-3f;
  // stats
  double _ein = 0, _ein2 = 0, _eout = 0, _eout2 = 0;
  size_t _ns = 0;
};

}  // namespace arx
