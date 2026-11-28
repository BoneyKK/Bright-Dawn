#include "audio/music_pack.h"

#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>

namespace bd::audio {
namespace {

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) s.remove_suffix(1);
  return s;
}

std::string upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

// Digits with at most one point: "12", "12.5". Nothing for anything else.
std::optional<double> decimal(std::string_view s) {
  if (s.empty() || s.size() > 20) return std::nullopt;
  double whole = 0, scale = 0;
  bool digits = false;
  for (char c : s) {
    if (c == '.') {
      if (scale != 0) return std::nullopt;
      scale = 1;
    } else if (c >= '0' && c <= '9') {
      digits = true;
      if (scale == 0) {
        whole = whole * 10 + (c - '0');
      } else {
        scale /= 10;
        whole += (c - '0') * scale;
      }
    } else {
      return std::nullopt;
    }
  }
  if (!digits) return std::nullopt;
  return whole;
}

std::optional<bool> yes_no(std::string_view s) {
  const std::string v = upper(s);
  if (v == "YES" || v == "ON" || v == "TRUE" || v == "1") return true;
  if (v == "NO" || v == "OFF" || v == "FALSE" || v == "0") return false;
  return std::nullopt;
}

std::optional<std::uint64_t> tag_frame(const std::map<std::string, std::string>& tags,
                                       std::initializer_list<const char*> names, int rate) {
  for (const char* name : names) {
    const auto it = tags.find(name);
    if (it == tags.end()) continue;
    if (const auto place = parse_place(it->second)) return place->frame(rate);
  }
  return std::nullopt;
}

}  // namespace

std::uint64_t Place::frame(int rate) const {
  if (!seconds) return static_cast<std::uint64_t>(value);
  return static_cast<std::uint64_t>(std::llround(value * rate));
}

std::optional<Place> parse_place(std::string_view text) {
  text = trim(text);
  if (const std::size_t colon = text.find(':'); colon != std::string_view::npos) {
    const auto minutes = decimal(text.substr(0, colon));
    const auto seconds = decimal(text.substr(colon + 1));
    if (!minutes || !seconds || std::floor(*minutes) != *minutes || *seconds >= 60) return std::nullopt;
    return Place{*minutes * 60 + *seconds, true};
  }
  if (!text.empty() && (text.back() == 's' || text.back() == 'S')) {
    const auto seconds = decimal(trim(text.substr(0, text.size() - 1)));
    if (!seconds) return std::nullopt;
    return Place{*seconds, true};
  }
  const auto samples = decimal(text);
  if (!samples || text.find('.') != std::string_view::npos) return std::nullopt;
  return Place{*samples, false};
}

std::string sequence_key(std::string_view name) {
  std::string key = upper(trim(name));
  if (key.rfind("SEQ_", 0) != 0) key = "SEQ_" + key;
  return key;
}

Loop resolve_loop(const PackTrack& track, int rate, std::uint64_t length,
                  const std::map<std::string, std::string>& tags, std::string& problem) {
  Loop loop;
  loop.on = track.loop.value_or(true);
  loop.end = length;
  const auto start = track.loop_start ? std::optional(track.loop_start->frame(rate))
                                      : tag_frame(tags, {"LOOPSTART", "LOOP_START"}, rate);
  std::optional<std::uint64_t> end;
  if (track.loop_end) {
    end = track.loop_end->frame(rate);
  } else if (!track.loop_start) {
    end = tag_frame(tags, {"LOOPEND", "LOOP_END"}, rate);
    if (!end) {
      if (const auto len = tag_frame(tags, {"LOOPLENGTH", "LOOP_LENGTH"}, rate)) end = start.value_or(0) + *len;
    }
  }
  loop.start = start.value_or(0);
  if (end) loop.end = std::min(*end, length);
  if (loop.start >= loop.end) {
    problem = "its loop (" + std::to_string(loop.start) + " to " + std::to_string(loop.end) +
              ") ends before it starts; the whole file loops";
    loop.start = 0;
    loop.end = length;
  }
  return loop;
}

