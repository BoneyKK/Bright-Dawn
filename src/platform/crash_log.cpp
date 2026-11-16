#include "platform/crash_log.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstdio>
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

// Only what may be done in a signal handler: one write of a fixed text.
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

#if !defined(_WIN32)
void on_signal(int signal_number) {
  switch (signal_number) {
    case SIGSEGV: write_raw("\nCRASH: the program crashed: it used memory it may not (SIGSEGV).\n"); break;
    case SIGBUS: write_raw("\nCRASH: the program crashed: a bad memory access (SIGBUS).\n"); break;
    case SIGFPE: write_raw("\nCRASH: the program crashed: an arithmetic error (SIGFPE).\n"); break;
    case SIGILL: write_raw("\nCRASH: the program crashed: an illegal instruction (SIGILL).\n"); break;
    case SIGABRT: write_raw("\nCRASH: the program was ended because something went wrong (SIGABRT).\n"); break;
    default: write_raw("\nCRASH: the program crashed (a signal).\n"); break;
  }
  // What would have happened without this handler.
  std::signal(signal_number, SIG_DFL);
  std::raise(signal_number);
}
#else
void on_abort(int) {
  write_raw("\nCRASH: the program was ended because something went wrong (abort).\n");
  std::signal(SIGABRT, SIG_DFL);
  std::raise(SIGABRT);
}

LONG WINAPI on_exception(EXCEPTION_POINTERS* info) {
  char text[160];
  const unsigned long code = info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionCode : 0;
  const char* what = code == EXCEPTION_ACCESS_VIOLATION      ? "it used memory it may not"
                     : code == EXCEPTION_STACK_OVERFLOW      ? "its stack overflowed"
                     : code == EXCEPTION_ILLEGAL_INSTRUCTION ? "an illegal instruction"
                     : code == EXCEPTION_INT_DIVIDE_BY_ZERO  ? "a division by zero"
                                                             : "an exception";
  // Not a signal handler: formatting is safe here.
  std::snprintf(text, sizeof text, "\nCRASH: the program crashed: %s (exception 0x%08lX).\n", what, code);
  write_raw(text);
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
  SetUnhandledExceptionFilter(on_exception);
  std::signal(SIGABRT, on_abort);
#else
  struct sigaction action {};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESETHAND | SA_NODEFER;
  for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) sigaction(s, &action, nullptr);
#endif
}

}  // namespace bd::platform
