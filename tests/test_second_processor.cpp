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
  SecondProcessor second{bus, kChipId};
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
  // The save memory's requests.
  for (const std::uint32_t request : {2u, 6u, 7u, 8u, 9u, 11u, 12u, 13u, 14u, 15u}) {
    Fixture f;
    f.initialise();
    CHECK(f.receive() == 0x0000006B);
    f.bus.write32(kBlock, 0xEEEEEEEE);
    f.send(SecondProcessor::word(11, true, request));
    CHECK(f.ipc.waiting_words() == 0);             // No answer.
    CHECK(f.bus.read32(kBlock) == 0xEEEEEEEE);     // No result.
    CHECK(f.second.not_carried_out().size() == 1);
    const std::string what = f.second.not_carried_out().begin()->first;
    CHECK(what.find("request " + std::to_string(request)) != std::string::npos);
    CHECK(what.find("not implemented") != std::string::npos);
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
