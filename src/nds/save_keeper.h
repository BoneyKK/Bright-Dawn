// Keeps the game's save between runs: the contents of the cartridge's save
// chip (save_memory.h) in a file in the player's data directory.
//
// The file holds the chip's 256 KiB exactly as they are, the form in which
// many emulators keep a DS save. It is read when the game starts. It is
// written when the game has changed the chip and then left it alone for a
// second (the game writes a save in many small steps; the file should hold a
// whole one), and once more when the run ends. Each write goes to a new file
// that then takes the old one's place, so a crash in the middle of a write
// leaves the old file whole.
//
// Before the first write of a run, the file as it was when the run started is
// kept beside it (".backup"), so one bad run cannot lose the player's save.
//
// A file that cannot be used (the wrong size, or unreadable) is never written
// over: the run goes on with an empty chip, and nothing is kept.
#pragma once

#include <cstdint>
#include <filesystem>

#include "core/result.h"
#include "nds/save_memory.h"

namespace bd::nds {

class SaveKeeper {
 public:
  // Frames (60 a second) the chip must stay unchanged before it is written.
  static constexpr std::uint32_t kQuietFrames = 60;

  SaveKeeper(SaveMemory& save, std::filesystem::path path);

  // Reads the file into the chip. No file is not an error: the chip stays
  // new, as in a cartridge never played. On an error, nothing will be
  // written for the rest of the run.
  Status load();

  // Once a frame: writes the file when the chip changed and has then stayed
  // the same for kQuietFrames frames.
  Status frame();

  // Writes the file now if the chip changed since it was last written (at
  // the end of a run).
  Status flush();

  const std::filesystem::path& path() const { return path_; }
  std::filesystem::path backup_path() const;
  bool keeping() const { return keeping_; }
  std::uint64_t writes() const { return writes_; }

 private:
  std::uint64_t edits() const;
  Status write();

  SaveMemory& save_;
  std::filesystem::path path_;
  bool keeping_ = true;
  bool backed_up_ = false;
  std::uint64_t written_edits_ = 0;  // edits() when the file was last written or read.
  std::uint64_t seen_edits_ = 0;     // edits() at the last frame.
  std::uint32_t quiet_frames_ = 0;
  std::uint64_t writes_ = 0;
};

}  // namespace bd::nds
