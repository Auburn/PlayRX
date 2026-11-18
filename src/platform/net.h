// Sockets for both platforms under Winsock's names: on Windows the real Winsock; on macOS / Linux, BSD sockets
// with the few Winsock names the app uses mapped onto them (so the network code is written once).
#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
#ifndef SD_BOTH
#define SD_BOTH SHUT_RDWR
#endif
inline int closesocket(SOCKET s) { return ::close(s); }
inline int ioctlsocket(SOCKET s, unsigned long cmd, unsigned long *arg) {
  int v = static_cast<int>(*arg);
  return ::ioctl(s, cmd, &v);
}
struct WSADATA {};
#ifndef MAKEWORD
#define MAKEWORD(a, b) ((a) | ((b) << 8))
#endif
// No Winsock to start; a write to a closed connection must not kill the app (SIGPIPE): ignore it process-wide.
inline int WSAStartup(int, WSADATA *) {
  ::signal(SIGPIPE, SIG_IGN);
  return 0;
}
inline int WSACleanup() { return 0; }
inline void Sleep(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif
