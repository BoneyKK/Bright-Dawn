#include "nds/frame_2d.h"

#include <cstring>
#include <utility>

namespace bd::nds {

namespace {

using u32 = std::uint32_t;

// Where byte `at` of all the banks laid one after another is
// (VideoMemory::kBankStarts).
const std::uint8_t* in_banks(const VideoMemory& video, u32 at) {
  unsigned bank = VideoMemory::kBanks - 1;
  while (VideoMemory::kBankStarts[bank] > at) --bank;
  return video.contents(static_cast<VideoMemory::Bank>(bank)).data() + (at - VideoMemory::kBankStarts[bank]);
}

}  // namespace

bool Video2DRecorder::place(const VideoMemory& video) {
  taken_.layout.resize(VideoMemory::kLayoutEntries);
  return video.layout_2d(taken_.layout.data());
}

void Video2DRecorder::begin(const VideoMemory& video, bool main_engine, bool second_engine) {
  const auto& writes = video.page_writes();
  if (taken_.memory.size() != VideoMemory::kAllSize) {
    taken_.memory.assign(VideoMemory::kAllSize, 0);
    taken_.versions.assign(VideoMemory::kAllPages, 0);
    seen_.assign(VideoMemory::kAllSize, 0);
    page_writes_taken_.assign(VideoMemory::kAllPages, 0);
    for (unsigned p = 0; p < VideoMemory::kAllPages; ++p) page_writes_taken_[p] = writes[p] - 1;  // All anew.
  }
  for (unsigned p = 0; p < VideoMemory::kAllPages; ++p) {
    if (writes[p] == page_writes_taken_[p]) continue;
    page_writes_taken_[p] = writes[p];
    ++taken_.versions[p];
    const u32 at = p * VideoMemory::kPageSize;
    std::memcpy(taken_.memory.data() + at, in_banks(video, at), VideoMemory::kPageSize);
    std::memcpy(seen_.data() + at, in_banks(video, at), VideoMemory::kPageSize);
  }
  line_writes_ = writes;
  changes_.clear();
  changed_.clear();
  // The pages the engines drawn by the graphics hardware can read: their
  // backgrounds' and sprites' (the main engine's display from a bank too),
  // and their extended palettes.
  shown_pages_.fill(false);
  const auto shown = [&](unsigned from, unsigned count) {
    for (unsigned n = from; n < from + count; ++n)
      if (const u32 entry = taken_.layout[n]; entry != 0) shown_pages_[(entry - 1) / 2] = true;
  };
  if (main_engine) {
    shown(0, 32);
    shown(40, 16);
    shown(64, 64);
    shown(128, VideoMemory::kExtendedPaletteSlots);
  }
  if (second_engine) {
    shown(32, 8);
    shown(56, 8);
    shown(128 + VideoMemory::kExtendedPaletteSlots, VideoMemory::kExtendedPaletteSlots);
  }
}

void Video2DRecorder::line(const VideoMemory& video, unsigned line) {
  // The blocks of the pages written since the line before that differ from
  // what the lines before were drawn with.
  constexpr u32 kBlock = Video2D::kBlockSize;
  const auto& writes = video.page_writes();
  for (unsigned p = 0; p < VideoMemory::kAllPages; ++p) {
    if (writes[p] == line_writes_[p]) continue;
    line_writes_[p] = writes[p];
    if (!shown_pages_[p]) continue;
    const u32 at = p * VideoMemory::kPageSize;
    const std::uint8_t* now = in_banks(video, at);
    for (u32 b = 0; b < VideoMemory::kPageSize; b += kBlock) {
      std::uint8_t* seen = seen_.data() + at + b;
      if (std::memcmp(now + b, seen, kBlock) == 0) continue;
      std::memcpy(seen, now + b, kBlock);
      if (changes_.size() >= kMostChanges) {
        ++left_out_;
        continue;
      }
      changes_.push_back({(at + b) / kBlock, line});
      changed_.insert(changed_.end(), now + b, now + b + kBlock);
    }
  }
}

std::uint64_t Video2DRecorder::finish() {
  // The changes in the form Video2D has them: block by block, and in a
  // block in the order they were made.
  Video2D& v = taken_;
  v.changes.clear();
  v.changed.clear();
  const std::uint64_t left_out = std::exchange(left_out_, 0);
  if (changes_.empty()) return left_out;
  constexpr u32 kBlocks = VideoMemory::kAllSize / Video2D::kBlockSize;
  v.changes.assign(2 * kBlocks + changes_.size(), 0);
  for (const Change& c : changes_) ++v.changes[2 * c.block + 1];
  u32 start = 0;
  for (u32 b = 0; b < kBlocks; ++b) {
    v.changes[2 * b] = start;
    start += v.changes[2 * b + 1];
    v.changes[2 * b + 1] = 0;
  }
  for (std::size_t n = 0; n < changes_.size(); ++n) {
    const Change& c = changes_[n];
    const u32 place = v.changes[2 * c.block] + v.changes[2 * c.block + 1]++;
    v.changes[2 * kBlocks + place] = c.line | static_cast<u32>(n) << 8;
  }
  std::swap(v.changed, changed_);
  return left_out;
}

}  // namespace bd::nds
