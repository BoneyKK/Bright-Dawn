// The stand-in for the second processor: how it receives words, and the
// cartridge service.
#include <cstdint>
#include <string>
#include <vector>

#include "nds/arm9_bus.h"
#include "nds/arm9_io.h"
#include "nds/interrupts.h"
#include "nds/ipc.h"
#include "nds/second_processor.h"
#include "test.h"

namespace {

using bd::nds::SecondProcessor;

class NoIo : public bd::nds::Io {
 public:
  std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
  void write(std::uint32_t, unsigned, std::uint32_t) override {}
};

constexpr std::uint32_t kChipId = 0x00001FC2;
constexpr std::uint32_t kBlock = 0x02107060;  // Where a program might keep its shared block.

struct Fixture {
  NoIo no_io;
  bd::nds::Arm9Bus bus{no_io};
  bd::nds::Arm9Io io;
  bd::nds::Interrupts irq;
  bd::nds::SaveMemory save;
  SecondProcessor second{bus, kChipId, save};
  bd::nds::Ipc ipc{irq, second};
  std::vector<std::pair<std::uint32_t, std::size_t>> written;

  Fixture() {
    ipc.attach(io);
    second.connect(ipc);
    second.on_memory_written = [this](std::uint32_t a, std::size_t n) { written.emplace_back(a, n); };
    io.write(0x04000184, 2, 0x8000);  // The program turns the queues on.
  }
  void send(std::uint32_t word) { io.write(0x04000188, 4, word); }
  std::uint32_t receive() { return io.read(0x04100000, 4); }
  // A block that allows `allowed` and holds something else everywhere else.
  void prepare_block(std::uint32_t allowed) {
    for (std::uint32_t i = 0; i < 0x60; i += 4) bus.write32(kBlock + i, 0xEEEEEEEE);
    bus.write32(kBlock + 0x58, allowed);
  }
  // The block as the game fills it in for its save memory (seen in a run of
  // the real program), with everything initialised and the answer read.
  void initialise_for_save() {
    initialise(0x2FFF);
    (void)receive();
    bus.write32(kBlock + 0x04, 0x00001202);  // Serial flash, 2^18 bytes.
    bus.write32(kBlock + 0x18, 0x00040000);  // Size,
    bus.write32(kBlock + 0x1C, 0x00010000);  // sector,
    bus.write32(kBlock + 0x20, 0);
    bus.write32(kBlock + 0x24, 0x00000100);  // page,
    bus.write32(kBlock + 0x28, 3);           // bytes in an address.
    bus.write32(kBlock + 0x54, 0x000000FF);  // Leave the status register alone.
    bus.write32(kBlock, 0xEEEEEEEE);
    written.clear();
  }
  // Sends a request with its three arguments and returns the result, having
  // checked that exactly one word came back.
  std::uint32_t request(std::uint32_t number, std::uint32_t source, std::uint32_t destination, std::uint32_t length) {
    bus.write32(kBlock + 0x0C, source);
    bus.write32(kBlock + 0x10, destination);
    bus.write32(kBlock + 0x14, length);
    bus.write32(kBlock, 0xEEEEEEEE);
    send(SecondProcessor::word(11, true, number));
    if (ipc.waiting_words() != 1 || receive() != 0x0000006B) return 0xDEAD;
    return bus.read32(kBlock);
  }
  void fill(std::uint32_t address, std::uint32_t size, std::uint8_t first) {
    for (std::uint32_t i = 0; i < size; ++i) bus.write8(address + i, static_cast<std::uint8_t>(first + i));
  }
  void initialise(std::uint32_t allowed = 0xFFFF) {
    prepare_block(allowed);
    send(SecondProcessor::word(11, true, 0));
    send(SecondProcessor::word(11, true, kBlock));
  }
};

}  // namespace

TEST(second_processor_words_are_put_together_as_the_programs_do) {
  // The two words the game's main program sends first to service 11, for a
  // block at 0x02107060, and the answer its second-processor program gives.
  CHECK(SecondProcessor::word(11, true, 0) == 0x0000002B);
  CHECK(SecondProcessor::word(11, true, 0x02107060) == 0x841C182B);
  CHECK(SecondProcessor::word(11, true, 1) == 0x0000006B);
  CHECK(SecondProcessor::word(13, false, 1) == 0x0000004D);
}

