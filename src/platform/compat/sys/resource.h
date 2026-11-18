// Windows stand-in for the two POSIX calls kepyur's radio uses to raise a thread's priority:
// gettid() + setpriority(PRIO_PROCESS, tid, nice). kepyur only ever boosts the calling thread, so this
// maps a negative nice to a higher Win32 priority for the current thread and ignores the id.
#pragma once
#ifndef _WIN32
#include_next <sys/resource.h>
#else
#include <windows.h>

#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#endif

inline int gettid() { return static_cast<int>(GetCurrentThreadId()); }

inline int setpriority(int /*which*/, int /*who*/, int nice) {
  const int prio = nice <= -10 ? THREAD_PRIORITY_HIGHEST : nice < 0 ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL;
  return SetThreadPriority(GetCurrentThread(), prio) ? 0 : -1;
}
#endif
