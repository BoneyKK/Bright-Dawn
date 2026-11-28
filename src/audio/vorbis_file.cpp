#include "audio/vorbis_file.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace bd::audio {
namespace {

std::FILE* open_for_reading(const std::filesystem::path& path) {
#ifdef _WIN32
  return _wfopen(path.c_str(), L"rb");
#else
  return std::fopen(path.c_str(), "rb");
#endif
}

}  // namespace

Result<std::unique_ptr<VorbisFile>> VorbisFile::open(const std::filesystem::path& path) {
  std::FILE* file = open_for_reading(path);
  if (file == nullptr) return Error{ErrorCode::Io, "cannot open " + path.string()};
  int error = 0;
  // The library closes the file when it is done with it, or when it fails.
  stb_vorbis* v = stb_vorbis_open_file(file, 1, &error, nullptr);
  if (v == nullptr) return Error{ErrorCode::Corrupt, path.string() + " is not an Ogg Vorbis file (error " + std::to_string(error) + ")"};
  std::unique_ptr<VorbisFile> f(new VorbisFile());
  f->vorbis_ = v;
  const stb_vorbis_info info = stb_vorbis_get_info(v);
  f->rate_ = static_cast<int>(info.sample_rate);
  f->channels_ = info.channels;
  const stb_vorbis_int64 length = stb_vorbis_stream_length_in_samples64(v);
  f->length_ = length > 0 ? static_cast<std::uint64_t>(length) : 0;
  if (f->rate_ <= 0 || f->rate_ > 384000 || f->channels_ <= 0 || f->length_ == 0)
    return Error{ErrorCode::Unsupported, path.string() + ": no sound in it, or a sample rate that cannot be played"};
  const stb_vorbis_comment comments = stb_vorbis_get_comment(v);
  for (int i = 0; i < comments.comment_list_length; ++i) {
    const std::string entry = comments.comment_list[i] != nullptr ? comments.comment_list[i] : "";
    const std::size_t equals = entry.find('=');
    if (equals == std::string::npos) continue;
    std::string name = entry.substr(0, equals);
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    f->tags_[name] = entry.substr(equals + 1);
  }
  return f;
}

VorbisFile::~VorbisFile() {
  if (vorbis_ != nullptr) stb_vorbis_close(vorbis_);
}

std::size_t VorbisFile::read(float* stereo, std::size_t frames) {
  std::size_t done = 0;
  while (done < frames) {
    const std::size_t want = std::min<std::size_t>(frames - done, 4096);
    buffer_.resize(want * static_cast<std::size_t>(channels_));
    const int got = stb_vorbis_get_samples_float_interleaved(vorbis_, channels_, buffer_.data(),
                                                             static_cast<int>(buffer_.size()));
    if (got <= 0) break;
    for (int i = 0; i < got; ++i) {
      const float* in = &buffer_[static_cast<std::size_t>(i) * channels_];
      float* out = &stereo[(done + static_cast<std::size_t>(i)) * 2];
      out[0] = in[0];
      out[1] = channels_ == 1 ? in[0] : in[1];
    }
    done += static_cast<std::size_t>(got);
  }
  return done;
}

bool VorbisFile::seek(std::uint64_t frame) {
  return stb_vorbis_seek64(vorbis_, static_cast<stb_vorbis_int64>(frame)) != 0;
}

}  // namespace bd::audio
