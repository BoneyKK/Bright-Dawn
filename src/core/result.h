// Error reporting primitives shared by every module.
//
// Parsers in this project consume untrusted input (a user-supplied ROM, mod
// packs), so failures are ordinary values rather than exceptions: callers must
// look at the result before using it.
#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace bd {

enum class ErrorCode {
  Io,               // The operating system refused a file operation.
  InvalidArgument,  // The caller asked for something that makes no sense.
  NotNds,           // The data is not a Nintendo DS ROM image.
  Truncated,        // The image ends before data it claims to contain.
  Corrupt,          // A structure inside the image is inconsistent.
  WrongGame,        // A valid DS ROM, but not a supported game.
  Unsupported,      // Valid data that this build cannot handle.
  NotFound,         // A requested file or entry does not exist.
};

const char* to_string(ErrorCode code);

struct Error {
  ErrorCode code;
  std::string message;
};

template <class T>
class [[nodiscard]] Result {
 public:
  Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : v_(std::in_place_index<1>, std::move(error)) {}

  bool ok() const { return v_.index() == 0; }
  explicit operator bool() const { return ok(); }

  T& value() & {
    assert(ok());
    return std::get<0>(v_);
  }
  const T& value() const& {
    assert(ok());
    return std::get<0>(v_);
  }
  T&& value() && {
    assert(ok());
    return std::get<0>(std::move(v_));
  }
  const Error& error() const {
    assert(!ok());
    return std::get<1>(v_);
  }

 private:
  std::variant<T, Error> v_;
};

// Result of an operation that produces no value.
class [[nodiscard]] Status {
 public:
  Status() = default;
  Status(Error error) : error_(std::move(error)) {}

  bool ok() const { return !error_.has_value(); }
  explicit operator bool() const { return ok(); }
  const Error& error() const {
    assert(!ok());
    return *error_;
  }

 private:
  std::optional<Error> error_;
};

}  // namespace bd
