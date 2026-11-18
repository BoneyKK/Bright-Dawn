// The save kept in a file between runs.
#include <cstdint>
#include <string>

#include "nds/save_keeper.h"
#include "nds/save_memory.h"
#include "temp_dir.h"
#include "test.h"

using bd::nds::SaveKeeper;
using bd::nds::SaveMemory;

namespace {

void run_frames(SaveKeeper& keeper, std::uint32_t frames) {
  for (std::uint32_t i = 0; i < frames; ++i) CHECK(keeper.frame());
}

void write_byte(SaveMemory& save, std::uint32_t address, std::uint8_t value) { save.page_write(address, &value, 1); }

}  // namespace

TEST(save_keeper_without_a_file_starts_with_a_new_chip) {
  bd_test::TempDir dir;
  SaveMemory save;
  SaveKeeper keeper(save, dir.path() / "saves" / "BO5E.sav");
  CHECK(keeper.load());
  CHECK(keeper.keeping());
  CHECK(save.read(0) == 0xFF);
  run_frames(keeper, 200);
  CHECK(keeper.flush());
  CHECK(keeper.writes() == 0);  // Nothing changed: no file is made.
  CHECK(!std::filesystem::exists(keeper.path()));
}

TEST(save_keeper_writes_once_the_game_is_done_writing) {
  bd_test::TempDir dir;
  SaveMemory save;
  SaveKeeper keeper(save, dir.path() / "saves" / "BO5E.sav");
  REQUIRE(keeper.load());
  write_byte(save, 0x100, 0x42);
  run_frames(keeper, SaveKeeper::kQuietFrames);
  CHECK(keeper.writes() == 0);  // Still within the quiet second after the change.
  write_byte(save, 0x101, 0x43);  // The game goes on writing: wait again.
  run_frames(keeper, SaveKeeper::kQuietFrames);
  CHECK(keeper.writes() == 0);
  run_frames(keeper, 1);
  CHECK(keeper.writes() == 1);
  run_frames(keeper, 300);
  CHECK(keeper.writes() == 1);  // Unchanged since: not written again.
  CHECK(std::filesystem::file_size(keeper.path()) == SaveMemory::kSize);
  CHECK(!std::filesystem::exists(keeper.backup_path()));  // There was no file to keep a copy of.

  SaveMemory next_run;
  SaveKeeper next_keeper(next_run, keeper.path());
  REQUIRE(next_keeper.load());
  CHECK(next_run.read(0x100) == 0x42 && next_run.read(0x101) == 0x43 && next_run.read(0x102) == 0xFF);
  CHECK(!next_run.changed());
}

TEST(save_keeper_writes_at_the_end_of_a_run) {
  bd_test::TempDir dir;
  SaveMemory save;
  SaveKeeper keeper(save, dir.path() / "BO5P.sav");
  REQUIRE(keeper.load());
  write_byte(save, 7, 0x00);
  run_frames(keeper, 3);
  CHECK(keeper.flush());
  CHECK(keeper.writes() == 1);
  CHECK(keeper.flush());
  CHECK(keeper.writes() == 1);
  CHECK(static_cast<std::uint8_t>(dir.read("BO5P.sav").at(7)) == 0x00);
  CHECK(!std::filesystem::exists(dir.path() / "BO5P.sav.new"));
}

TEST(save_keeper_keeps_a_copy_of_the_save_the_run_started_with) {
  bd_test::TempDir dir;
  std::string before(SaveMemory::kSize, '\xFF');
  before[0] = 'A';
  dir.write("BO5J.sav", before);
  SaveMemory save;
  SaveKeeper keeper(save, dir.path() / "BO5J.sav");
  REQUIRE(keeper.load());
  CHECK(save.read(0) == 'A');
  write_byte(save, 0, 'B');
  CHECK(keeper.flush());
  write_byte(save, 0, 'C');
  CHECK(keeper.flush());
  CHECK(keeper.writes() == 2);
  CHECK(dir.read("BO5J.sav")[0] == 'C');
  CHECK(dir.read("BO5J.sav.backup") == before);  // Not the save of the first write.
}

TEST(save_keeper_never_writes_over_a_file_it_cannot_use) {
  bd_test::TempDir dir;
  dir.write("BO5E.sav", "not a save");
  SaveMemory save;
  SaveKeeper keeper(save, dir.path() / "BO5E.sav");
  CHECK(!keeper.load());
  CHECK(!keeper.keeping());
  CHECK(save.read(0) == 0xFF);
  write_byte(save, 0, 0x01);
  run_frames(keeper, 200);
  CHECK(keeper.flush());
  CHECK(keeper.writes() == 0);
  CHECK(dir.read("BO5E.sav") == "not a save");
}
