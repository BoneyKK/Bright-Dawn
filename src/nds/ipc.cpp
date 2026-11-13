#include "nds/ipc.h"

namespace bd::nds {
namespace {

using u32 = std::uint32_t;

// Synchronisation register.
constexpr u32 kSyncIn = 0x000F;           // Bits 0-3: what the other side sent (read only).
constexpr unsigned kSyncOutShift = 8;     // Bits 8-11: what this side sends.
constexpr u32 kSyncSendInterrupt = 1u << 13;    // Write one: interrupt the other side.
constexpr u32 kSyncInterruptEnable = 1u << 14;  // Accept the other side's interrupt.

// Queue control register.
constexpr u32 kSendEmpty = 1u << 0;
constexpr u32 kSendFull = 1u << 1;
constexpr u32 kSendEmptyInterrupt = 1u << 2;
constexpr u32 kSendClear = 1u << 3;       // Write one: empty the send queue.
constexpr u32 kReceiveEmpty = 1u << 8;
constexpr u32 kReceiveFull = 1u << 9;
constexpr u32 kReceiveInterrupt = 1u << 10;
constexpr u32 kError = 1u << 14;          // Read while empty or sent while full; write one to clear.
constexpr u32 kEnable = 1u << 15;

}  // namespace

Ipc::Ipc(Interrupts& interrupts, IpcPeer& peer) : interrupts_(interrupts), peer_(peer) {}

void Ipc::attach(Arm9Io& io) {
  io.add(0x04000180, 4, [this] { return read_sync(); }, [this](u32 v, u32 m) { write_sync(v, m); });
  io.add(0x04000184, 4, [this] { return read_control(); }, [this](u32 v, u32 m) { write_control(v, m); });
  io.add(0x04000188, 4, nullptr, [this](u32 v, u32 m) { write_send(v, m); });
  io.add(0x04100000, 4, [this] { return read_receive(); }, nullptr);
}

u32 Ipc::read_sync() const {
  return sync_in_ | (static_cast<u32>(sync_out_) << kSyncOutShift) |
         (sync_interrupt_enabled_ ? kSyncInterruptEnable : 0);
}

void Ipc::write_sync(u32 value, u32 mask) {
  if (mask & kSyncInterruptEnable) sync_interrupt_enabled_ = (value & kSyncInterruptEnable) != 0;
  if (mask & (15u << kSyncOutShift)) {
    sync_out_ = (value >> kSyncOutShift) & 15u;
    peer_.sync_written(sync_out_);
  }
  if ((mask & value & kSyncSendInterrupt) != 0) peer_.interrupt_requested();
}

void Ipc::peer_request_interrupt() {
  if (sync_interrupt_enabled_) interrupts_.request(Interrupts::kIpcSync);
}

u32 Ipc::read_control() const {
  // The peer takes every word at once, so the send queue is always empty.
  u32 value = kSendEmpty;
  if (send_empty_interrupt_) value |= kSendEmptyInterrupt;
  if (receive_.empty()) value |= kReceiveEmpty;
  if (receive_.size() == kQueueSize) value |= kReceiveFull;
  if (receive_interrupt_) value |= kReceiveInterrupt;
  if (error_) value |= kError;
  if (enabled_) value |= kEnable;
  return value;
}

void Ipc::write_control(u32 value, u32 mask) {
  // kSendClear: the send queue is always empty, so there is nothing to clear.
  (void)kSendClear;
  (void)kSendFull;
  if (mask & kSendEmptyInterrupt) {
    const bool on = (value & kSendEmptyInterrupt) != 0;
    // Enabling it while the queue is empty, which it always is, requests it.
    if (on && !send_empty_interrupt_) interrupts_.request(Interrupts::kIpcSendEmpty);
    send_empty_interrupt_ = on;
  }
  if (mask & kReceiveInterrupt) {
    const bool on = (value & kReceiveInterrupt) != 0;
    if (on && !receive_interrupt_ && !receive_.empty()) interrupts_.request(Interrupts::kIpcReceive);
    receive_interrupt_ = on;
  }
  if ((mask & value & kError) != 0) error_ = false;
  if (mask & kEnable) enabled_ = (value & kEnable) != 0;
}

void Ipc::write_send(u32 value, u32 mask) {
  // The program writes this register whole; a partial write would send the
  // bytes written with the rest zero.
  if (!enabled_) return;
  peer_.word_sent(value & mask);
  // The word has been taken, so the queue is empty again.
  if (send_empty_interrupt_) interrupts_.request(Interrupts::kIpcSendEmpty);
}

void Ipc::peer_send(u32 word) {
  if (receive_.size() == kQueueSize) {
    backlog_.push_back(word);
    return;
  }
  receive_.push_back(word);
  if (receive_.size() == 1 && receive_interrupt_) interrupts_.request(Interrupts::kIpcReceive);
}

u32 Ipc::read_receive() {
  if (receive_.empty()) {
    error_ = true;
    return last_received_;
  }
  last_received_ = receive_.front();
  // With the queue disabled the word is read but stays at the front.
  if (!enabled_) return last_received_;
  receive_.pop_front();
  if (!backlog_.empty()) {
    receive_.push_back(backlog_.front());
    backlog_.pop_front();
  }
  return last_received_;
}

}  // namespace bd::nds
