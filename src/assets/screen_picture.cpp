#include "assets/screen_picture.h"

#include <functional>
#include <string>

#include "rom/compression.h"

namespace bd::assets {
namespace {

constexpr std::size_t kPixels = static_cast<std::size_t>(kScreenPictureWidth) * kScreenPictureHeight;

Error not_a_picture(const std::string& why) { return Error{ErrorCode::Corrupt, "not a picture of the game's menus: " + why}; }

// The file's contents, its compression removed when it has any.
Result<std::vector<std::uint8_t>> contents(ByteSpan file) {
  if (!file.empty() && file[0] == 0x40) {
    // A stored picture may start with 0x40 too, as part of its first
    // colour: it is taken as compressed only if it decodes as such.
    if (auto decoded = rom::lz40_decompress_file(file)) return decoded;
  }
  return std::vector<std::uint8_t>(file.begin(), file.end());
}

// Pixels `count` bytes from the end of `bytes`, coloured from `colours`.
Result<Picture> paint(const std::vector<std::uint8_t>& bytes, ByteSpan colours, int width, int height,
                      bool clear_zero) {
  const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (colours.empty() || colours.size() % 2 != 0 || colours.size() > 512)
    return not_a_picture("a palette of " + std::to_string(colours.size()) + " bytes");
  const std::size_t colour_count = colours.size() / 2;
  Picture picture;
  picture.width = width;
  picture.height = height;
  picture.rgba.resize(count * 4);
  const std::uint8_t* pixels = bytes.data() + (bytes.size() - count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::size_t index = pixels[i];
    if (index >= colour_count)
      return not_a_picture("a pixel uses colour " + std::to_string(index) + " of " + std::to_string(colour_count));
    const unsigned colour = colours[index * 2] | (static_cast<unsigned>(colours[index * 2 + 1]) << 8);
    picture.rgba[i * 4 + 0] = widen5(colour);
    picture.rgba[i * 4 + 1] = widen5(colour >> 5);
    picture.rgba[i * 4 + 2] = widen5(colour >> 10);
    picture.rgba[i * 4 + 3] = clear_zero && index == 0 ? 0 : 255;
  }
  return picture;
}

Result<Picture> read_named(const rom::Rom& rom, std::string_view path,
                           const std::function<Result<Picture>(ByteSpan)>& decode) {
  const rom::NamedFile* named = rom.fs().find(path);
  if (named == nullptr) return Error{ErrorCode::NotFound, "the ROM has no " + std::string(path)};
  auto file = rom.file(named->file_id);
  if (!file) return file.error();
  auto picture = decode(file.value());
  if (!picture) return Error{picture.error().code, std::string(path) + ": " + picture.error().message};
  return picture;
}

}  // namespace

Result<Picture> decode_indexed_picture(ByteSpan file, int width, int height, bool clear_zero) {
  if (width <= 0 || height <= 0 || width > 1024 || height > 1024)
    return Error{ErrorCode::InvalidArgument, "a picture of " + std::to_string(width) + " by " + std::to_string(height)};
  auto data = contents(file);
  if (!data) return data.error();
  const std::vector<std::uint8_t>& bytes = data.value();
  const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  if (bytes.size() <= count)
    return not_a_picture(std::to_string(bytes.size()) + " bytes, too few for " + std::to_string(width) + " by " +
                         std::to_string(height) + " and a palette");
  return paint(bytes, ByteSpan(bytes.data(), bytes.size() - count), width, height, clear_zero);
}

Result<Picture> read_indexed_picture(const rom::Rom& rom, std::string_view path, int width, int height,
                                     bool clear_zero) {
  return read_named(rom, path, [&](ByteSpan file) { return decode_indexed_picture(file, width, height, clear_zero); });
}

Result<Picture> decode_screen_picture(ByteSpan file, std::optional<ByteSpan> palette) {
  auto data = contents(file);
  if (!data) return data.error();
  const std::vector<std::uint8_t>& bytes = data.value();
  if (bytes.size() < kPixels) return not_a_picture("too small (" + std::to_string(bytes.size()) + " bytes)");

  // The palette: what comes before the pixels, or the other file.
  ByteSpan colours = palette ? *palette : ByteSpan(bytes.data(), bytes.size() - kPixels);
  if (palette && bytes.size() != kPixels) return not_a_picture("a palette was given, but the file has one");
  return paint(bytes, colours, kScreenPictureWidth, kScreenPictureHeight, false);
}

Result<Picture> read_screen_picture(const rom::Rom& rom, std::string_view path) {
  return read_named(rom, path, [](ByteSpan file) { return decode_screen_picture(file); });
}

}  // namespace bd::assets
