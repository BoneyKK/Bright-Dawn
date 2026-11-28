#include "audio/music_replacer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio/vorbis_file.h"
#include "core/log.h"

namespace bd::audio {

// --- The catalogue --------------------------------------------------------

std::uint64_t SequenceCatalog::key(const std::uint8_t* data, std::size_t size) {
  // FNV-1a over the first bytes, and how many there were.
  std::uint64_t h = 0xcbf29ce484222325ull ^ size;
  for (std::size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 0x100000001b3ull;
  return h;
}

SequenceCatalog::SequenceCatalog(const std::vector<rom::SdatSequence>& sequences) {
  for (const rom::SdatSequence& s : sequences) {
    if (s.name.empty() || s.data.empty()) continue;
    const std::size_t n = std::min(kKeyBytes, s.data.size());
    by_key_.emplace(key(s.data.data(), n), sequences_.size());
    sequences_.push_back(Entry{s.name, std::vector<std::uint8_t>(s.data.begin(), s.data.end())});
  }
}

std::string SequenceCatalog::identify(ByteSpan memory, std::size_t at) const {
  if (at >= memory.size()) return {};
  const std::size_t left = memory.size() - at;
  // A short sequence's key is all of it; try each length a key can have.
  std::size_t best = sequences_.size();
  for (std::size_t n = std::min(kKeyBytes, left); n > 0; --n) {
    const auto range = by_key_.equal_range(key(memory.data() + at, n));
    for (auto it = range.first; it != range.second; ++it) {
      const Entry& e = sequences_[it->second];
      if (e.data.size() < kKeyBytes && e.data.size() != n) continue;
      if (e.data.size() <= left && std::memcmp(e.data.data(), memory.data() + at, e.data.size()) == 0)
        best = std::min(best, it->second);
    }
    if (n == kKeyBytes && best < sequences_.size()) break;
  }
  return best < sequences_.size() ? sequences_[best].name : std::string();
}

// --- The music ------------------------------------------------------------

MusicReplacer::MusicReplacer(SequenceCatalog catalog, MusicPack pack)
    : catalog_(std::move(catalog)), pack_(std::move(pack)) {}

bool MusicReplacer::prepared(unsigned player, std::uint32_t starts, ByteSpan memory, std::size_t at) {
  if (player >= kPlayers) return false;
  Slot& s = pending_[player];
  s = Slot{};
  s.starts = starts;
  s.fresh = true;
  s.name = catalog_.identify(memory, at);
  s.track = s.name.empty() || !enabled_ ? nullptr : pack_.find(s.name);
  // The names, for whoever makes a pack.
  if (!s.name.empty() && s.track == nullptr) log::info("music", s.name + " (the game's own) on player " + std::to_string(player));
  if (s.track == nullptr) s.name.clear();  // The game's own music: nothing to follow.
  return s.track != nullptr;
}

std::uint32_t MusicReplacer::take_failed() {
  const std::uint32_t f = failed_;
  failed_ = 0;
  return f;
}

void MusicReplacer::stop_all() {
  for (unsigned p = 0; p < kPlayers; ++p) {
    if (current_[p].voice) current_[p].voice->stop();
    current_[p] = Slot{};
    pending_[p] = Slot{};
  }
}

void MusicReplacer::update(const std::array<PlayerState, kPlayers>& players, float master, Mixer& mixer) {
  for (unsigned p = 0; p < kPlayers; ++p) {
    const PlayerState& state = players[p];
    Slot& cur = current_[p];
    // A new sequence on this player: what played for the one before stops.
    if (pending_[p].fresh) {
      if (cur.voice) cur.voice->stop();
      cur = pending_[p];
      cur.fresh = false;
      pending_[p].fresh = false;
      cur.ends = state.ends;
      if (cur.track != nullptr) {
        auto file = VorbisFile::open(cur.track->file);
        if (!file) {
          log::warning("music", cur.name + ": " + file.error().message + "; the game's own music plays");
          failed_ |= 1u << p;
          cur.track = nullptr;
        } else {
          std::string problem;
          const Loop loop = resolve_loop(*cur.track, file.value()->rate(), file.value()->length(), file.value()->tags(), problem);
          if (!problem.empty()) log::warning("music", cur.name + ": " + problem);
          log::info("music", cur.name + " plays " + cur.track->file.filename().string() +
                                 (loop.on ? ", looping from " + std::to_string(loop.start) + " to " + std::to_string(loop.end)
                                          : ", once"));
          cur.voice = std::make_shared<MusicVoice>(std::move(file.value()), loop, Mixer::kRate);
        }
      }
    }
    if (!cur.voice) continue;
    if (state.starts != cur.starts) continue;  // Its own sequence is not yet seen.
    if (!state.prepared) {
      // The game stopped it, or it ended by itself.
      if (state.ends != cur.ends) cur.voice->finish();
      else cur.voice->stop();
      if (!cur.in_mixer) cur.voice.reset();
      cur = Slot{};
      continue;
    }
    const float level = std::pow(10.0f, static_cast<float>(state.decibels) / 200.0f);
    cur.voice->set_gain(level * master * static_cast<float>(cur.track->volume) * volume_ * kFileLevel);
    cur.voice->set_paused(state.paused);
    if (state.playing && !cur.in_mixer) {
      mixer.play(cur.voice);
      cur.in_mixer = true;
    }
  }
}

}  // namespace bd::audio
