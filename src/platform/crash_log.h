// When the program crashes, the log says so: its last line names what
// happened (the signal, or on Windows the exception), after the lines that
// lead up to it. Written straight to the log's file, which is all that may
// safely be done at that point; the program then ends as it would have.
#pragma once

#include <atomic>
#include <cstdint>

namespace bd::platform {

// Starts watching for crashes, writing to the log file open now (see
// core/log.h). Call again after opening another one. Also logs exceptions
// that nothing caught, before the program is ended for them.
void log_crashes();

// While a game runs, the guest address its code was last entered at (see
// runtime::Machine::last_entered); a crash's line then names it. Null when
// no game runs. The atomic must outlive its use here.
void set_crash_guest_address(const std::atomic<std::uint32_t>* address);

}  // namespace bd::platform
