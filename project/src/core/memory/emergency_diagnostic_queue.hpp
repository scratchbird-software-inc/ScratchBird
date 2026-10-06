// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "memory.hpp"

#include <atomic>
#include <limits>
#include <span>

namespace scratchbird::core::memory {

enum class EmergencyCaptureStatus {
  ok, busy, uninitialized, invalid_request, allocation_failed,
  already_initialized, full, empty, destination_too_small,
  stale_receipt, receipt_exhausted, closed, sink_failed, partial_delivery,
  invalid_sink_receipt
};

struct EmergencyCaptureReceipt {
  EmergencyCaptureStatus status = EmergencyCaptureStatus::uninitialized;
  u64 sequence = 0;
  usize bytes = 0;
  bool ok() const { return status == EmergencyCaptureStatus::ok; }
};

struct EmergencyCaptureSnapshot {
  EmergencyCaptureStatus status = EmergencyCaptureStatus::uninitialized;
  usize capacity = 0;
  usize queued = 0;
  usize backing_bytes = 0;
  u64 captured = 0;
  u64 acknowledged = 0;
  u64 dropped = 0;
  bool accepting = false;
};

// Custody of already-authorized/redacted canonical bytes, NOT a renderer or
// validator. Producers and a single trusted delivery consumer own those duties.
// No external callback is invoked here. Receipts are transient queue-local
// sequence tokens, never engine identities or durable/wire representations.
// Manager outlives queue; destruction requires quiescent producers/consumer.
class EmergencyDiagnosticQueue {
 public:
  EmergencyDiagnosticQueue(MemoryManager& manager, MemoryTag tag);
  ~EmergencyDiagnosticQueue();
  EmergencyDiagnosticQueue(const EmergencyDiagnosticQueue&) = delete;
  EmergencyDiagnosticQueue& operator=(const EmergencyDiagnosticQueue&) = delete;

  // Bootstrap only: reserves all slot headers and payload capacity in one real
  // governed allocation. No live resizing or implicit reset of captured data.
  EmergencyCaptureStatus Initialize(usize slots, usize max_record_bytes,
      u64 receipt_limit = std::numeric_limits<u64>::max());
  // All methods below make one nonblocking lock attempt and allocate nothing.
  // Busy consumes nothing; the caller retains responsibility for its record.
  EmergencyCaptureReceipt TryCapture(std::span<const std::byte> canonical_bytes) noexcept;
  EmergencyCaptureReceipt TryRead(std::span<std::byte> destination) noexcept;
  // Call only after full sink delivery. Partial/failed delivery must not ack.
  // Exact head sequence required; duplicate/stale acknowledgements retain data.
  EmergencyCaptureStatus AcknowledgeDelivery(u64 sequence) noexcept;
  // Stop new capture without discarding retained records or releasing backing.
  EmergencyCaptureStatus CloseCapture() noexcept;
  EmergencyCaptureSnapshot Snapshot() const noexcept;

 private:
  struct Guard {
    explicit Guard(std::atomic_flag& flag) noexcept : flag(flag), held(!flag.test_and_set(std::memory_order_acquire)) {}
    ~Guard() { if (held) flag.clear(std::memory_order_release); }
    std::atomic_flag& flag;
    bool held;
  };
  std::byte* Slot(usize index) const noexcept;
  void Drop() noexcept;
  mutable std::atomic_flag gate_ = ATOMIC_FLAG_INIT;
  MemoryManager& manager_;
  MemoryTag tag_;
  ScopedAllocation backing_;
  usize capacity_ = 0;
  usize max_record_bytes_ = 0;
  usize stride_ = 0;
  usize head_ = 0;
  usize queued_ = 0;
  u64 receipt_limit_ = 0;
  u64 captured_ = 0;
  u64 acknowledged_ = 0;
  u64 dropped_ = 0;
  bool closed_ = false;
};

struct EmergencyDiagnosticSinkReceipt {
  usize bytes = 0;
  bool complete = false;
};
using EmergencyDiagnosticSink = EmergencyDiagnosticSinkReceipt (*)(
    void* context, std::span<const std::byte> bytes) noexcept;
struct EmergencyDiagnosticDelivery {
  EmergencyCaptureReceipt record;
  usize delivered_bytes = 0;
  bool delivered = false;
  bool retired = false;
};
// Single trusted consumer. Scratch and sink context are caller-owned and
// pre-admitted. Sink must be nonblocking/allocation-free and apply its admitted
// disclosure policy. No queue lock is held across the callback. If delivered
// but not retired (busy ack), retry only AcknowledgeDelivery with this receipt;
// rerunning the sink can duplicate delivery. Completion is not durable storage.
EmergencyDiagnosticDelivery DeliverEmergencyDiagnostic(
    EmergencyDiagnosticQueue& queue, std::span<std::byte> scratch,
    EmergencyDiagnosticSink sink, void* context) noexcept;

}  // namespace scratchbird::core::memory