TEST(second_processor_initialises_the_cartridge_service) {
  Fixture f;
  f.prepare_block(0xFFFF);
  f.send(0x0000002B);
  // The request number alone does nothing: the block's address is still to come.
  CHECK(f.ipc.waiting_words() == 0 && f.written.empty());
  f.send(0x841C182B);
  CHECK(f.bus.read32(kBlock) == 0);                    // Done.
  CHECK(f.bus.read32(kBlock + 4) == 0xEEEEEEEE);       // Nothing else is touched.
  CHECK(f.bus.read32(kBlock + 8) == 0xEEEEEEEE);
  CHECK(f.ipc.waiting_words() == 1);
  CHECK(f.receive() == 0x0000006B);
  CHECK(f.ipc.waiting_words() == 0);
  CHECK(f.second.words_answered() == 1);
  CHECK(f.second.not_carried_out().empty() && f.second.unanswered_by_service().empty());
  CHECK(f.written.size() == 1 && f.written[0].first == kBlock && f.written[0].second == 4);
  // The answer raises the "word received" interrupt when the program asked for it.
  CHECK(f.irq.requested() == 0);
  f.io.write(0x04000184, 2, 0x8400);
  f.send(SecondProcessor::word(11, true, 3));
  CHECK((f.irq.requested() & (1u << bd::nds::Interrupts::kIpcReceive)) != 0);
}

TEST(second_processor_refuses_a_request_the_block_does_not_allow) {
  Fixture f;
  f.initialise(0xFFFE);  // Everything but "initialise" itself.
  CHECK(f.bus.read32(kBlock) == 3);
  CHECK(f.receive() == 0x0000006B);  // Refused, and said so.
  f.bus.write32(kBlock + 0x58, 0xFFF7);
  f.send(SecondProcessor::word(11, true, 3));
  CHECK(f.bus.read32(kBlock) == 3 && f.bus.read32(kBlock + 8) == 0xEEEEEEEE);
  CHECK(f.receive() == 0x0000006B);
}

TEST(second_processor_reads_the_chip_identifier) {
  Fixture f;
  f.initialise();
  CHECK(f.receive() == 0x0000006B);
  f.bus.write32(kBlock, 0xEEEEEEEE);
  f.send(SecondProcessor::word(11, true, 3));
  CHECK(f.bus.read32(kBlock) == 0);
  CHECK(f.bus.read32(kBlock + 8) == kChipId);
  CHECK(f.bus.read32(kBlock + 4) == 0xEEEEEEEE && f.bus.read32(kBlock + 0xC) == 0xEEEEEEEE);
  CHECK(f.receive() == 0x0000006B);
}

TEST(second_processor_refuses_the_requests_the_program_refuses) {
  for (const std::uint32_t request : {4u, 5u, 10u}) {
    Fixture f;
    f.initialise();
    CHECK(f.receive() == 0x0000006B);
    f.send(SecondProcessor::word(11, true, request));
    CHECK(f.bus.read32(kBlock) == 3);
    CHECK(f.receive() == 0x0000006B);
  }
}

TEST(second_processor_records_cartridge_requests_it_cannot_carry_out) {
  // Requests this game's kind of save memory does not take, when a block
  // allows them all the same.
  for (const std::uint32_t request : {12u, 14u, 15u}) {
    Fixture f;
    f.initialise_for_save();
    f.bus.write32(kBlock + 0x58, 0xFFFF);
    f.send(SecondProcessor::word(11, true, request));
    CHECK(f.ipc.waiting_words() == 0);             // No answer.
    CHECK(f.bus.read32(kBlock) == 0xEEEEEEEE);     // No result.
    CHECK(f.second.not_carried_out().size() == 1);
    const std::string what = f.second.not_carried_out().begin()->first;
    CHECK(what.find("request " + std::to_string(request)) != std::string::npos);
    CHECK(what.find("not implemented") != std::string::npos);
  }
  // As the game's block has it, they are refused like anything not allowed.
  for (const std::uint32_t request : {12u, 14u, 15u}) {
    Fixture f;
    f.initialise_for_save();
    CHECK(f.request(request, 0, 0, 0) == 3);
  }
  // Requests the second-processor program itself stops on.
  for (const std::uint32_t request : {1u, 16u, 1000u}) {
    Fixture f;
    f.initialise();
    CHECK(f.receive() == 0x0000006B);
    f.send(SecondProcessor::word(11, true, request));
    CHECK(f.ipc.waiting_words() == 0);
    CHECK(f.second.not_carried_out().size() == 1);
    CHECK(f.second.not_carried_out().begin()->first.find("stops") != std::string::npos);
  }
}