MusicPack MusicPack::from(const std::filesystem::path& folder, std::string_view manifest,
                          const std::vector<std::filesystem::path>& files, std::vector<std::string>& problems) {
  MusicPack pack;
  // The files, by their names in capitals.
  std::map<std::string, std::filesystem::path> by_name;
  for (const auto& f : files) {
    by_name[upper(f.filename().string())] = f;
    if (upper(f.extension().string()) != ".OGG") continue;
    PackTrack t;
    t.sequence = sequence_key(f.stem().string());
    t.file = f;
    pack.tracks_[t.sequence] = t;
  }

  double pack_volume = 1.0;
  std::string section;
  PackTrack* track = nullptr;
  std::size_t line_number = 0;
  std::size_t at = 0;
  while (at <= manifest.size()) {
    std::size_t end = manifest.find('\n', at);
    if (end == std::string_view::npos) end = manifest.size();
    std::string_view line = manifest.substr(at, end - at);
    at = end + 1;
    ++line_number;
    if (const std::size_t comment = line.find_first_of(";#"); comment != std::string_view::npos) line = line.substr(0, comment);
    line = trim(line);
    if (line.empty()) continue;
    const std::string where = std::string(kManifest) + ", line " + std::to_string(line_number) + ": ";
    if (line.front() == '[') {
      if (line.back() != ']') {
        problems.push_back(where + "a section's name has no ']'");
        continue;
      }
      section = upper(trim(line.substr(1, line.size() - 2)));
      track = nullptr;
      if (section == "PACK") continue;
      section = sequence_key(section);
      PackTrack& t = pack.tracks_[section];
      t.sequence = section;
      track = &t;
      continue;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      problems.push_back(where + "not \"name = value\"");
      continue;
    }
    const std::string key = upper(trim(line.substr(0, equals)));
    const std::string_view value = trim(line.substr(equals + 1));
    if (section == "PACK") {
      const auto v = decimal(value);
      if (key == "VOLUME" && v) pack_volume = *v / 100;
      else problems.push_back(where + "the pack takes only \"volume = percent\"");
      continue;
    }
    if (track == nullptr) {
      problems.push_back(where + "\"" + key + "\" before any [section]");
      continue;
    }
    if (key == "FILE") {
      const auto found = by_name.find(upper(std::filesystem::path(std::string(value)).filename().string()));
      if (found != by_name.end()) track->file = found->second;
      else track->file = folder / std::string(value);
    } else if (key == "LOOP_START" || key == "LOOP_END") {
      const auto place = parse_place(value);
      if (!place) problems.push_back(where + "\"" + std::string(value) + "\" is not a place (a sample number, 12.5s or 1:02.5)");
      else (key == "LOOP_START" ? track->loop_start : track->loop_end) = place;
    } else if (key == "LOOP") {
      track->loop = yes_no(value);
      if (!track->loop) problems.push_back(where + "loop is yes or no");
    } else if (key == "VOLUME") {
      const auto v = decimal(value);
      if (v) track->volume = *v / 100;
      else problems.push_back(where + "volume is a percentage");
    } else {
      problems.push_back(where + "\"" + key + "\" is not known");
    }
  }
  for (auto it = pack.tracks_.begin(); it != pack.tracks_.end();) {
    if (it->second.file.empty()) {
      problems.push_back(it->first + ": no file named for it");
      it = pack.tracks_.erase(it);
      continue;
    }
    it->second.volume *= pack_volume;
    ++it;
  }
  return pack;
}

MusicPack MusicPack::scan(const std::filesystem::path& folder, std::vector<std::string>& problems) {
  std::error_code ec;
  if (!std::filesystem::is_directory(folder, ec)) return MusicPack{};
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
    if (entry.is_regular_file(ec)) files.push_back(entry.path());
  std::string manifest;
  for (const auto& f : files) {
    if (upper(f.filename().string()) != upper(kManifest)) continue;
    std::ifstream in(f, std::ios::binary);
    manifest.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (manifest.size() > 1 << 20) {
      problems.push_back(std::string(kManifest) + " is larger than a megabyte, and is not read");
      manifest.clear();
    }
  }
  return from(folder, manifest, files, problems);
}

const PackTrack* MusicPack::find(std::string_view sequence) const {
  const auto it = tracks_.find(sequence_key(sequence));
  return it == tracks_.end() ? nullptr : &it->second;
}

}  // namespace bd::audio
