// analogrx_sim: virtual adapters and transmitters (src/sim/sim_receiver.cpp).
#pragma once
#include <string>
#include <vector>

#include "adapters.h"

namespace arx::sim {
// `adapters` virtual 8812BUs; every recording (.cs16 + .json with centre_hz) is a transmitter all of them hear.
void configure(int adapters, const std::vector<std::string> &recordings);
std::vector<AdapterInfo> list_adapters();
// What path B hears. false (default): a gate antenna (the signal ~26 dB down, rising at the gate once a lap).
// true: a second video antenna: A and B each fade on their own (Rayleigh, like a quad flying through
// multipath), with noise, at a mean SNR of snr_db, so two-antenna combining can be tried and measured.
void set_two_antennas(bool on, double snr_db = 12);
}  // namespace arx::sim