TEST(second_processor_prepares_the_save_memory) {
  Fixture f;
  f.initialise_for_save();
  CHECK(f.request(2, 0, 0, 0) == 0);  // Status register to be left alone: nothing to do.
  CHECK(f.save.statistics().bytes_read == 0 && !f.save.changed());
  // A status the chip already has is fine too; one it does not have would
  // mean writing the register, which is not there.
  f.bus.write32(kBlock + 0x54, 0x00);
  CHECK(f.request(2, 0, 0, 0) == 0);
  Fixture g;
  g.initialise_for_save();
  g.bus.write32(kBlock + 0x54, 0x02);
  g.send(SecondProcessor::word(11, true, 2));
  CHECK(g.ipc.waiting_words() == 0 && g.second.not_carried_out().size() == 1);
}

TEST(second_processor_reads_the_save_memory) {
  Fixture f;
  f.initialise_for_save();
  const std::uint8_t data[] = {0x10, 0x20, 0x30, 0x40};
  f.save.page_write(0x12345, data, 4);
  f.fill(0x02200000, 16, 0x80);
  CHECK(f.request(6, 0x12344, 0x02200004, 6) == 0);
  CHECK(f.bus.read8(0x02200003) == 0x83);  // Before: untouched.
  CHECK(f.bus.read8(0x02200004) == 0xFF && f.bus.read8(0x02200005) == 0x10 && f.bus.read8(0x02200008) == 0x40);
  CHECK(f.bus.read8(0x02200009) == 0xFF);
  CHECK(f.bus.read8(0x0220000A) == 0x8A);  // After: untouched.
  // Told about the memory it changed: the bytes read, then the result.
  CHECK(f.written.size() == 2 && f.written[0].first == 0x02200004 && f.written[0].second == 6);
  // Nothing to read is still a request that is answered.
  CHECK(f.request(6, 0, 0x02200000, 0) == 0);
  CHECK(f.bus.read8(0x02200000) == 0x80);
}

TEST(second_processor_writes_the_save_memory_page_by_page) {
  Fixture f;
  f.initialise_for_save();
  f.fill(0x02200000, 0x300, 1);
  // 0x220 bytes from 0x1F0: the end of one page, a whole page, the start of a third.
  CHECK(f.request(7, 0x02200000, 0x000001F0, 0x220) == 0);
  bool same = true;
  for (std::uint32_t i = 0; i < 0x220; ++i) same = same && f.save.read(0x1F0 + i) == static_cast<std::uint8_t>(1 + i);
  CHECK(same);
  CHECK(f.save.read(0x1EF) == 0xFF && f.save.read(0x410) == 0xFF);  // Nothing wrapped round inside a page.
  CHECK(f.save.read(0x100) == 0xFF && f.save.read(0x200 - 0x11) == 0xFF);
  CHECK(f.save.statistics().bytes_written == 0x220);
  // Writing replaces: over the same place with other bytes.
  f.fill(0x02200000, 4, 0xF0);
  CHECK(f.request(7, 0x02200000, 0x000001F0, 4) == 0);
  CHECK(f.save.read(0x1F0) == 0xF0 && f.save.read(0x1F3) == 0xF3 && f.save.read(0x1F4) == 5);
}

TEST(second_processor_programs_the_save_memory) {
  Fixture f;
  f.initialise_for_save();
  f.bus.write8(0x02200000, 0xF0);
  f.bus.write8(0x02200001, 0x0F);
  CHECK(f.request(8, 0x02200000, 0x00030000, 1) == 0);
  CHECK(f.save.read(0x30000) == 0xF0);
  CHECK(f.request(8, 0x02200001, 0x00030000, 1) == 0);
  CHECK(f.save.read(0x30000) == 0x00);  // Programming only clears bits.
  // Across a page boundary it is split like a write.
  f.fill(0x02200000, 8, 0x40);
  CHECK(f.request(8, 0x02200000, 0x000300FC, 8) == 0);
  CHECK(f.save.read(0x300FC) == 0x40 && f.save.read(0x30103) == 0x47 && f.save.read(0x30001) == 0xFF);
}

