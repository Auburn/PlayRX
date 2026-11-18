// The engine's message loop on macOS / Linux, under the Win32 names main.cpp uses (a message-only window with
// timers on Windows): a thread of its own that runs posted messages, calls from the HTTP threads (waited for,
// with a timeout) and two periodic timers, all through the same window procedure. So the engine code is the
// same on every platform; only main() differs (the Cocoa window must own the main thread, so the engine gets
// this thread instead of sharing the UI thread as on Windows).
#pragma once
#ifndef _WIN32
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using UINT = unsigned;
using WPARAM = uintptr_t;
using LPARAM = intptr_t;
using LRESULT = intptr_t;
using UINT_PTR = uintptr_t;
using DWORD = unsigned;
using DWORD_PTR = uintptr_t;
#define CALLBACK
constexpr UINT WM_TIMER = 0x0113;
constexpr UINT WM_APP = 0x8000;
constexpr UINT SMTO_BLOCK = 1, SMTO_ABORTIFHUNG = 2;

class EngineLoop;
using HWND = EngineLoop *;
using WNDPROC = LRESULT (*)(HWND, UINT, WPARAM, LPARAM);

class EngineLoop {
public:
  explicit EngineLoop(WNDPROC proc) : _proc(proc) {}
  ~EngineLoop() { stop(); }
  void start() { _t = std::thread([this] { run(); }); }
  void stop() {
    {
      std::lock_guard<std::mutex> l(_m);
      _stop = true;
    }
    _cv.notify_all();
    if (_t.joinable() && _t.get_id() != std::this_thread::get_id()) _t.join();
  }
  bool post(UINT msg, WPARAM wp, LPARAM lp) {
    {
      std::lock_guard<std::mutex> l(_m);
      if (_stop) return false;
      _q.push_back({msg, wp, lp, nullptr});
    }
    _cv.notify_all();
    return true;
  }
  // Runs the message on the engine thread and waits for it; false (and it never runs) if it did not start in time.
  bool send(UINT msg, WPARAM wp, LPARAM lp, unsigned timeout_ms) {
    if (std::this_thread::get_id() == _t.get_id()) {  // already on the engine thread
      _proc(this, msg, wp, lp);
      return true;
    }
    auto w = std::make_shared<Wait>();
    {
      std::lock_guard<std::mutex> l(_m);
      if (_stop) return false;
      _q.push_back({msg, wp, lp, w});
    }
    _cv.notify_all();
    std::unique_lock<std::mutex> l(w->m);
    if (!w->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return w->state == 2; })) {
      if (w->state == 0) {  // not started: make sure it never runs (its arguments live on our stack)
        w->state = 3;
        return false;
      }
      w->cv.wait(l, [&] { return w->state == 2; });  // started: it must finish before our stack goes
    }
    return true;
  }
  void set_timer(UINT_PTR id, unsigned ms) {
    {
      std::lock_guard<std::mutex> l(_m);
      _timers[id] = {std::chrono::milliseconds(ms), std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)};
    }
    _cv.notify_all();
  }
  void kill_timer(UINT_PTR id) {
    std::lock_guard<std::mutex> l(_m);
    _timers.erase(id);
  }

private:
  struct Wait {
    std::mutex m;
    std::condition_variable cv;
    int state = 0;  // 0 queued, 1 running, 2 done, 3 abandoned
  };
  struct Msg {
    UINT msg;
    WPARAM wp;
    LPARAM lp;
    std::shared_ptr<Wait> wait;
  };
  struct Timer {
    std::chrono::milliseconds period;
    std::chrono::steady_clock::time_point next;
  };
  void run() {
    std::unique_lock<std::mutex> l(_m);
    while (!_stop) {
      auto next = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      for (auto &t : _timers) next = std::min(next, t.second.next);
      _cv.wait_until(l, next, [&] { return _stop || !_q.empty(); });
      if (_stop) break;
      while (!_q.empty()) {
        Msg m = _q.front();
        _q.pop_front();
        l.unlock();
        if (m.wait) {
          {
            std::lock_guard<std::mutex> g(m.wait->m);
            if (m.wait->state == 3) {  // the caller gave up
              l.lock();
              continue;
            }
            m.wait->state = 1;
          }
          _proc(this, m.msg, m.wp, m.lp);
          {
            std::lock_guard<std::mutex> g(m.wait->m);
            m.wait->state = 2;
          }
          m.wait->cv.notify_all();
        } else {
          _proc(this, m.msg, m.wp, m.lp);
        }
        l.lock();
      }
      const auto now = std::chrono::steady_clock::now();
      std::vector<UINT_PTR> due;
      for (auto &t : _timers)
        if (now >= t.second.next) {
          due.push_back(t.first);
          t.second.next += t.second.period;
          if (t.second.next < now) t.second.next = now + t.second.period;  // fell behind: don't burst
        }
      for (UINT_PTR id : due) {
        l.unlock();
        _proc(this, WM_TIMER, id, 0);
        l.lock();
      }
    }
  }
  WNDPROC _proc;
  std::mutex _m;
  std::condition_variable _cv;
  std::deque<Msg> _q;
  std::map<UINT_PTR, Timer> _timers;
  bool _stop = false;
  std::thread _t;
};

inline bool PostMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return h && h->post(msg, wp, lp); }
inline LRESULT SendMessageTimeoutW(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT, UINT timeout_ms, DWORD_PTR *) {
  return h && h->send(msg, wp, lp, timeout_ms) ? 1 : 0;
}
inline UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT ms, void *) {
  h->set_timer(id, ms);
  return id;
}
inline bool KillTimer(HWND h, UINT_PTR id) {
  h->kill_timer(id);
  return true;
}
inline LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 0; }
#endif
