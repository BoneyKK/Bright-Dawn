// A music pack: Ogg Vorbis files that play in place of the game's music.
//
// A pack is a folder (the "music" folder in the user data directory). A file
// named after one of the game's sequences replaces it: "SEQ_MAP1.ogg", or
// "MAP1.ogg", the names of the sound archive in the player's ROM, in any
// case. A file "music.ini" in the folder can name other files and say more:
//
//   [pack]
//   volume = 80            ; percent, for every track of the pack
//
//   [SEQ_MAP1]             ; the sequence, with or without "SEQ_"
//   file = overworld.ogg
//   loop_start = 1:02.5    ; minutes:seconds, 62.5s, or a sample number
//   loop_end = 3:10        ; where it goes back to loop_start
//   loop = yes             ; no: plays once, then is silent
//   volume = 90            ; percent, for this track
//
// Where the pack says nothing of a loop, the file's own tags say it, as
// many music tools write them: LOOPSTART, and LOOPLENGTH or LOOPEND, in
// samples. Where neither does, the whole file loops. A loop is a file's
// own: it need not be where the game's sequence loops.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bd::audio {

// A place in a file: a sample number, or a time in seconds.
struct Place {
  double value = 0;
  bool seconds = false;
  std::uint64_t frame(int rate) const;
};
// "441000", "10s", "10.5s", "1:02.5" (minutes:seconds). Nothing if it is none of these.
std::optional<Place> parse_place(std::string_view text);

struct Loop {
  bool on = true;
  std::uint64_t start = 0, end = 0;  // In frames; it plays [start, end) again and again.
};

struct PackTrack {
  std::string sequence;        // "SEQ_MAP1".
  std::filesystem::path file;
  std::optional<Place> loop_start, loop_end;
  std::optional<bool> loop;
  double volume = 1.0;         // This track's, times the pack's.
};

// The loop of a track whose file has `rate`, `length` frames and `tags`:
// the pack's, else the tags', else the whole file. A loop that makes no
// sense (its end before its start) is the whole file, with why in `problem`.
Loop resolve_loop(const PackTrack& track, int rate, std::uint64_t length,
                  const std::map<std::string, std::string>& tags, std::string& problem);

// "map1", "seq_map1", "SEQ_MAP1" -> "SEQ_MAP1".
std::string sequence_key(std::string_view name);

class MusicPack {
 public:
  static constexpr const char* kManifest = "music.ini";

  // The pack in `folder`; an empty pack if there is none. What could not be
  // read is said in `problems`, a line each.
  static MusicPack scan(const std::filesystem::path& folder, std::vector<std::string>& problems);
  // The same from a manifest's text and the files the folder has (for tests).
  static MusicPack from(const std::filesystem::path& folder, std::string_view manifest,
                        const std::vector<std::filesystem::path>& files, std::vector<std::string>& problems);

  // The track for a sequence ("SEQ_MAP1"), or nothing.
  const PackTrack* find(std::string_view sequence) const;
  std::size_t size() const { return tracks_.size(); }
  const std::map<std::string, PackTrack>& tracks() const { return tracks_; }

 private:
  std::map<std::string, PackTrack> tracks_;
};

}  // namespace bd::audio
