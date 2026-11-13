// The channel between the DS's two processors, as the main processor sees
// it: a 4-bit synchronisation value in each direction and a queue of 32-bit
// words in each direction.
//
// The other end is not a second processor here but an IpcPeer, which is told
// what the main processor sends and answers through this class.
#pragma once

#include <cstdint>
#include <deque>

#include "nds/arm9_io.h"
#include "nds/interrupts.h"

namespace bd::nds {

class IpcPeer {
 public:
  virtual ~IpcPeer() = default;
  // The main processor wrote its synchronisation value (0-15).
  virtual void sync_written(unsigned value) = 0;
  // The main processor asked for an interrupt on the other side.
  virtual void interrupt_requested() = 0;
  // The main processor sent a word.
  virtual void word_sent(std::uint32_t word) = 0;
};

class Ipc {
 public:
  static constexpr std::size_t kQueueSize = 16;

  Ipc(Interrupts& interrupts, IpcPeer& peer);

  // Registers: synchronisation (0x04000180), queue control (0x04000184), send
  // (0x04000188) and receive (0x04100000).
  void attach(Arm9Io& io);

  // --- For the peer -----------------------------------------------------
  // The synchronisation value the main processor reads.
  void peer_set_sync(unsigned value) { sync_in_ = value & 15u; }
  // Requests the synchronisation interrupt on the main processor, which
  // takes effect if the main processor has enabled it.
  void peer_request_interrupt();
  // Queues a word for the main processor. The hardware queue holds 16 words;
  // more than that wait here and move up as the main processor reads.
  void peer_send(std::uint32_t word);

  // --- State, for reports and tests ---------------------------------------
  unsigned sync_out() const { return sync_out_; }
  std::size_t waiting_words() const { return receive_.size() + backlog_.size(); }
  bool enabled() const { return enabled_; }

 private:
  std::uint32_t read_sync() const;
  void write_sync(std::uint32_t value, std::uint32_t mask);
  std::uint32_t read_control() const;
  void write_control(std::uint32_t value, std::uint32_t mask);
  void write_send(std::uint32_t value, std::uint32_t mask);
  std::uint32_t read_receive();

  Interrupts& interrupts_;
  IpcPeer& peer_;
  unsigned sync_in_ = 0, sync_out_ = 0;
  bool sync_interrupt_enabled_ = false;
  bool enabled_ = false, error_ = false;
  bool send_empty_interrupt_ = false, receive_interrupt_ = false;
  std::deque<std::uint32_t> receive_, backlog_;
  std::uint32_t last_received_ = 0;
};

}  // namespace bd::nds
