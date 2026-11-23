// Starting helper programs on Linux (espeak-ng / aplay for the voice, ffmpeg): posix_spawnp with the
// signal mask cleared (the app blocks SIGINT / SIGTERM for its own shutdown; children must not inherit that),
// optionally its own process group (so a pipeline can be stopped as a whole) and a pipe on its stdin.
#pragma once
#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace arx {

struct Child {
  pid_t pid = -1;
  int stdin_fd = -1;  // the write end of the child's stdin, when asked for
};

// argv[0] is looked up on PATH. stdout / stderr go to /dev/null unless `keep_output`.
inline Child spawn(const std::vector<std::string> &args, bool with_stdin = false, bool new_group = false,
                   bool keep_output = false) {
  Child c;
  std::vector<char *> argv;
  for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);
  int p[2] = {-1, -1};
  if (with_stdin && pipe(p) != 0) return c;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  if (with_stdin) {
    posix_spawn_file_actions_adddup2(&fa, p[0], 0);
    posix_spawn_file_actions_addclose(&fa, p[0]);
    posix_spawn_file_actions_addclose(&fa, p[1]);
  } else {
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  }
  if (!keep_output) {
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  }
  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  sigset_t none, def;
  sigemptyset(&none);
  sigemptyset(&def);
  sigaddset(&def, SIGPIPE);
  sigaddset(&def, SIGINT);
  sigaddset(&def, SIGTERM);
  posix_spawnattr_setsigmask(&at, &none);
  posix_spawnattr_setsigdefault(&at, &def);
  short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
  if (new_group) {
    posix_spawnattr_setpgroup(&at, 0);
    flags |= POSIX_SPAWN_SETPGROUP;
  }
  posix_spawnattr_setflags(&at, flags);
  pid_t pid = -1;
  const int rc = posix_spawnp(&pid, argv[0], &fa, &at, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  if (with_stdin) close(p[0]);
  if (rc != 0) {
    if (with_stdin) close(p[1]);
    return c;
  }
  c.pid = pid;
  if (with_stdin) {
    fcntl(p[1], F_SETFD, FD_CLOEXEC);  // later children must not hold it open
    c.stdin_fd = p[1];
  }
  return c;
}

// Waits for the child up to `timeout_ms`, then kills it (its group if it has one). True if it ended by itself.
inline bool reap(pid_t pid, int timeout_ms, bool group = false) {
  if (pid <= 0) return true;
  for (int waited = 0;; waited += 10) {
    int st = 0;
    const pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid || r < 0) return true;
    if (waited >= timeout_ms) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  kill(group ? -pid : pid, SIGKILL);
  int st = 0;
  waitpid(pid, &st, 0);
  return false;
}

// The output of a short command (e.g. "ffmpeg -encoders"), or "" if it could not run.
inline std::string run_capture(const std::vector<std::string> &args) {
  std::vector<char *> argv;
  for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
  argv.push_back(nullptr);
  int p[2];
  if (pipe(p) != 0) return {};
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, p[1], 1);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addclose(&fa, p[0]);
  posix_spawn_file_actions_addclose(&fa, p[1]);
  posix_spawnattr_t at;
  posix_spawnattr_init(&at);
  sigset_t none;
  sigemptyset(&none);
  posix_spawnattr_setsigmask(&at, &none);
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGMASK);
  pid_t pid = -1;
  const int rc = posix_spawnp(&pid, argv[0], &fa, &at, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  close(p[1]);
  std::string out;
  if (rc == 0) {
    char b[4096];
    for (ssize_t n; (n = read(p[0], b, sizeof b)) > 0;) out.append(b, static_cast<size_t>(n));
    int st = 0;
    waitpid(pid, &st, 0);
  }
  close(p[0]);
  return out;
}

}  // namespace arx
#endif
