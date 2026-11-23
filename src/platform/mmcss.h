// AnalogRX low latency: put the calling thread in Windows' "Pro Audio" multimedia class (MMCSS), which schedules
// it ahead of ordinary threads (MMCSS still leaves the rest of the machine its share). Off: back to where it was.
// No-op elsewhere (macOS / Linux keep their usual boost). Call from the thread itself.
#pragma once
#ifdef _WIN32
#include <windows.h>

namespace arx {

inline void mmcss_thread(bool on) {
  using SetFn = HANDLE(WINAPI *)(LPCWSTR, LPDWORD);
  using RevertFn = BOOL(WINAPI *)(HANDLE);
  static HMODULE avrt = LoadLibraryW(L"avrt.dll");
  static auto set = avrt ? reinterpret_cast<SetFn>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW")) : nullptr;
  static auto revert = avrt ? reinterpret_cast<RevertFn>(GetProcAddress(avrt, "AvRevertMmThreadCharacteristics")) : nullptr;
  thread_local HANDLE h = nullptr;
  if (on && !h && set) {
    DWORD idx = 0;
    h = set(L"Pro Audio", &idx);
  } else if (!on && h && revert) {
    revert(h);
    h = nullptr;
  }
}

}  // namespace arx
#else
namespace arx {
inline void mmcss_thread(bool) {}
}  // namespace arx
#endif
