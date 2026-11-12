#include "codecs.h"

#include <algorithm>
#include <stdexcept>

namespace bd_test {
namespace {

struct Match {
  std::size_t length = 0;
  std::size_t distance = 0;
};

// Longest match for data[pos..] among earlier positions, by exhaustive search.
Match find_match(const Bytes& data, std::size_t pos, std::size_t max_length,
                 std::size_t min_distance, std::size_t max_distance) {
  Match best;
  const std::size_t limit = std::min(max_length, data.size() - pos);
  for (std::size_t distance = min_distance; distance <= max_distance && distance <= pos;
       ++distance) {
    std::size_t length = 0;
    while (length < limit && data[pos + length] == data[pos + length - distance]) ++length;
    if (length > best.length) best = Match{length, distance};
  }
  return best;
}

// A 24-bit size of zero means "a 32-bit size follows", so empty and very
// large payloads use the extended form.
void put_size_header(Bytes& out, std::uint8_t type, std::size_t size) {
  const bool extended = size == 0 || size > 0xFFFFFF;
  const std::size_t short_size = extended ? 0 : size;
  out.push_back(type);
  for (int shift = 0; shift < 24; shift += 8)
    out.push_back(static_cast<std::uint8_t>(short_size >> shift));
  if (extended) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(size >> shift));
  }
}

// Emits flag bytes (most significant bit first) interleaved with tokens.
class TokenWriter {
 public:
  explicit TokenWriter(Bytes& out) : out_(out) {}
  void begin_token(bool is_match) {
    if (bit_ == 0) {
      flag_pos_ = out_.size();
      flag_positions_.push_back(flag_pos_);
      out_.push_back(0);
      bit_ = 0x80;
    }
    if (is_match) out_[flag_pos_] |= bit_;
    bit_ >>= 1;
  }
  // Where each flag byte was written, for formats that transform them.
  const std::vector<std::size_t>& flag_positions() const { return flag_positions_; }

 private:
  Bytes& out_;
  std::vector<std::size_t> flag_positions_;
  std::size_t flag_pos_ = 0;
  std::uint8_t bit_ = 0;
};

}  // namespace

Bytes lz10_compress(const Bytes& raw) {
  Bytes out;
  put_size_header(out, 0x10, raw.size());
  TokenWriter writer(out);
  for (std::size_t pos = 0; pos < raw.size();) {
    const Match m = find_match(raw, pos, 18, 1, 0x1000);
    if (m.length >= 3) {
      writer.begin_token(true);
      const std::size_t d = m.distance - 1;
      out.push_back(static_cast<std::uint8_t>(((m.length - 3) << 4) | (d >> 8)));
      out.push_back(static_cast<std::uint8_t>(d));
      pos += m.length;
    } else {
      writer.begin_token(false);
      out.push_back(raw[pos++]);
    }
  }
  return out;
}

Bytes lz11_compress(const Bytes& raw) {
  Bytes out;
  put_size_header(out, 0x11, raw.size());
  TokenWriter writer(out);
  for (std::size_t pos = 0; pos < raw.size();) {
    const Match m = find_match(raw, pos, 0x10110, 1, 0x1000);
    if (m.length < 3) {
      writer.begin_token(false);
      out.push_back(raw[pos++]);
      continue;
    }
    writer.begin_token(true);
    const std::size_t d = m.distance - 1;
    if (m.length <= 0x10) {
      out.push_back(static_cast<std::uint8_t>(((m.length - 1) << 4) | (d >> 8)));
    } else if (m.length <= 0x110) {
      const std::size_t v = m.length - 0x11;
      out.push_back(static_cast<std::uint8_t>(v >> 4));
      out.push_back(static_cast<std::uint8_t>(((v & 0xF) << 4) | (d >> 8)));
    } else {
      const std::size_t v = m.length - 0x111;
      out.push_back(static_cast<std::uint8_t>(0x10 | (v >> 12)));
      out.push_back(static_cast<std::uint8_t>(v >> 4));
      out.push_back(static_cast<std::uint8_t>(((v & 0xF) << 4) | (d >> 8)));
    }
    out.push_back(static_cast<std::uint8_t>(d));
    pos += m.length;
  }
  return out;
}

Bytes lz40_compress(const Bytes& raw, Lz40Flags flags, bool long_length_big_endian) {
  Bytes out;
  put_size_header(out, 0x40, raw.size());
  TokenWriter writer(out);
  for (std::size_t pos = 0; pos < raw.size();) {
    const Match m = find_match(raw, pos, 0x110 + 0xFFFF, 1, 0xFFF);
    if (m.length < 2) {
      writer.begin_token(false);
      out.push_back(raw[pos++]);
      continue;
    }
    writer.begin_token(true);
    // Lengths 2-15 fit the token; longer ones use one or two extra bytes.
    const std::size_t code = m.length < 0x10 ? m.length : m.length < 0x110 ? 0 : 1;
    const std::size_t word = (m.distance << 4) | code;
    out.push_back(static_cast<std::uint8_t>(word));
    out.push_back(static_cast<std::uint8_t>(word >> 8));
    if (code == 0) {
      out.push_back(static_cast<std::uint8_t>(m.length - 0x10));
    } else if (code == 1) {
      const std::size_t v = m.length - 0x110;
      const auto hi = static_cast<std::uint8_t>(v >> 8);
      const auto lo = static_cast<std::uint8_t>(v);
      out.push_back(long_length_big_endian ? hi : lo);
      out.push_back(long_length_big_endian ? lo : hi);
    }
    pos += m.length;
  }
  for (std::size_t at : writer.flag_positions()) {
    if (flags == Lz40Flags::Negated) out[at] = static_cast<std::uint8_t>(0x100u - out[at]);
    if (flags == Lz40Flags::Inverted) out[at] = static_cast<std::uint8_t>(~out[at]);
  }
  return out;
}

