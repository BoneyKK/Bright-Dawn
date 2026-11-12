#include "core/result.h"

namespace bd {

const char* to_string(ErrorCode code) {
  switch (code) {
    case ErrorCode::Io: return "I/O error";
    case ErrorCode::InvalidArgument: return "invalid argument";
    case ErrorCode::NotNds: return "not a Nintendo DS ROM";
    case ErrorCode::Truncated: return "truncated ROM";
    case ErrorCode::Corrupt: return "corrupt data";
    case ErrorCode::WrongGame: return "wrong game";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::NotFound: return "not found";
  }
  return "unknown error";
}

}  // namespace bd
