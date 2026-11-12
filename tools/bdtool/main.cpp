// bdtool: inspects a user-supplied ROM. It validates the image, lists and
// classifies its contents, and extracts files into the per-user data
// directory. It never writes into the source tree unless told to.
#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/checksum.h"
#include "platform/paths.h"
#include "rom/export_path.h"
#include "rom/file_format.h"
#include "rom/game_id.h"
#include "rom/rom.h"

namespace {

using bd::ByteSpan;
using bd::rom::Rom;

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

void print_usage(std::FILE* out) {
  std::fputs(
      "bdtool " BD_VERSION " - ROM inspection tool for the Bright Dawn project\n"
      "\n"
      "Usage:\n"
      "  bdtool validate <rom>            Check that <rom> is a usable copy of\n"
      "                                   Golden Sun: Dark Dawn (exit status 0 if so)\n"
      "  bdtool info <rom>                Show header, program and overlay details\n"
      "  bdtool list <rom>                List every file in the ROM file system\n"
      "  bdtool survey <rom> [--top N]    Classify the ROM's contents by format\n"
      "  bdtool extract <rom> [options] [PATH...]\n"
      "                                   Extract files (all files when no PATH is given)\n"
      "      --out DIR                    Destination (default: per-user data directory)\n"
      "      --code                       Also write decompressed program code\n"
      "\n"
      "You must supply your own legally obtained ROM. Nothing is downloaded.\n",
      out);
}

// Text from the ROM is untrusted; keep control characters off the terminal.
std::string printable(std::string_view text) {
  std::string out;
  for (char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    out += (c >= 0x20 && c < 0x7F) ? ch : '?';
  }
  return out;
}

std::string human_size(std::uint64_t bytes) {
  char text[32];
  if (bytes >= 1024ull * 1024) {
    std::snprintf(text, sizeof text, "%.2f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024) {
    std::snprintf(text, sizeof text, "%.1f KiB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(text, sizeof text, "%" PRIu64 " B", bytes);
  }
  return text;
}

int report(const bd::Error& error) {
  std::fprintf(stderr, "error: %s\n", error.message.c_str());
  return kExitFailure;
}

void print_warnings(const Rom& rom) {
  for (const std::string& warning : rom.warnings())
    std::fprintf(stderr, "warning: %s\n", warning.c_str());
}

// Notes on stderr when the ROM is some other DS game. Inspection commands
// still work on it, which is useful for testing the tool itself.
void note_if_other_game(const Rom& rom) {
  if (bd::rom::identify_title(rom.header()) == nullptr) {
    std::fprintf(stderr, "note: game code %s is not a known release of Golden Sun: Dark Dawn\n",
                 printable(rom.header().game_code).c_str());
  }
}

int cmd_validate(const std::filesystem::path& path) {
  auto rom = Rom::open(path);
  if (!rom) return report(rom.error());
  if (auto status = bd::rom::require_dark_dawn(rom.value().header()); !status)
    return report(status.error());
  print_warnings(rom.value());
  if (auto arm9 = rom.value().arm9_decompressed(); !arm9) return report(arm9.error());
  for (const auto& overlay : rom.value().arm9_overlays()) {
    if (auto data = rom.value().overlay_data(overlay); !data) return report(data.error());
  }
  const auto* title = bd::rom::identify_title(rom.value().header());
  std::printf("OK: Golden Sun: Dark Dawn (%s), game code %s, revision %u\n",
              bd::rom::to_string(title->region), rom.value().header().game_code.c_str(),
              static_cast<unsigned>(rom.value().header().rom_version));
  return kExitOk;
}

int cmd_info(const std::filesystem::path& path) {
  auto opened = Rom::open(path);
  if (!opened) return report(opened.error());
  const Rom& rom = opened.value();
  const auto& h = rom.header();
  print_warnings(rom);

  std::printf("File:           %s\n", path.string().c_str());
  std::printf("Size:           %zu bytes (%s), CRC32 %08" PRIX32 "\n", rom.bytes().size(),
              human_size(rom.bytes().size()).c_str(), bd::crc32(rom.bytes()));
  std::printf("Title:          %s\n", printable(h.title).c_str());
  if (const auto* title = bd::rom::identify_title(h)) {
    std::printf("Game code:      %s -> Golden Sun: Dark Dawn (%s)\n", h.game_code.c_str(),
                bd::rom::to_string(title->region));
  } else {
    std::printf("Game code:      %s -> NOT a known release of Golden Sun: Dark Dawn\n",
                printable(h.game_code).c_str());
  }
  std::printf("Maker code:     %s\n", printable(h.maker_code).c_str());
  std::printf("ROM revision:   %u\n", static_cast<unsigned>(h.rom_version));
  std::printf("Cartridge:      %s chip, %s used\n", human_size(h.chip_size()).c_str(),
              human_size(h.used_rom_size).c_str());

  std::printf("ARM9 program:   %u bytes at ROM 0x%08" PRIX32 ", loads at 0x%08" PRIX32
              ", entry 0x%08" PRIX32 "\n",
              static_cast<unsigned>(h.arm9.size), h.arm9.rom_offset, h.arm9.ram_address,
              h.arm9.entry_address);
  if (const auto& params = rom.module_params()) {
    std::printf("  SDK version:  %u.%u (revision %u, raw 0x%08" PRIX32 ")\n", params->sdk_major(),
                params->sdk_minor(), params->sdk_revision(), params->sdk_version);
    if (params->compressed_static_end != 0) {
      auto arm9 = rom.arm9_decompressed();
      if (!arm9) return report(arm9.error());
      std::printf("  Compressed:   yes, %zu bytes once decompressed\n", arm9.value().size());
    } else {
      std::printf("  Compressed:   no\n");
    }
  }
  const char* secure = rom.secure_area() == bd::rom::SecureArea::Decrypted   ? "decrypted"
                       : rom.secure_area() == bd::rom::SecureArea::Encrypted ? "encrypted"
                                                                             : "not present";
  std::printf("  Secure area:  %s\n", secure);
  std::printf("ARM7 program:   %u bytes at ROM 0x%08" PRIX32 ", loads at 0x%08" PRIX32 "\n",
              static_cast<unsigned>(h.arm7.size), h.arm7.rom_offset, h.arm7.ram_address);

  std::size_t compressed = 0;
  std::uint64_t loaded_bytes = 0;
  for (const auto& overlay : rom.arm9_overlays()) {
    compressed += overlay.compressed ? 1 : 0;
    loaded_bytes += overlay.ram_size;
  }
  std::printf("ARM9 overlays:  %zu (%zu compressed), %s of code and data when loaded\n",
              rom.arm9_overlays().size(), compressed, human_size(loaded_bytes).c_str());
  std::printf("ARM7 overlays:  %zu\n", rom.arm7_overlays().size());
  std::printf("File system:    %zu named files in %zu directories, %zu file IDs in total\n",
              rom.fs().named_files().size(), rom.fs().directory_count(),
              rom.fs().ranges().size());
  return kExitOk;
}

int cmd_list(const std::filesystem::path& path) {
  auto opened = Rom::open(path);
  if (!opened) return report(opened.error());
  const Rom& rom = opened.value();
  print_warnings(rom);
  note_if_other_game(rom);
  for (const auto& file : rom.fs().named_files()) {
    const auto& range = rom.fs().ranges()[file.file_id];
    std::printf("%5u  %10u  %s\n", static_cast<unsigned>(file.file_id),
                static_cast<unsigned>(range.size), printable(file.path).c_str());
  }
  return kExitOk;
}

std::string extension_of(std::string_view path) {
  const std::size_t slash = path.rfind('/');
  const std::string_view leaf = slash == std::string_view::npos ? path : path.substr(slash + 1);
  const std::size_t dot = leaf.rfind('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == leaf.size()) return "(none)";
  std::string ext(leaf.substr(dot));
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return printable(ext);
}

struct Tally {
  std::size_t count = 0;
  std::uint64_t bytes = 0;
};

// Rows sorted by total size, largest first.
std::vector<std::pair<std::string, Tally>> by_size(const std::map<std::string, Tally>& tallies) {
  std::vector<std::pair<std::string, Tally>> rows(tallies.begin(), tallies.end());
  std::stable_sort(rows.begin(), rows.end(),
                   [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
  return rows;
}

int cmd_survey(const std::filesystem::path& path, std::size_t top) {
  auto opened = Rom::open(path);
  if (!opened) return report(opened.error());
  const Rom& rom = opened.value();
  print_warnings(rom);
  note_if_other_game(rom);

  std::map<std::string, Tally> by_format;
  std::map<std::string, Tally> by_extension;
  std::map<std::string, Tally> unknown_leads;
  std::map<std::string, std::string> format_notes;
  struct Sized {
    std::uint32_t size;
    std::string path;
    std::string format;
  };
  std::vector<Sized> files;
  std::uint64_t total_bytes = 0;

  for (const auto& file : rom.fs().named_files()) {
    const ByteSpan data = rom.file(file.file_id).value();
    const bd::rom::Sniffed sniffed = bd::rom::sniff(data);
    std::string label = bd::rom::name(sniffed.format);
    if (sniffed.format == bd::rom::FileFormat::Lz10 ||
        sniffed.format == bd::rom::FileFormat::Lz11) {
      label += std::string(" > ") + bd::rom::name(sniffed.inner);
    }
    format_notes[label] = bd::rom::description(
        sniffed.inner != bd::rom::FileFormat::Unknown ? sniffed.inner : sniffed.format);

    auto add = [&](Tally& tally) {
      ++tally.count;
      tally.bytes += data.size();
    };
    add(by_format[label]);
    add(by_extension[extension_of(file.path)]);
    if (sniffed.format == bd::rom::FileFormat::Unknown) {
      // Group unrecognised files by their first four bytes: a value shared by
      // many files is very likely the signature of a game-specific format.
      char lead[40] = "(shorter than 4 bytes)";
      if (data.size() >= 4) {
        char ascii[5];
        for (int i = 0; i < 4; ++i) ascii[i] = (data[i] >= 0x20 && data[i] < 0x7F) ? data[i] : '.';
        ascii[4] = '\0';
        std::snprintf(lead, sizeof lead, "%02X %02X %02X %02X  \"%s\"", data[0], data[1], data[2],
                      data[3], ascii);
      }
      add(unknown_leads[lead]);
    }
    files.push_back(Sized{static_cast<std::uint32_t>(data.size()), file.path, label});
    total_bytes += data.size();
  }

  std::printf("Game code %s, revision %u: %zu named files, %s\n\n",
              printable(rom.header().game_code).c_str(),
              static_cast<unsigned>(rom.header().rom_version), files.size(),
              human_size(total_bytes).c_str());

  std::printf("By detected format\n");
  std::printf("  %-16s %7s %12s  %s\n", "format", "files", "size", "meaning");
  for (const auto& [label, tally] : by_size(by_format)) {
    std::printf("  %-16s %7zu %12s  %s\n", label.c_str(), tally.count,
                human_size(tally.bytes).c_str(), format_notes[label].c_str());
  }

  std::printf("\nBy file extension\n");
  std::printf("  %-16s %7s %12s\n", "extension", "files", "size");
  std::size_t shown = 0;
  for (const auto& [ext, tally] : by_size(by_extension)) {
    if (shown++ == top) break;
    std::printf("  %-16s %7zu %12s\n", ext.c_str(), tally.count, human_size(tally.bytes).c_str());
  }

  if (!unknown_leads.empty()) {
    std::printf("\nLeading bytes of unrecognised files\n");
    std::printf("  %-28s %7s %12s\n", "first four bytes", "files", "size");
    shown = 0;
    for (const auto& [lead, tally] : by_size(unknown_leads)) {
      if (shown++ == top) break;
      std::printf("  %-28s %7zu %12s\n", lead.c_str(), tally.count,
                  human_size(tally.bytes).c_str());
    }
  }

  std::stable_sort(files.begin(), files.end(),
                   [](const Sized& a, const Sized& b) { return a.size > b.size; });
  std::printf("\nLargest files\n");
  for (std::size_t i = 0; i < files.size() && i < top; ++i) {
    std::printf("  %12s  %-16s %s\n", human_size(files[i].size).c_str(), files[i].format.c_str(),
                printable(files[i].path).c_str());
  }

  // Overlays that occupy the same memory cannot be resident together; the
  // count of distinct load addresses hints at how the game swaps code.
  const auto& overlays = rom.arm9_overlays();
  if (!overlays.empty()) {
    std::map<std::uint32_t, std::size_t> by_address;
    for (const auto& overlay : overlays) ++by_address[overlay.ram_address];
    std::printf("\nARM9 overlays: %zu, at %zu distinct load addresses\n", overlays.size(),
                by_address.size());
    for (const auto& [address, count] : by_address)
      std::printf("  0x%08" PRIX32 "  %zu overlay(s)\n", address, count);
  }
  return kExitOk;
}

bd::Status write_file(const std::filesystem::path& path, ByteSpan data) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return bd::Error{bd::ErrorCode::Io, "cannot create directory " +
                                            path.parent_path().string() + ": " + ec.message()};
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (data.size() > 0)
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  out.close();
  if (!out) return bd::Error{bd::ErrorCode::Io, "cannot write " + path.string()};
  return {};
}

int cmd_extract(const std::filesystem::path& path, std::filesystem::path out_dir, bool with_code,
                const std::vector<std::string>& wanted) {
  auto opened = Rom::open(path);
  if (!opened) return report(opened.error());
  const Rom& rom = opened.value();
  print_warnings(rom);
  note_if_other_game(rom);

  if (out_dir.empty()) {
    auto base = bd::platform::user_data_dir();
    if (!base) return report(base.error());
    const auto code = bd::rom::to_export_path(rom.header().game_code);
    if (!code) return report({bd::ErrorCode::Corrupt, "the game code cannot be used as a name"});
    out_dir = base.value() / "extracted" /
              (code->string() + "-rev" + std::to_string(rom.header().rom_version));
  }

  std::vector<const bd::rom::NamedFile*> selection;
  if (wanted.empty()) {
    for (const auto& file : rom.fs().named_files()) selection.push_back(&file);
  } else {
    for (const std::string& name : wanted) {
      const auto* file = rom.fs().find(name);
      if (file == nullptr)
        return report({bd::ErrorCode::NotFound, "no such file in the ROM: " + name});
      selection.push_back(file);
    }
  }

  std::size_t written = 0;
  std::size_t skipped = 0;
  for (const auto* file : selection) {
    const auto relative = bd::rom::to_export_path(file->path);
    if (!relative) {
      std::fprintf(stderr, "warning: skipping file with an unsafe name: %s\n",
                   printable(file->path).c_str());
      ++skipped;
      continue;
    }
    const ByteSpan data = rom.file(file->file_id).value();
    if (auto s = write_file(out_dir / "files" / *relative, data); !s) return report(s.error());
    ++written;
  }

  if (with_code) {
    auto arm9 = rom.arm9_decompressed();
    if (!arm9) return report(arm9.error());
    if (auto s = write_file(out_dir / "code" / "arm9.bin", arm9.value()); !s)
      return report(s.error());
    if (auto s = write_file(out_dir / "code" / "arm7.bin", rom.arm7()); !s)
      return report(s.error());
    written += 2;
    for (const auto& overlay : rom.arm9_overlays()) {
      auto data = rom.overlay_data(overlay);
      if (!data) return report(data.error());
      char name[32];
      std::snprintf(name, sizeof name, "overlay9_%04u.bin", static_cast<unsigned>(overlay.id));
      if (auto s = write_file(out_dir / "code" / name, data.value()); !s) return report(s.error());
      ++written;
    }
  }

  std::printf("Wrote %zu file(s) to %s\n", written, out_dir.string().c_str());
  if (skipped != 0) {
    std::fprintf(stderr, "error: %zu file(s) were skipped\n", skipped);
    return kExitFailure;
  }
  return kExitOk;
}

int usage_error(const char* message) {
  std::fprintf(stderr, "error: %s\n\n", message);
  print_usage(stderr);
  return kExitUsage;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) {
    print_usage(stderr);
    return kExitUsage;
  }
  const std::string& command = args[0];
  if (command == "-h" || command == "--help" || command == "help") {
    print_usage(stdout);
    return kExitOk;
  }
  if (command == "--version") {
    std::puts("bdtool " BD_VERSION);
    return kExitOk;
  }
  if (command != "validate" && command != "info" && command != "list" && command != "survey" &&
      command != "extract") {
    return usage_error(("unknown command: " + command).c_str());
  }

  std::filesystem::path out_dir;
  bool with_code = false;
  std::size_t top = 20;
  std::vector<std::string> positional;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (arg == "--out" && command == "extract") {
      if (++i == args.size() || args[i].empty()) return usage_error("--out needs a directory");
      out_dir = args[i];
    } else if (arg == "--code" && command == "extract") {
      with_code = true;
    } else if (arg == "--top" && command == "survey") {
      if (++i == args.size()) return usage_error("--top needs a number");
      char* end = nullptr;
      const unsigned long value = std::strtoul(args[i].c_str(), &end, 10);
      if (end == args[i].c_str() || *end != '\0' || value == 0 || value > 100000)
        return usage_error("--top needs a positive number");
      top = value;
    } else if (arg.size() > 1 && arg[0] == '-') {
      return usage_error(("unknown option for '" + command + "': " + arg).c_str());
    } else {
      positional.push_back(arg);
    }
  }
  if (positional.empty()) return usage_error("no ROM file given");
  if (command != "extract" && positional.size() > 1)
    return usage_error(("'" + command + "' takes exactly one ROM file").c_str());

  const std::filesystem::path rom_path = positional[0];
  if (command == "validate") return cmd_validate(rom_path);
  if (command == "info") return cmd_info(rom_path);
  if (command == "list") return cmd_list(rom_path);
  if (command == "survey") return cmd_survey(rom_path, top);
  return cmd_extract(rom_path, out_dir, with_code,
                     std::vector<std::string>(positional.begin() + 1, positional.end()));
}
