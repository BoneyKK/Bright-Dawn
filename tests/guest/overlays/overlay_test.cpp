// Overlays: several pieces of code translated for the same address.
//
// The test program (main.S, ovl_a.S, ovl_b.S, ovl_c.S) is translated as a whole
// game would be. This harness plays the part of the game's loader: it copies
// one overlay or the other into the shared slot and calls the program. The
// runtime is never told which overlay is there. It must use the translation
// whose code is in memory, and interpret when memory matches no translation.
//
// usage: guest_overlay_test <code directory>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include "guest_bus.h"
#include "guest_runner.h"
#include "machine.h"
#include "translated_table.h"

extern "C" {
#include "inputs.h"
}

namespace {

using u32 = std::uint32_t;
using Bytes = std::vector<std::uint8_t>;

constexpr u32 kMain = 0x02000000, kSlot = 0x02010000, kFixed = 0x02020000;
constexpr u32 kSlotSize = 0x100;       // Cleared before each load.
constexpr u32 kSecondA = 0x0201001c;   // Overlay A's exported function.
constexpr u32 kSecondB = 0x02010014;   // Overlay B's.
constexpr u32 kPatchOffset = 0x24;     // The "+ 0x11" in overlay A's exported function.

// What the program computes, from the assembly sources.
u32 helper(u32 x) { return x * 5 + 9; }
u32 entry_a(u32 a, u32 b) { return a + 3 * b + helper(b); }
u32 second_a(u32 a, u32 b, u32 addend) { return (a ^ b) + addend; }
u32 entry_b(u32 a, u32 b) { return 5 * a - b; }
u32 second_b(u32 a, u32 b) { return helper(a + b + 1); }
u32 fixed_c(u32 a, u32 b) { return (a - b) + helper(a); }

bool read_file(const std::string& path, Bytes& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

int g_failures = 0;
void check(bool condition, const char* phase, const char* what) {
  if (condition) return;
  ++g_failures;
  std::printf("  FAIL [%s] %s\n", phase, what);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: guest_overlay_test <code directory>\n");
    return 2;
  }
  const std::string dir = argv[1];
  Bytes main_image, overlay_a, overlay_b, overlay_c;
  if (!read_file(dir + "/arm9.bin", main_image) || !read_file(dir + "/overlay9_0000.bin", overlay_a) ||
      !read_file(dir + "/overlay9_0001.bin", overlay_b) || !read_file(dir + "/overlay9_0002.bin", overlay_c)) {
    std::printf("cannot read the program files in %s\n", dir.c_str());
    return 2;
  }
  if (overlay_a.size() <= kPatchOffset || overlay_a[kPatchOffset] != 0x11) {
    std::printf("overlay A is not laid out as this test expects\n");
    return 2;
  }

  guest::Memory memory;
  guest::Bus bus(memory);
  guest::System system;
  bd::runtime::Machine machine(bus, system, translated_table);
  machine.set_instruction_limit(100000);
  machine.add_overlay(0, kSlot, overlay_a.data(), overlay_a.size());
  machine.add_overlay(1, kSlot, overlay_b.data(), overlay_b.size());
  machine.add_overlay(2, kFixed, overlay_c.data(), overlay_c.size());

  // The loader's part: put bytes into guest memory and say that it happened.
  auto place = [&](u32 address, const Bytes& image, u32 clear) {
    std::memset(memory.at(address, clear), 0, clear);
    std::memcpy(memory.at(address, static_cast<u32>(image.size())), image.data(), image.size());
    machine.note_memory_written(address, clear);
  };
  auto run = [&](u32 a, u32 b) {
    machine.reset_cpu();
    machine.cpu().r[0] = a;
    machine.cpu().r[1] = b;
    machine.cpu().r[13] = guest::kStackTop;
    machine.call_function(kMain, guest::kReturnAddress);
    return machine.cpu().r[0];
  };
  // Runs every input and compares with `expected`; returns the number wrong.
  auto run_all = [&](const char* phase, const std::function<u32(u32, u32)>& expected, int count) {
    guest_rng rng = guest_rng_seed();
    long wrong = 0;
    for (int i = 0; i < count; ++i) {
      u32 a, b;
      guest_input(&rng, i, &a, &b);
      if (run(a, b) != expected(a, b)) ++wrong;
    }
    check(wrong == 0, phase, "wrong results");
    check(system.problem.empty(), phase, system.problem.c_str());
    check(!memory.faulted(), phase, "touched memory outside the guest RAM");
    system.problem.clear();
    memory.clear_fault();
  };
  const std::vector<std::uint8_t> blank;
  place(kMain, main_image, static_cast<u32>(main_image.size()));
  place(kFixed, overlay_c, static_cast<u32>(overlay_c.size()));

  // 1. Overlay A in the slot.
  place(kSlot, overlay_a, kSlotSize);
  machine.clear_statistics();
  check(machine.is_translated(kSlot), "A", "the slot's entry has no usable translation");
  check(machine.is_translated(kSecondA), "A", "A's exported function has no usable translation");
  check(!machine.is_translated(kSecondB), "A", "B's function is treated as present while A is loaded");
  run_all("A", [](u32 a, u32 b) { return (entry_a(a, b) ^ second_a(a, b, 0x11)) + fixed_c(a, b); }, GUEST_PAIR_COUNT);
  check(machine.statistics().interpreter_entries.empty(), "A", "translated code exists but the interpreter was used");
  // Memory did not change during the phase, so each translation is compared once at most.
  check(machine.statistics().translations_checked <= 8, "A", "translations were compared with memory again and again");
  std::printf("A loaded:        %llu comparisons with memory, %llu rejected\n",
              static_cast<unsigned long long>(machine.statistics().translations_checked),
              static_cast<unsigned long long>(machine.statistics().translations_rejected));

  // 2. Overlay B in the same slot. Nothing tells the runtime except the bytes.
  place(kSlot, overlay_b, kSlotSize);
  machine.clear_statistics();
  check(machine.is_translated(kSlot), "B", "the slot's entry has no usable translation");
  check(machine.is_translated(kSecondB), "B", "B's exported function has no usable translation");
  check(!machine.is_translated(kSecondA), "B", "A's function is treated as present while B is loaded");
  run_all("B", [](u32 a, u32 b) { return (entry_b(a, b) ^ second_b(a, b)) + fixed_c(a, b); }, GUEST_PAIR_COUNT);
  check(machine.statistics().interpreter_entries.empty(), "B", "translated code exists but the interpreter was used");
  check(machine.statistics().translations_checked <= 8, "B", "translations were compared with memory again and again");
  std::printf("B loaded:        %llu comparisons with memory, %llu rejected\n",
              static_cast<unsigned long long>(machine.statistics().translations_checked),
              static_cast<unsigned long long>(machine.statistics().translations_rejected));

  // 3. Swapping back and forth, one call each.
  machine.clear_statistics();
  {
    guest_rng rng = guest_rng_seed();
    long wrong = 0;
    for (int i = 0; i < 600; ++i) {
      u32 a, b;
      guest_input(&rng, i, &a, &b);
      const bool use_a = (i % 2) == 0;
      place(kSlot, use_a ? overlay_a : overlay_b, kSlotSize);
      const u32 want = use_a ? (entry_a(a, b) ^ second_a(a, b, 0x11)) + fixed_c(a, b)
                             : (entry_b(a, b) ^ second_b(a, b)) + fixed_c(a, b);
      if (run(a, b) != want) ++wrong;
    }
    check(wrong == 0, "swapping", "wrong results");
    check(system.problem.empty(), "swapping", system.problem.c_str());
    check(machine.statistics().interpreter_entries.empty(), "swapping", "the interpreter was used");
  }
  std::printf("swapping:        %llu comparisons with memory, %llu rejected\n",
              static_cast<unsigned long long>(machine.statistics().translations_checked),
              static_cast<unsigned long long>(machine.statistics().translations_rejected));

  // 4. Overlay A with one byte of its exported function changed. No
  // translation was made from these bytes, so that function must be
  // interpreted, and the result must follow the changed code. The rest of
  // the overlay is untouched and must still run translated.
  Bytes patched = overlay_a;
  patched[kPatchOffset] = 0x12;
  place(kSlot, patched, kSlotSize);
  machine.clear_statistics();
  check(machine.is_translated(kSlot), "patched", "the untouched entry lost its translation");
  check(!machine.is_translated(kSecondA), "patched", "a translation was accepted for code that differs from it");
  run_all("patched", [](u32 a, u32 b) { return (entry_a(a, b) ^ second_a(a, b, 0x12)) + fixed_c(a, b); }, GUEST_PAIR_COUNT);
  {
    const auto& entries = machine.statistics().interpreter_entries;
    check(entries.size() == 1 && entries.count(kSecondA) == 1, "patched",
          "expected the interpreter to be entered at the changed function and nowhere else");
    check(entries.size() == 1 && entries.begin()->second == static_cast<std::uint64_t>(GUEST_PAIR_COUNT), "patched",
          "expected one interpreter entry per call");
  }
  std::printf("patched A:       interpreter entered at %zu address(es)\n",
              machine.statistics().interpreter_entries.size());

  // 5. An overlay whose image the runtime was never given: its translations
  // must not be used. A second machine knows only overlays A and C.
  {
    bd::runtime::Machine partial(bus, system, translated_table);
    partial.set_instruction_limit(100000);
    partial.add_overlay(0, kSlot, overlay_a.data(), overlay_a.size());
    partial.add_overlay(2, kFixed, overlay_c.data(), overlay_c.size());
    place(kSlot, overlay_b, kSlotSize);
    partial.note_memory_written(kSlot, kSlotSize);
    check(!partial.is_translated(kSlot), "unknown overlay", "a translation was used for an overlay with no image");
    partial.reset_cpu();
    partial.cpu().r[0] = 1234;
    partial.cpu().r[1] = 77;
    partial.cpu().r[13] = guest::kStackTop;
    partial.call_function(kMain, guest::kReturnAddress);
    check(partial.cpu().r[0] == (entry_b(1234, 77) ^ second_b(1234, 77)) + fixed_c(1234, 77), "unknown overlay",
          "wrong result when the overlay had to be interpreted");
    check(!partial.statistics().interpreter_entries.empty(), "unknown overlay", "expected the interpreter to be used");
    check(system.problem.empty(), "unknown overlay", system.problem.c_str());
  }

  std::printf("%s\n", g_failures == 0 ? "ok" : "FAILED");
  return g_failures == 0 ? 0 : 1;
}
