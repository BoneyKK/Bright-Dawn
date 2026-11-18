#include "nds/save_keeper.h"

#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace bd::nds {
namespace {

// A path in UTF-8 for messages; path::string() can fail on Windows for names
// outside the system's code page.
std::string text(const std::filesystem::path& path) {
  const std::u8string u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
}

}  // namespace

SaveKeeper::SaveKeeper(SaveMemory& save, std::filesystem::path path) : save_(save), path_(std::move(path)) {}

std::filesystem::path SaveKeeper::backup_path() const {
  std::filesystem::path backup = path_;
  backup += ".backup";
  return backup;
}

std::uint64_t SaveKeeper::edits() const {
  return save_.statistics().bytes_written + save_.statistics().sectors_erased;
}

Status SaveKeeper::load() {
  written_edits_ = seen_edits_ = edits();
  quiet_frames_ = 0;
  std::error_code ec;
  const bool exists = std::filesystem::exists(path_, ec);
  if (ec) {
    keeping_ = false;
    return Error{ErrorCode::Io, "cannot look for the save file " + text(path_) + ": " + ec.message()};
  }
  if (!exists) return {};
  const std::uintmax_t size = std::filesystem::file_size(path_, ec);
  if (ec || size != SaveMemory::kSize) {
    keeping_ = false;
    return Error{ErrorCode::Corrupt, "the save file " + text(path_) + " is not a save of this game: it holds " +
                                         (ec ? std::string("an unknown number of") : std::to_string(size)) +
                                         " bytes, not " + std::to_string(SaveMemory::kSize)};
  }
  std::vector<std::uint8_t> bytes(SaveMemory::kSize);
  std::ifstream in(path_, std::ios::binary);
  in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!in || in.gcount() != static_cast<std::streamsize>(bytes.size())) {
    keeping_ = false;
    return Error{ErrorCode::Io, "cannot read the save file " + text(path_)};
  }
  if (Status status = save_.load(ByteSpan(bytes.data(), bytes.size())); !status) {
    keeping_ = false;
    return status;
  }
  return {};
}

Status SaveKeeper::frame() {
  const std::uint64_t now = edits();
  if (now != seen_edits_) {
    seen_edits_ = now;
    quiet_frames_ = 0;
    return {};
  }
  if (now == written_edits_) return {};
  if (++quiet_frames_ < kQuietFrames) return {};
  return write();
}

Status SaveKeeper::flush() {
  seen_edits_ = edits();
  if (seen_edits_ == written_edits_) return {};
  return write();
}

Status SaveKeeper::write() {
  // Whatever happens, this change is not tried again until the next one.
  written_edits_ = edits();
  quiet_frames_ = 0;
  if (!keeping_) return {};
  std::error_code ec;
  std::filesystem::create_directories(path_.parent_path(), ec);
  if (!backed_up_) {
    if (std::filesystem::exists(path_, ec))
      std::filesystem::copy_file(path_, backup_path(), std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return Error{ErrorCode::Io, "cannot keep a copy of the save file as " + text(backup_path()) + ": " + ec.message()};
    backed_up_ = true;
  }
  std::filesystem::path temporary = path_;
  temporary += ".new";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    const ByteSpan bytes = save_.contents();
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
      out.close();
      std::filesystem::remove(temporary, ec);
      return Error{ErrorCode::Io, "cannot write the save file " + text(temporary)};
    }
  }
  std::filesystem::rename(temporary, path_, ec);
  if (ec) {
    const std::string why = ec.message();
    std::filesystem::remove(temporary, ec);
    return Error{ErrorCode::Io, "cannot replace the save file " + text(path_) + ": " + why};
  }
  ++writes_;
  return {};
}

}  // namespace bd::nds
