#include <cstdint>
#include <vector>

#include "nds/arm9_io.h"
#include "nds/interrupts.h"
#include "nds/ipc.h"
#include "nds/second_processor.h"
#include "test.h"

namespace {

using bd::nds::Arm9Io;
using bd::nds::Interrupts;

// Records what the main processor's side sends.
class RecordingPeer : public bd::nds::IpcPeer {
 public:
  void sync_written(unsigned value) override { sync.push_back(value); }
  void interrupt_requested() override { ++interrupts; }
  void word_sent(std::uint32_t word) override { words.push_back(word); }
  std::vector<unsigned> sync;
  std::vector<std::uint32_t> words;
  int interrupts = 0;
};

}  // namespace

TEST(arm9_io_unimplemented_registers_keep_their_value_and_are_counted) {
  Arm9Io io;
  CHECK(io.read(0x04000004, 2) == 0);
  io.write(0x04000004, 2, 0xBEEF);
  CHECK(io.read(0x04000004, 2) == 0xBEEF);
  CHECK(io.read(0x04000005, 1) == 0xBE);
  const auto& use = io.unmodelled();
  CHECK(use.at(0x04000004).reads == 2 && use.at(0x04000004).writes == 1);
  CHECK(use.at(0x04000004).last_written == 0xBEEF && use.at(0x04000004).size == 2);
  CHECK(use.at(0x04000005).reads == 1);
  io.set(0x04000300, 1, 1);  // Not an access by the program.
  CHECK(use.count(0x04000300) == 0);
  CHECK(io.read(0x04000300, 1) == 1);
  CHECK(io.read(0x04FFFFFC, 4) == 0);  // Far beyond the registers: reads as zero.
}

TEST(arm9_io_implemented_registers_take_whole_and_partial_accesses) {
  Arm9Io io;
  std::uint32_t reg = 0x11223344;
  int reads = 0;
  io.add(0x04000100, 4,
         [&] {
           ++reads;
           return reg;
         },
         [&](std::uint32_t value, std::uint32_t mask) { reg = (reg & ~mask) | (value & mask); });
  CHECK(io.read(0x04000100, 4) == 0x11223344);
  CHECK(io.read(0x04000100, 2) == 0x3344);
  CHECK(io.read(0x04000102, 2) == 0x1122);
  CHECK(io.read(0x04000103, 1) == 0x11);
  CHECK(reads == 4);  // Once per access, whatever part is read.
  io.write(0x04000101, 1, 0xAB);
  CHECK(reg == 0x1122AB44);
  io.write(0x04000102, 2, 0xCDEF);
  CHECK(reg == 0xCDEFAB44);
  io.write(0x04000100, 4, 0x01020304);
  CHECK(reg == 0x01020304);
  CHECK(io.unmodelled().empty());
}

TEST(arm9_io_an_access_can_span_registers) {
  Arm9Io io;
  std::uint32_t low = 0xAAAA, high = 0xBBBB;
  io.add(0x04000200, 2, [&] { return low; }, [&](std::uint32_t v, std::uint32_t m) { low = (low & ~m) | (v & m); });
  io.add(0x04000202, 2, [&] { return high; }, [&](std::uint32_t v, std::uint32_t m) { high = (high & ~m) | (v & m); });
  CHECK(io.read(0x04000200, 4) == 0xBBBBAAAA);
  io.write(0x04000200, 4, 0x12345678);
  CHECK(low == 0x5678 && high == 0x1234);
  CHECK(io.unmodelled().empty());

  // Half implemented, half not: the access is recorded, and each half goes
  // where it belongs.
  io.write(0x04000202, 4, 0xFACE0001);
  CHECK(high == 0x0001);
  CHECK(io.unmodelled().at(0x04000202).writes == 1);
  CHECK(io.read(0x04000204, 2) == 0xFACE);
}

TEST(arm9_io_registers_without_a_side_read_as_zero_or_ignore_writes) {
  Arm9Io io;
  int writes = 0;
  io.add(0x04000188, 4, nullptr, [&](std::uint32_t, std::uint32_t) { ++writes; });
  io.add(0x04100000, 4, [] { return 0x55u; }, nullptr);
  CHECK(io.read(0x04000188, 4) == 0);
  io.write(0x04000188, 4, 1);
  CHECK(writes == 1);
  io.write(0x04100000, 4, 0x99);
  CHECK(io.read(0x04100000, 4) == 0x55);
  CHECK(io.unmodelled().empty());
}

