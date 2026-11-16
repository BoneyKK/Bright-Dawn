#include "platform/crash_log.h"

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "core/log.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace bd::platform {
namespace {

std::atomic<int> g_fd{-1};
std::atomic<const std::atomic<std::uint32_t>*> g_guest{nullptr};
std::atomic<bool> g_written{false};  // One crash line, however many handlers see it.

// A line put together without allocating or formatting: all that may be done
// in a signal handler.
struct Line {
  char text[400];
  std::size_t size = 0;
  void add(const char* part) {
    while (*part != '\0' && size + 1 < sizeof text) text[size++] = *part++;
    text[size] = '\0';
  }
  void hex(std::uint64_t value, int digits) {
    char digits_text[19] = "0x";
    for (int i = 0; i < digits; ++i)
      digits_text[2 + i] = "0123456789ABCDEF"[(value >> (4 * (digits - 1 - i))) & 0xF];
    digits_text[2 + digits] = '\0';
    add(digits_text);
  }
};

void write_raw(const char* text) {
  const int fd = g_fd.load();
  if (fd < 0) return;
#if defined(_WIN32)
  (void)_write(fd, text, static_cast<unsigned>(std::strlen(text)));
#else
  ssize_t ignored = ::write(fd, text, std::strlen(text));
  (void)ignored;
#endif
}

// The end of a crash line: where the game's code was, and the full stop.
void finish(Line& line) {
  if (const std::atomic<std::uint32_t>* guest = g_guest.load()) {
    line.add("; the game's code was last entered at ");
    line.hex(guest->load(std::memory_order_relaxed), 8);
  }
  line.add(".\n");
}

void write_once(const Line& line) {
  if (g_written.exchange(true)) return;
  write_raw(line.text);
}

#if !defined(_WIN32)
// Its own stack, so that a stack that overflowed can still be reported.
alignas(16) char g_signal_stack[64 * 1024];

void on_signal(int signal_number, siginfo_t* info, void*) {
  Line line;
  line.add("\nCRASH: ");
  switch (signal_number) {
    case SIGSEGV: line.add("the program crashed: it used memory it may not (SIGSEGV)"); break;
    case SIGBUS: line.add("the program crashed: a bad memory access (SIGBUS)"); break;
    case SIGFPE: line.add("the program crashed: an arithmetic error (SIGFPE)"); break;
    case SIGILL: line.add("the program crashed: an illegal instruction (SIGILL)"); break;
    case SIGABRT: line.add("the program was ended because something went wrong (SIGABRT)"); break;
    default: line.add("the program crashed (a signal)"); break;
  }
  if (info != nullptr && (signal_number == SIGSEGV || signal_number == SIGBUS)) {
    line.add(", at memory address ");
    line.hex(reinterpret_cast<std::uintptr_t>(info->si_addr), 16);
  }
  finish(line);
  write_once(line);
  // What would have happened without this handler.
  std::signal(signal_number, SIG_DFL);
  std::raise(signal_number);
}
#else
void on_abort(int) {
  Line line;
  line.add("\nCRASH: the program was ended because something went wrong (abort)");
  finish(line);
  write_once(line);
  std::signal(SIGABRT, SIG_DFL);
  std::raise(SIGABRT);
}

void describe_exception(const EXCEPTION_POINTERS* info, Line& line) {
  const EXCEPTION_RECORD* record = info != nullptr ? info->ExceptionRecord : nullptr;
  const unsigned long code = record != nullptr ? record->ExceptionCode : 0;
  line.add("\nCRASH: the program crashed: ");
  line.add(code == EXCEPTION_ACCESS_VIOLATION        ? "it used memory it may not"
           : code == EXCEPTION_STACK_OVERFLOW        ? "its stack overflowed"
           : code == EXCEPTION_ILLEGAL_INSTRUCTION   ? "an illegal instruction"
           : code == EXCEPTION_INT_DIVIDE_BY_ZERO    ? "a division by zero"
           : code == EXCEPTION_IN_PAGE_ERROR         ? "memory could not be read in"
           : code == EXCEPTION_PRIV_INSTRUCTION      ? "a privileged instruction"
                                                     : "an exception");
  line.add(" (exception ");
  line.hex(code, 8);
  line.add(")");
  if (record == nullptr) return;
  // Which module the failing instruction is in, and where in it.
  const auto at = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
  HMODULE module = nullptr;
  char path[MAX_PATH] = "";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(record->ExceptionAddress), &module) &&
      GetModuleFileNameA(module, path, sizeof path) != 0) {
    const char* name = path;
    for (const char* c = path; *c != '\0'; ++c)
      if (*c == '\\' || *c == '/') name = c + 1;
    line.add(", in ");
    line.add(name);
    line.add(" at ");
    line.hex(at - reinterpret_cast<std::uintptr_t>(module), 8);
  } else {
    line.add(", at ");
    line.hex(at, 16);
  }
  if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
    line.add(record->ExceptionInformation[0] == 1 ? ", writing " : ", reading ");
    line.hex(record->ExceptionInformation[1], 16);
  }
}

// An overflowed stack leaves little room: this runs first, in the space
// SetThreadStackGuarantee keeps, and writes the line before anything else.
LONG WINAPI on_first_chance(EXCEPTION_POINTERS* info) {
  if (info != nullptr && info->ExceptionRecord != nullptr &&
      info->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
    Line line;
    describe_exception(info, line);
    finish(line);
    write_once(line);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
  Line line;
  describe_exception(info, line);
  finish(line);
  write_once(line);
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void on_terminate() {
  std::string message = "the program was ended: an error nothing handled";
  if (const std::exception_ptr current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      message += std::string(": ") + e.what();
    } catch (...) {
      message += " (not a standard exception)";
    }
  }
  log::error("crash", message);
  std::abort();
}

}  // namespace

void log_crashes() {
  g_fd.store(log::file_descriptor());
  static bool installed = false;
  if (installed) return;
  installed = true;
  std::set_terminate(on_terminate);
#if defined(_WIN32)
  // Room for the handlers when the main thread's stack overflows.
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);
  AddVectoredExceptionHandler(1, on_first_chance);
  SetUnhandledExceptionFilter(on_exception);
  std::signal(SIGABRT, on_abort);
#else
  stack_t alternate{};
  alternate.ss_sp = g_signal_stack;
  alternate.ss_size = sizeof g_signal_stack;
  sigaltstack(&alternate, nullptr);
  struct sigaction action {};
  action.sa_sigaction = on_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_NODEFER | SA_ONSTACK;
  for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(s, &action, nullptr);
#endif
}

void set_crash_guest_address(const std::atomic<std::uint32_t>* address) { g_guest.store(address); }

}  // namespace bd::platform