TEST(second_processor_verifies_the_save_memory) {
  Fixture f;
  f.initialise_for_save();
  f.fill(0x02200000, 0x180, 3);
  CHECK(f.request(7, 0x02200000, 0x00008080, 0x180) == 0);
  const std::uint64_t written_before = f.save.statistics().bytes_written;
  CHECK(f.request(9, 0x02200000, 0x00008080, 0x180) == 0);  // The same.
  f.bus.write8(0x0220017F, 0x00);                            // The last byte differs.
  CHECK(f.request(9, 0x02200000, 0x00008080, 0x180) == 1);
  CHECK(f.request(9, 0x02200000, 0x00008080, 0x17F) == 0);  // Not if it is left out.
  CHECK(f.request(9, 0x02200000, 0x00008080, 0) == 0);
  CHECK(f.save.statistics().bytes_written == written_before);  // Verifying writes nothing,
  CHECK(f.save.read(0x8080) == 3 && f.save.read(0x81FF) == static_cast<std::uint8_t>(3 + 0x17F));
  CHECK(f.bus.read8(0x02200000) == 3);                         // on either side.
}

TEST(second_processor_erases_sectors_of_the_save_memory) {
  Fixture f;
  f.initialise_for_save();
  f.fill(0x02200000, 4, 0);
  for (const std::uint32_t at : {0x0FFFCu, 0x10000u, 0x2FFFCu, 0x30000u}) CHECK(f.request(7, 0x02200000, at, 4) == 0);
  CHECK(f.request(11, 0, 0x00010000, 0x00020000) == 0);  // The second and third sectors.
  CHECK(f.save.read(0x0FFFC) == 0x00);
  CHECK(f.save.read(0x10000) == 0xFF && f.save.read(0x2FFFC) == 0xFF);
  CHECK(f.save.read(0x30001) == 0x01);
  CHECK(f.save.statistics().sectors_erased == 2);
  // Not on sector boundaries: refused with its own result, nothing erased.
  CHECK(f.request(11, 0, 0x00030100, 0x00010000) == 2);
  CHECK(f.request(11, 0, 0x00030000, 0x00008000) == 2);
  CHECK(f.save.read(0x30001) == 0x01 && f.save.statistics().sectors_erased == 2);
  CHECK(f.request(11, 0, 0x00030000, 0) == 0);  // Nothing to erase.
  CHECK(f.save.read(0x30001) == 0x01);
}

TEST(second_processor_reads_the_save_memory_status) {
  Fixture f;
  f.initialise_for_save();
  f.bus.write32(0x02200000, 0xEEEEEEEE);
  CHECK(f.request(13, 0, 0x02200001, 0) == 0);
  CHECK(f.bus.read32(0x02200000) == 0xEEEE00EE);  // One byte: not busy.
}

TEST(second_processor_turns_away_save_requests_it_cannot_do_faithfully) {
  // Another kind of save memory than this game's.
  for (const auto& [offset, value] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
           {0x04, 0x00001302}, {0x04, 0x00001201}, {0x04, 0x00FF1202}, {0x18, 0x00080000}, {0x28, 2}}) {
    Fixture f;
    f.initialise_for_save();
    f.bus.write32(kBlock + offset, value);
    f.send(SecondProcessor::word(11, true, 6));
    CHECK(f.ipc.waiting_words() == 0 && f.second.not_carried_out().size() == 1);
    CHECK(f.second.not_carried_out().begin()->first.find("other than this game's") != std::string::npos);
  }
  // Memory that is not main memory, or runs off its end.
  const std::vector<std::vector<std::uint32_t>> cases = {
      {6, 0, 0x03000000, 4}, {6, 0, 0x02FFFFFE, 4}, {6, 0, 0x01FFFFFF, 2}, {7, 0x04000000, 0, 4},
      {8, 0x02FFFFFF, 0, 2}, {9, 0x06000000, 0, 1}, {13, 0, 0x03000000, 0}};
  for (const auto& c : cases) {
    Fixture f;
    f.initialise_for_save();
    f.bus.write32(kBlock + 0x0C, c[1]);
    f.bus.write32(kBlock + 0x10, c[2]);
    f.bus.write32(kBlock + 0x14, c[3]);
    f.send(SecondProcessor::word(11, true, c[0]));
    CHECK(f.ipc.waiting_words() == 0 && f.second.not_carried_out().size() == 1);
    CHECK(f.second.not_carried_out().begin()->first.find("main memory") != std::string::npos);
    CHECK(!f.save.changed() && f.bus.read32(kBlock) == 0xEEEEEEEE);
  }
  // Up to the very end of main memory is fine.
  Fixture f;
  f.initialise_for_save();
  CHECK(f.request(6, 0, 0x02FFFFFC, 4) == 0);
  CHECK(f.bus.read32(0x02FFFFFC) == 0xFFFFFFFF);
  // A page size that is not a power of two.
  f.bus.write32(kBlock + 0x24, 0x180);
  f.send(SecondProcessor::word(11, true, 7));
  CHECK(f.ipc.waiting_words() == 0 && f.second.not_carried_out().size() == 1);
}