TEST(interrupts_registers) {
  Arm9Io io;
  Interrupts irq;
  irq.attach(io);
  CHECK(!irq.pending());
  io.write(0x04000208, 2, 1);
  CHECK(irq.master() && io.read(0x04000208, 4) == 1);
  io.write(0x04000210, 4, 0x00040001);
  CHECK(io.read(0x04000210, 4) == 0x00040001);
  CHECK(!irq.pending());

  irq.request(Interrupts::kIpcReceive);
  irq.request(Interrupts::kHBlank);  // Requested, but not enabled.
  CHECK(io.read(0x04000214, 4) == 0x00040002);
  CHECK(irq.pending());
  io.write(0x04000208, 4, 0x04000000);  // Bit 0 clear: master switch off.
  CHECK(!irq.pending() && irq.any_enabled_requested());
  io.write(0x04000208, 1, 1);
  CHECK(irq.pending());

  io.write(0x04000214, 4, 0x00040000);  // Writing a one acknowledges that source only.
  CHECK(io.read(0x04000214, 4) == 0x00000002);
  CHECK(!irq.pending());
  io.write(0x04000214, 1, 0xFF);
  CHECK(irq.requested() == 0);
  CHECK(io.unmodelled().empty());
}

TEST(ipc_synchronisation_values_and_interrupt) {
  Arm9Io io;
  Interrupts irq;
  RecordingPeer peer;
  bd::nds::Ipc ipc(irq, peer);
  ipc.attach(io);

  ipc.peer_set_sync(7);
  CHECK(io.read(0x04000180, 2) == 0x0007);
  io.write(0x04000180, 2, 0x0700);
  CHECK(peer.sync.size() == 1 && peer.sync[0] == 7);
  CHECK(io.read(0x04000180, 2) == 0x0707);
  io.write(0x04000180, 2, 0x000F);  // The low bits belong to the other side.
  CHECK(io.read(0x04000180, 2) == 0x0007);
  CHECK(peer.sync.size() == 2 && peer.sync[1] == 0);

  ipc.peer_request_interrupt();  // Not enabled on this side: nothing.
  CHECK(irq.requested() == 0);
  io.write(0x04000180, 2, 0x4000);
  ipc.peer_request_interrupt();
  CHECK(irq.requested() == (1u << Interrupts::kIpcSync));
  CHECK((io.read(0x04000180, 2) & 0x4000) != 0);

  io.write(0x04000180, 2, 0x6000);  // Interrupt the other side.
  CHECK(peer.interrupts == 1);
  CHECK((io.read(0x04000180, 2) & 0x2000) == 0);  // That bit is not kept.
}

TEST(ipc_words_to_the_peer) {
  Arm9Io io;
  Interrupts irq;
  RecordingPeer peer;
  bd::nds::Ipc ipc(irq, peer);
  ipc.attach(io);

  io.write(0x04000188, 4, 0x11111111);  // The queue is off: the word goes nowhere.
  CHECK(peer.words.empty());
  io.write(0x04000184, 2, 0xC408);      // What DS programs write to start it.
  CHECK(ipc.enabled());
  CHECK(io.read(0x04000184, 2) == 0x8501);  // On, receive interrupt on, both queues empty.
  io.write(0x04000188, 4, 0x22222222);
  CHECK(peer.words.size() == 1 && peer.words[0] == 0x22222222);
  CHECK((io.read(0x04000184, 2) & 0x0003) == 0x0001);  // Taken at once: empty, not full.
  CHECK(irq.requested() == 0);

  io.write(0x04000184, 2, 0x8404);  // Ask for the "send queue empty" interrupt: it is empty.
  CHECK(irq.requested() == (1u << Interrupts::kIpcSendEmpty));
}

