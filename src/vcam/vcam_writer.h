// The app's side of the virtual cameras (Windows): every pilot's newest picture into its shared memory block
// (vcam_shared.h), and the cameras the user switched on registered for this user so other programs list them
// ("Play RX Pilot 1" ...). frame() is a copy and never waits (the decoder threads call it).
#pragma once
#include <cstdint>
#include <string>

namespace arx {

class VCamWriter {
public:
  ~VCamWriter();
  // Opens the shared memory for every slot (no camera is listed yet: see cameras()).
  void start(const std::string &product);
  void stop();
  // Which cameras other programs list: bit n = "<product> Pilot n" (0: none). Registers them with the camera DLL
  // next to the exe and removes the rest. False (error() says why) if the DLL is missing or refused.
  bool cameras(uint32_t mask);
  void frame(int slot, const uint8_t *bgrx, int w, int h);
  void set_slot(int slot, bool active, const std::string &pilot);
  uint32_t mask() const { return _mask; }
  const std::string &error() const { return _err; }

private:
  void *_map[9] = {};
  void *_view[9] = {};
  std::string _product;
  uint32_t _mask = 0;
  bool _applied = false;
  std::string _err;
};

}  // namespace arx