TEST(second_processor_cartridge_service_ignores_words_without_the_flag) {
  Fixture f;
  f.prepare_block(0xFFFF);
  f.send(SecondProcessor::word(11, false, 0));
  f.send(SecondProcessor::word(11, false, kBlock));
  CHECK(f.ipc.waiting_words() == 0 && f.written.empty());
  // They did not count as the start of the conversation either.
  f.send(SecondProcessor::word(11, true, 0));
  f.send(SecondProcessor::word(11, true, kBlock));
  CHECK(f.bus.read32(kBlock) == 0);
  CHECK(f.receive() == 0x0000006B);
}

TEST(second_processor_does_not_follow_a_block_address_outside_main_memory) {
  Fixture f;
  f.send(SecondProcessor::word(11, true, 0));
  f.send(SecondProcessor::word(11, true, 0x03000100));
  CHECK(f.ipc.waiting_words() == 0 && f.written.empty());
  CHECK(f.second.not_carried_out().size() == 1);
  CHECK(f.second.not_carried_out().begin()->first.find("not in main memory") != std::string::npos);
}

TEST(second_processor_sees_main_memory_not_the_fast_memories) {
  // The main processor can have its fast data memory in front of main
  // memory. The second processor has no such thing.
  Fixture f;
  const std::uint32_t block = 0x027E0100;
  f.bus.write32(block, 0xEEEEEEEE);  // Into main memory, before the fast memory is there.
  f.bus.write32(block + 0x58, 0xFFFF);
  f.bus.map_dtcm(true, true, 0x027E0000, 0x4000);
  f.bus.write32(block, 0x11111111);  // Into the fast memory.
  f.bus.write32(block + 0x58, 0);
  f.send(SecondProcessor::word(11, true, 0));
  f.send(SecondProcessor::word(11, true, block));
  CHECK(f.receive() == 0x0000006B);
  CHECK(f.bus.read32(block) == 0x11111111);     // The fast memory is untouched,
  CHECK(f.bus.main_read(block, 4) == 0);        // and main memory has the result.
}

TEST(second_processor_returns_words_for_services_it_does_not_have) {
  Fixture f;
  // Service 15 has no handler in the game's second-processor program.
  f.send(SecondProcessor::word(15, false, 0x1234));
  CHECK(f.receive() == SecondProcessor::word(15, true, 0x1234));
  f.send(SecondProcessor::word(15, true, 0x1234));  // Already marked: dropped.
  CHECK(f.ipc.waiting_words() == 0);
  f.send(SecondProcessor::word(0, false, 0x99));    // Service 0: dropped.
  f.send(SecondProcessor::word(0, true, 0x99));
  CHECK(f.ipc.waiting_words() == 0);
  f.send(SecondProcessor::word(1, false, 7));
  CHECK(f.receive() == SecondProcessor::word(1, true, 7));
  CHECK(f.second.unanswered_by_service().empty() && f.second.not_carried_out().empty());
  CHECK(f.second.words_by_service().at(15) == 2 && f.second.words_by_service().at(0) == 2);
}

TEST(second_processor_samples_an_untouched_screen) {
  Fixture f;
  f.bus.write32(0x027FFFA8, 0xEEEEEEEE);
  f.bus.write32(0x027FFFAC, 0xEEEEEEEE);
  // The word the game's main program sends: one piece, first and last,
  // request 0. And the answer it waits for.
  CHECK(SecondProcessor::word(6, false, 0x03000000) == 0xC0000006);
  f.send(0xC0000006);
  CHECK(f.receive() == 0xC0200006);
  CHECK(SecondProcessor::group_answer(0, 0) == 0xC0200006);
  // The sample: nothing touched, neither reading usable.
  CHECK(f.bus.read16(0x027FFFAA) == 0x0000 && f.bus.read16(0x027FFFAC) == 0x0600);
  CHECK(f.bus.read16(0x027FFFA8) == 0xEEEE && f.bus.read16(0x027FFFAE) == 0xEEEE);
  CHECK(f.bus.read16(0x02FFFFAC) == 0x0600);  // Where the game reads it.
  CHECK(f.second.unanswered_by_service().empty() && f.second.not_carried_out().empty());
  CHECK(f.written.size() == 2);
}