TEST(ipc_words_from_the_peer) {
  Arm9Io io;
  Interrupts irq;
  RecordingPeer peer;
  bd::nds::Ipc ipc(irq, peer);
  ipc.attach(io);
  irq.attach(io);
  io.write(0x04000184, 2, 0x8000);  // On, no interrupts.

  CHECK(io.read(0x04100000, 4) == 0);  // Nothing there: an error is flagged.
  CHECK((io.read(0x04000184, 2) & 0x4000) != 0);
  io.write(0x04000184, 2, 0xC000);     // Acknowledge it.
  CHECK((io.read(0x04000184, 2) & 0x4000) == 0);

  ipc.peer_send(0xA1);
  CHECK(irq.requested() == 0);  // The receive interrupt is not enabled.
  CHECK((io.read(0x04000184, 2) & 0x0300) == 0);
  io.write(0x04000184, 2, 0x8400);  // Enabling it with a word waiting requests it.
  CHECK(irq.requested() == (1u << Interrupts::kIpcReceive));
  io.write(0x04000214, 4, 0xFFFFFFFF);

  ipc.peer_send(0xA2);  // Not the first word in the queue: no new request.
  CHECK(irq.requested() == 0);
  CHECK(io.read(0x04100000, 4) == 0xA1);
  CHECK(io.read(0x04100000, 4) == 0xA2);
  CHECK((io.read(0x04000184, 2) & 0x0100) != 0);  // Empty again.
  CHECK(io.read(0x04100000, 4) == 0xA2);          // Reading on repeats the last word...
  CHECK((io.read(0x04000184, 2) & 0x4000) != 0);  // ...and flags the error.
  ipc.peer_send(0xA3);  // First word again: requested.
  CHECK(irq.requested() == (1u << Interrupts::kIpcReceive));
}

TEST(ipc_holds_sixteen_words_and_keeps_the_rest_waiting) {
  Arm9Io io;
  Interrupts irq;
  RecordingPeer peer;
  bd::nds::Ipc ipc(irq, peer);
  ipc.attach(io);
  io.write(0x04000184, 2, 0x8000);
  for (std::uint32_t i = 0; i < 20; ++i) ipc.peer_send(100 + i);
  CHECK(ipc.waiting_words() == 20);
  CHECK((io.read(0x04000184, 2) & 0x0200) != 0);  // Full.
  bool in_order = true;
  for (std::uint32_t i = 0; i < 20; ++i) in_order = in_order && io.read(0x04100000, 4) == 100 + i;
  CHECK(in_order);
  CHECK((io.read(0x04000184, 2) & 0x0300) == 0x0100);
  CHECK((io.read(0x04000184, 2) & 0x4000) == 0);  // No error on the way.
}

TEST(ipc_reading_while_off_does_not_take_the_word) {
  Arm9Io io;
  Interrupts irq;
  RecordingPeer peer;
  bd::nds::Ipc ipc(irq, peer);
  ipc.attach(io);
  ipc.peer_send(0x77);
  CHECK(io.read(0x04100000, 4) == 0x77);
  CHECK(io.read(0x04100000, 4) == 0x77);
  CHECK(ipc.waiting_words() == 1);
  io.write(0x04000184, 2, 0x8000);
  CHECK(io.read(0x04100000, 4) == 0x77);
  CHECK(ipc.waiting_words() == 0);
}

TEST(second_processor_answers_the_start_up_handshake) {
  class NoIo : public bd::nds::Io {
   public:
    std::uint32_t read(std::uint32_t, unsigned) override { return 0; }
    void write(std::uint32_t, unsigned, std::uint32_t) override {}
  } no_io;
  bd::nds::Arm9Bus bus(no_io);
  Arm9Io io;
  Interrupts irq;
  bd::nds::SecondProcessor second(bus);
  bd::nds::Ipc ipc(irq, second);
  ipc.attach(io);
  second.connect(ipc);

  // What a DS program does: echo the value it reads until it has seen more
  // than four changes and the value is 0.
  std::vector<unsigned> seen;
  for (int step = 0; step < 32 && !second.handshake_done(); ++step) {
    const unsigned value = io.read(0x04000180, 2) & 0xF;
    seen.push_back(value);
    io.write(0x04000180, 2, value << 8);
  }
  CHECK(second.handshake_done());
  CHECK((seen == std::vector<unsigned>{7, 6, 5, 4, 3, 2, 1, 0}));
  CHECK(bus.read32(bd::nds::SecondProcessor::kServicesReady) == 0x0003FFF0);

  // Afterwards the value no longer moves, and what is sent is recorded.
  io.write(0x04000180, 2, 0x0500);
  CHECK((io.read(0x04000180, 2) & 0xF) == 0);
  io.write(0x04000184, 2, 0x8000);
  io.write(0x04000188, 4, 0x00000046);
  io.write(0x04000188, 4, 0x00000106);
  io.write(0x04000188, 4, 0x0000000B);
  CHECK(second.words_by_service().at(6) == 2 && second.words_by_service().at(11) == 1);
  CHECK(second.first_words().size() == 3 && second.first_words()[1] == 0x106);
  CHECK(ipc.waiting_words() == 0);  // Nothing is answered yet.
}