Bytes build_narc(const std::vector<NarcMember>& members) {
  auto put32 = [](Bytes& b, std::size_t v) {
    for (int shift = 0; shift < 32; shift += 8) b.push_back(static_cast<std::uint8_t>(v >> shift));
  };
  auto put16 = [](Bytes& b, std::size_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
  };
  auto section = [&](const char* tag, Bytes body) {
    while (body.size() % 4 != 0) body.push_back(0xFF);
    Bytes out(tag, tag + 4);
    put32(out, body.size() + 8);
    out.insert(out.end(), body.begin(), body.end());
    return out;
  };

  Bytes image;
  Bytes fat;
  put16(fat, members.size());
  put16(fat, 0);
  for (const NarcMember& member : members) {
    put32(fat, image.size());
    image.insert(image.end(), member.data.begin(), member.data.end());
    put32(fat, image.size());
    while (image.size() % 4 != 0) image.push_back(0xFF);
  }

  // A single root directory. Names must cover a contiguous run of file IDs,
  // so naming stops at the first member without one.
  Bytes fnt;
  put32(fnt, 8);
  put16(fnt, 0);
  put16(fnt, 1);
  for (const NarcMember& member : members) {
    if (member.name.empty()) break;
    fnt.push_back(static_cast<std::uint8_t>(member.name.size()));
    fnt.insert(fnt.end(), member.name.begin(), member.name.end());
  }
  fnt.push_back(0);

  const Bytes btaf = section("BTAF", fat);
  const Bytes btnf = section("BTNF", fnt);
  const Bytes gmif = section("GMIF", image);
  Bytes out = {'N', 'A', 'R', 'C', 0xFE, 0xFF, 0x00, 0x01};
  put32(out, 16 + btaf.size() + btnf.size() + gmif.size());
  put16(out, 16);
  put16(out, 3);
  out.insert(out.end(), btaf.begin(), btaf.end());
  out.insert(out.end(), btnf.begin(), btnf.end());
  out.insert(out.end(), gmif.begin(), gmif.end());
  return out;
}

Bytes blz_compress(const Bytes& raw, std::size_t keep_prefix) {
  keep_prefix = std::min(keep_prefix, raw.size());
  // The format is an ordinary forward LZ applied to the reversed data, with
  // the resulting stream stored reversed as well.
  Bytes reversed(raw.rbegin(), raw.rend() - static_cast<std::ptrdiff_t>(keep_prefix));
  Bytes stream;
  TokenWriter writer(stream);
  for (std::size_t pos = 0; pos < reversed.size();) {
    const Match m = find_match(reversed, pos, 18, 3, 0x1002);
    if (m.length >= 3) {
      writer.begin_token(true);
      const std::size_t pair = ((m.length - 3) << 12) | (m.distance - 3);
      stream.push_back(static_cast<std::uint8_t>(pair >> 8));
      stream.push_back(static_cast<std::uint8_t>(pair));
      pos += m.length;
    } else {
      writer.begin_token(false);
      stream.push_back(reversed[pos++]);
    }
  }

  Bytes out(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(keep_prefix));
  out.insert(out.end(), stream.rbegin(), stream.rend());
  std::size_t trailer = 8;
  while (out.size() % 4 != 0) {
    out.push_back(0xFF);
    ++trailer;
  }
  // Padding counts as part of the trailer. Everything after the prefix is the
  // compressed region; "extra" is how much the data grows when decoded.
  const std::size_t region = stream.size() + trailer;
  const std::size_t total = keep_prefix + region;
  if (raw.size() < total) throw std::runtime_error("blz_compress: input does not compress");
  const std::size_t extra = raw.size() - total;
  const std::uint32_t word = static_cast<std::uint32_t>((trailer << 24) | region);
  for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>(word >> shift));
  for (int shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(extra) >> shift));
  return out;
}

Bytes sample_data(std::size_t size, std::uint32_t seed) {
  Bytes out;
  out.reserve(size);
  std::uint32_t state = seed * 2654435761u + 1;
  auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  while (out.size() < size) {
    const std::uint32_t r = next();
    const std::size_t run = 1 + (r >> 8) % 40;
    if ((r & 3) == 0 && out.size() > 64) {
      // Repeat something seen earlier.
      const std::size_t from = (r >> 16) % (out.size() - 32);
      for (std::size_t i = 0; i < run && out.size() < size; ++i) out.push_back(out[from + i % 32]);
    } else if ((r & 3) == 1) {
      for (std::size_t i = 0; i < run && out.size() < size; ++i)
        out.push_back(static_cast<std::uint8_t>(r >> 24));
    } else {
      for (std::size_t i = 0; i < run % 8 + 1 && out.size() < size; ++i)
        out.push_back(static_cast<std::uint8_t>(next()));
    }
  }
  return out;
}

}  // namespace bd_test