TEST(second_processor_samples_a_touched_screen) {
  Fixture f;
  f.second.set_touch(true, 100, 50);
  f.send(0xC0000006);
  CHECK(f.receive() == 0xC0200006);
  // Readings are the pixel times 16: 0x640 across, 0x320 down; touched; both usable.
  const std::uint32_t sample = f.bus.read16(0x027FFFAA) | (static_cast<std::uint32_t>(f.bus.read16(0x027FFFAC)) << 16);
  CHECK((sample & 0xFFF) == 0x640);
  CHECK(((sample >> 12) & 0xFFF) == 0x320);
  CHECK(((sample >> 24) & 1) == 1);
  CHECK(((sample >> 25) & 3) == 0);
  CHECK((sample >> 27) == 0);
  // The corners, and positions off the screen, which are brought back onto it.
  f.second.set_touch(true, 255, 191);
  f.send(0xC0000006);
  CHECK(f.receive() == 0xC0200006);
  CHECK(f.bus.read16(0x027FFFAA) == (0xFF0 | ((0xBF0 & 0xF) << 12)) && f.bus.read16(0x027FFFAC) == (0x0100 | (0xBF0 >> 4)));
  f.second.set_touch(true, 9999, 9999);
  f.send(0xC0000006);
  CHECK(f.receive() == 0xC0200006);
  CHECK(f.bus.read16(0x027FFFAA) == (0xFF0 | ((0xBF0 & 0xF) << 12)) && f.bus.read16(0x027FFFAC) == (0x0100 | (0xBF0 >> 4)));
  f.second.set_touch(true, 0, 0);
  f.send(0xC0000006);
  CHECK(f.receive() == 0xC0200006);
  CHECK(f.bus.read16(0x027FFFAA) == 0 && f.bus.read16(0x027FFFAC) == 0x0100);
  // Lifted again.
  f.second.set_touch(false, 100, 50);
  f.send(0xC0000006);
  CHECK(f.receive() == 0xC0200006);
  CHECK(f.bus.read16(0x027FFFAA) == 0 && f.bus.read16(0x027FFFAC) == 0x0600);
}

TEST(second_processor_touch_requests_are_built_from_pieces) {
  Fixture f;
  // Request 3 (how far apart readings may lie) with its argument in the low
  // byte of the only piece.
  f.send(SecondProcessor::word(6, false, 0x03000000 | 0x0314));
  CHECK(f.receive() == SecondProcessor::group_answer(3, 0));
  f.send(SecondProcessor::word(6, false, 0x03000000 | 0x0300));  // Zero is refused.
  CHECK(f.receive() == SecondProcessor::group_answer(3, 2));
  // Two pieces: nothing happens until the last one arrives.
  f.send(SecondProcessor::word(6, false, 0x02000000 | 0x0105));  // Request 1, five samples a frame: too many.
  CHECK(f.ipc.waiting_words() == 0);
  f.send(SecondProcessor::word(6, false, 0x01010000 | 0x0000));
  CHECK(f.receive() == SecondProcessor::group_answer(1, 2));
  f.send(SecondProcessor::word(6, false, 0x02000000 | 0x0102));
  f.send(SecondProcessor::word(6, false, 0x01010000 | 263));     // A display line that does not exist.
  CHECK(f.receive() == SecondProcessor::group_answer(1, 2));
  // A valid request for regular sampling is not implemented: recorded, not answered.
  f.send(SecondProcessor::word(6, false, 0x02000000 | 0x0102));
  f.send(SecondProcessor::word(6, false, 0x01010000 | 100));
  CHECK(f.ipc.waiting_words() == 0 && f.second.not_carried_out().size() == 1);
  // Stopping what never started is the wrong state.
  f.send(SecondProcessor::word(6, false, 0x03000000 | 0x0200));
  CHECK(f.receive() == SecondProcessor::group_answer(2, 3));
  // A first piece clears what an abandoned request left behind.
  f.send(SecondProcessor::word(6, false, 0x02000000 | 0x0314));  // Never finished.
  f.send(SecondProcessor::word(6, false, 0x03010000 | 0x0000));  // A new request: piece 1 only, so piece 0 is zero,
  CHECK(f.receive() == 0xC0200006);                              // which is "sample once".
}

