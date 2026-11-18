// Echo cancelling (CMA equaliser) for analog FM video: see equalize.h.
#include "equalize.h"

#include <algorithm>
#include <cmath>

namespace arx {

void Equalizer::reset() {
  const float mu = _mu;
  *this = Equalizer{};
  _mu = mu;
}

void Equalizer::process(cf *x, size_t n) {
  if (!_init) {
    std::fill(_w.begin(), _w.end(), cf{});
    _w[kTaps / 2] = 1;
    _init = true;
  }
  // DC (the adapter's DC line) and power, from this block, smoothed across blocks
  if (n == 0) return;
  double sr = 0, si = 0, p = 0;
  for (size_t i = 0; i < n; i++) sr += x[i].real(), si += x[i].imag();
  const cf mean(static_cast<float>(sr / n), static_cast<float>(si / n));
  _dc += (mean - _dc) * 0.3f;
  for (size_t i = 0; i < n; i++) p += std::norm(x[i] - _dc);
  const float pw = static_cast<float>(p / n);
  _pow = _pow <= 0 ? pw : _pow + (pw - _pow) * 0.3f;
  const float g = _pow > 0 ? 1.f / std::sqrt(_pow) : 1.f;  // input to unit power: target modulus 1
  const float back = 1.f / g;
  cf *w = _w.data(), *h = _hist.data();
  for (size_t i = 0; i < n; i++) {
    const cf in = (x[i] - _dc) * g;
    h[_pos] = in;
    // y = sum w_k * x[n - k]  (history is a ring: index _pos is the newest)
    cf y{};
    for (int k = 0, j = _pos; k < kTaps; k++, j = j == 0 ? kTaps - 1 : j - 1) y += w[k] * h[j];
    const float m = std::norm(y);
    // CMA (2,2): e = y (|y|^2 - 1); w -= mu e conj(x)
    const cf e = y * (m - 1.f) * _mu;
    for (int k = 0, j = _pos; k < kTaps; k++, j = j == 0 ? kTaps - 1 : j - 1) w[k] -= e * std::conj(h[j]);
    _pos = _pos + 1 == kTaps ? 0 : _pos + 1;
    // stats: envelope ripple in and out
    const float ai = std::abs(in), ao = std::sqrt(m);
    _ein += ai, _ein2 += ai * ai, _eout += ao, _eout2 += ao * ao;
    x[i] = y * back;
  }
  _ns += n;
  // keep the filter from drifting off: renormalise its gain, and reset it if it has gone unstable
  double s = 0;
  for (int k = 0; k < kTaps; k++) s += std::norm(w[k]);
  if (!(s > 1e-6 && s < 1e3)) {
    std::fill(_w.begin(), _w.end(), cf{});
    _w[kTaps / 2] = 1;
  }
}

Equalizer::Stats Equalizer::take_stats() {
  Stats st;
  if (_ns) {
    auto rip = [&](double s1, double s2) {
      const double m = s1 / _ns, v = std::max(0.0, s2 / _ns - m * m);
      return static_cast<float>(m > 0 ? std::sqrt(v) / m : 0);
    };
    st.ripple_in = rip(_ein, _ein2);
    st.ripple_out = rip(_eout, _eout2);
  }
  double tot = 0;
  for (const auto &t : _w) tot += std::abs(t);
  st.main_tap = tot > 0 ? static_cast<float>(std::abs(_w[kTaps / 2]) / tot) : 1.f;
  _ein = _ein2 = _eout = _eout2 = 0;
  _ns = 0;
  return st;
}

}  // namespace arx
