#include "assets/screen_picture.h"

#include <string>

#include "rom/compression.h"

namespace bd::assets {
namespace {

constexpr std::size_t kPixels = static_cast<std::size_t>(kScreenPictureWidth) * kScreenPictureHeight;

Error not_a_picture(const std::string& why) { return Error{ErrorCode::Corrupt, "not a screen picture: " + why}; }

// The file's contents, its compression removed when it has any.
Result<std::vector<std::uint8_t>> contents(ByteSpan file) {
  if (!file.empty() && file[0] == 0x40) {
    // A stored picture may start with 0x40 too, as part of its first
    // colour: it is taken as compressed only if it decodes as such.
    if (auto decoded = rom::lz40_decompress_file(file)) return decoded;
  }
  return std::vector<std::uint8_t>(file.begin(), file.end());
}

}  // namespace

Result<Picture> decode_screen_picture(ByteSpan file, std::optional<ByteSpan> palette) {
  auto data = contents(file);
  if (!data) return data.error();
  const std::vector<std::uint8_t>& bytes = data.value();
  if (bytes.size() < kPixels) return not_a_picture("too small (" + std::to_string(bytes.size()) + " bytes)");

  // The palette: what comes before the pixels, or the other file.
  ByteSpan colours = palette ? *palette : ByteSpan(bytes.data(), bytes.size() - kPixels);
  if (palette && bytes.size() != kPixels) return not_a_picture("a palette was given, but the file has one");
  if (colours.empty() || colours.size() % 2 != 0 || colours.size() > 512)
    return not_a_picture("a palette of " + std::to_string(colours.size()) + " bytes");
  const std::size_t colour_count = colours.size() / 2;

  Picture picture;
  picture.width = kScreenPictureWidth;
  picture.height = kScreenPictureHeight;
  picture.rgba.resize(kPixels * 4);
  const std::uint8_t* pixels = bytes.data() + (bytes.size() - kPixels);
  for (std::size_t i = 0; i < kPixels; ++i) {
    const std::size_t index = pixels[i];
    if (index >= colour_count)
      return not_a_picture("a pixel uses colour " + std::to_string(index) + " of " + std::to_string(colour_count));
    const unsigned colour = colours[index * 2] | (static_cast<unsigned>(colours[index * 2 + 1]) << 8);
    picture.rgba[i * 4 + 0] = widen5(colour);
    picture.rgba[i * 4 + 1] = widen5(colour >> 5);
    picture.rgba[i * 4 + 2] = widen5(colour >> 10);
    picture.rgba[i * 4 + 3] = 255;
  }
  return picture;
}

Result<Picture> read_screen_picture(const rom::Rom& rom, std::string_view path) {
  const rom::NamedFile* named = rom.fs().find(path);
  if (named == nullptr) return Error{ErrorCode::NotFound, "the ROM has no " + std::string(path)};
  auto file = rom.file(named->file_id);
  if (!file) return file.error();
  auto picture = decode_screen_picture(file.value());
  if (!picture) return Error{picture.error().code, std::string(path) + ": " + picture.error().message};
  return picture;
}

}  // namespace bd::assets