TEST(second_processor_answers_unknown_touch_requests_as_the_program_does) {
  Fixture f;
  f.send(SecondProcessor::word(6, false, 0x03000000 | 0x0400));
  CHECK(f.receive() == SecondProcessor::group_answer(4, 1));
  // The answer goes out on the service the request's number belongs to,
  // even though it came in on the touch screen's.
  f.send(SecondProcessor::word(6, false, 0x03000000 | 0x4500));
  const std::uint32_t answer = f.receive();
  CHECK((answer & 0x1F) == 9);
  CHECK(answer == SecondProcessor::word(9, false, 0x03000000 | 0xC500 | 1));
  CHECK((SecondProcessor::group_answer(0x25, 1) & 0x1F) == 4 && (SecondProcessor::group_answer(0x7F, 1) & 0x1F) == 8);
  CHECK((SecondProcessor::group_answer(0x1F, 1) & 0x1F) == 6 && (SecondProcessor::group_answer(0xE0, 1) & 0x1F) == 8);
  // Words with the flag set are not for this handler.
  f.send(SecondProcessor::word(6, true, 0x03000000));
  CHECK(f.ipc.waiting_words() == 0);
}

TEST(second_processor_finds_the_second_slot_empty) {
  Fixture f;
  f.bus.write8(SecondProcessor::kSecondSlotFlags, 0xFF);
  f.bus.write8(SecondProcessor::kSecondSlotFlags - 1, 0xEE);
  f.bus.write8(SecondProcessor::kSecondSlotFlags + 1, 0xEE);
  // The word the game's main program sends: "look at the second slot".
  f.send(0x0000004D);
  // No cartridge of that kind: bit 0 of the flags is cleared, the rest left.
  CHECK(f.bus.read8(SecondProcessor::kSecondSlotFlags) == 0xFE);
  CHECK(f.bus.read8(SecondProcessor::kSecondSlotFlags - 1) == 0xEE);
  CHECK(f.bus.read8(SecondProcessor::kSecondSlotFlags + 1) == 0xEE);
  CHECK(f.written.size() == 1 && f.written[0].first == SecondProcessor::kSecondSlotFlags && f.written[0].second == 1);
  // The request's number comes back.
  CHECK(f.receive() == 0x0000004D);
  CHECK(f.ipc.waiting_words() == 0);
  CHECK(f.second.unanswered_by_service().empty() && f.second.not_carried_out().empty());

  // From then on the service takes only the notice that a cartridge was
  // pulled out, on which the program shuts down. Neither that nor any other
  // word is acted on or answered.
  f.send(SecondProcessor::word(13, false, 2));
  f.send(SecondProcessor::word(13, true, 1));
  CHECK(f.ipc.waiting_words() == 0);
  CHECK(f.second.not_carried_out().size() == 2);
  CHECK(f.bus.read8(SecondProcessor::kSecondSlotFlags) == 0xFE);
}

TEST(second_processor_takes_only_the_check_as_the_second_slot_s_first_word) {
  Fixture f;
  f.send(SecondProcessor::word(13, false, 2));
  CHECK(f.ipc.waiting_words() == 0 && f.written.empty());
  CHECK(f.second.not_carried_out().size() == 1);
  // The check itself still works afterwards here (the program would have
  // stopped), and bits above the low six do not change what the request is.
  f.send(SecondProcessor::word(13, false, 0x02000041));
  CHECK(f.receive() == 0x0000004D);
}

TEST(second_processor_notes_a_request_to_reset) {
  // The word the game sends when L, R, Start and Select are held together
  // (seen in a run): service 12, request 0x10 in bits 8 to 14 of the data.
  Fixture f;
  f.send(SecondProcessor::word(12, false, 0x0800));  // Another request: not a reset.
  CHECK(!f.second.reset_asked());
  f.send(0x00040000 | 12);
  CHECK(f.second.reset_asked());
  CHECK(SecondProcessor::word(12, false, SecondProcessor::kSystemReset << 8) == 0x0004000C);
  CHECK(f.ipc.waiting_words() == 0);  // Not answered: the console is started anew.
}
