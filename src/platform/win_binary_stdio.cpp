// Linked into the command-line tools on Windows: stdin/stdout default to text mode there (CRLF translation,
// ^Z = EOF), which corrupts raw IQ and frame pipes. Switch both to binary before main() runs.
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <cstdio>
namespace {
struct BinaryStdio {
  BinaryStdio() {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
  }
} g_binary_stdio;
}  // namespace
#endif
