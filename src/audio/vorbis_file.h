// An Ogg Vorbis file, decoded as it plays, by stb_vorbis (third_party/stb).
//
// Only this file and its .cpp see the library. The file is read from disk a
// little at a time; seeking is exact to the sample, which is what loop
// points need. Files with one channel play on both sides; of files with
// more than two, the first two are played.
//
// The file's comments (its tags) are kept, their names in capitals: a loop
// can be given there (LOOPSTART, LOOPLENGTH or LOOPEND, music_pack.h).
#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/source.h"
#include "core/result.h"

struct stb_vorbis;

namespace bd::audio {

class VorbisFile final : public Source {
 public:
  static Result<std::unique_ptr<VorbisFile>> open(const std::filesystem::path& path);
  ~VorbisFile() override;
  VorbisFile(const VorbisFile&) = delete;
  VorbisFile& operator=(const VorbisFile&) = delete;

  int rate() const override { return rate_; }
  std::uint64_t length() const override { return length_; }
  std::size_t read(float* stereo, std::size_t frames) override;
  bool seek(std::uint64_t frame) override;

  int channels() const { return channels_; }
  // "LOOPSTART" -> "441000".
  const std::map<std::string, std::string>& tags() const { return tags_; }

 private:
  VorbisFile() = default;

  stb_vorbis* vorbis_ = nullptr;
  int rate_ = 0, channels_ = 0;
  std::uint64_t length_ = 0;
  std::map<std::string, std::string> tags_;
  std::vector<float> buffer_;  // The file's own channels, before they are made two.
};

}  // namespace bd::audio
