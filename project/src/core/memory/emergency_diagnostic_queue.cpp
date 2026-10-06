// SPDX-License-Identifier: MPL-2.0
#include "emergency_diagnostic_queue.hpp"

#include <cstring>
#include <new>
#include <utility>

namespace scratchbird::core::memory {
namespace {
constexpr usize kHeaderBytes = sizeof(u64) + sizeof(usize);
}

EmergencyDiagnosticQueue::EmergencyDiagnosticQueue(MemoryManager& manager, MemoryTag tag)
    : manager_(manager), tag_(std::move(tag)) {
  tag_.category = MemoryCategory::diagnostics;
}

EmergencyDiagnosticQueue::~EmergencyDiagnosticQueue() {
  if (backing_.valid()) SecureZeroMemory(backing_.data(), backing_.size());
}

EmergencyCaptureStatus EmergencyDiagnosticQueue::Initialize(
    usize slots, usize max_record_bytes, u64 receipt_limit) {
  Guard guard(gate_);
  if (!guard.held) return EmergencyCaptureStatus::busy;
  if (backing_.valid()) return EmergencyCaptureStatus::already_initialized;
  if (!slots || !max_record_bytes || !receipt_limit || tag_.binary_ownership.empty() ||
      !MemoryBinaryOwnershipValid(tag_) ||
      max_record_bytes > std::numeric_limits<usize>::max() - kHeaderBytes)
    return EmergencyCaptureStatus::invalid_request;
  const auto stride = kHeaderBytes + max_record_bytes;
  if (slots > std::numeric_limits<usize>::max() / stride)
    return EmergencyCaptureStatus::invalid_request;
  try {
    auto allocation = manager_.AllocateScoped(slots * stride, alignof(std::max_align_t), tag_);
    if (!allocation.ok()) return EmergencyCaptureStatus::allocation_failed;
    SecureZeroMemory(allocation.allocation.data(), allocation.allocation.size());
    backing_ = std::move(allocation.allocation);
  } catch (const std::bad_alloc&) {
    return EmergencyCaptureStatus::allocation_failed;
  }
  capacity_ = slots;
  max_record_bytes_ = max_record_bytes;
  stride_ = stride;
  receipt_limit_ = receipt_limit;
  return EmergencyCaptureStatus::ok;
}

std::byte* EmergencyDiagnosticQueue::Slot(usize index) const noexcept {
  return static_cast<std::byte*>(backing_.data()) + index * stride_;
}

void EmergencyDiagnosticQueue::Drop() noexcept {
  if (dropped_ != std::numeric_limits<u64>::max()) ++dropped_;
}

EmergencyCaptureReceipt EmergencyDiagnosticQueue::TryCapture(
    std::span<const std::byte> canonical_bytes) noexcept {
  Guard guard(gate_);
  if (!guard.held) return {EmergencyCaptureStatus::busy};
  if (!backing_.valid()) return {EmergencyCaptureStatus::uninitialized};
  if (closed_) return {EmergencyCaptureStatus::closed};
  if (canonical_bytes.empty() || canonical_bytes.size() > max_record_bytes_) {
    Drop();
    return {EmergencyCaptureStatus::invalid_request};
  }
  if (captured_ == receipt_limit_) {
    Drop();
    return {EmergencyCaptureStatus::receipt_exhausted};
  }
  if (queued_ == capacity_) {
    Drop();
    return {EmergencyCaptureStatus::full};
  }
  // Avoid head+queued overflow even for a maximally sized admitted buffer.
  const auto tail = queued_ < capacity_ - head_ ? head_ + queued_ : queued_ - (capacity_ - head_);
  auto* slot = Slot(tail);
  const auto sequence = captured_ + 1;
  const auto bytes = canonical_bytes.size();
  std::memcpy(slot, &sequence, sizeof(sequence));
  std::memcpy(slot + sizeof(sequence), &bytes, sizeof(bytes));
  std::memcpy(slot + kHeaderBytes, canonical_bytes.data(), bytes);
  ++captured_;
  ++queued_;
  return {EmergencyCaptureStatus::ok, sequence, bytes};
}

EmergencyCaptureReceipt EmergencyDiagnosticQueue::TryRead(std::span<std::byte> destination) noexcept {
  Guard guard(gate_);
  if (!guard.held) return {EmergencyCaptureStatus::busy};
  if (!backing_.valid()) return {EmergencyCaptureStatus::uninitialized};
  if (!queued_) return {EmergencyCaptureStatus::empty};
  const auto* slot = Slot(head_);
  u64 sequence = 0;
  usize bytes = 0;
  std::memcpy(&sequence, slot, sizeof(sequence));
  std::memcpy(&bytes, slot + sizeof(sequence), sizeof(bytes));
  if (destination.size() < bytes)
    return {EmergencyCaptureStatus::destination_too_small, sequence, bytes};
  std::memcpy(destination.data(), slot + kHeaderBytes, bytes);
  return {EmergencyCaptureStatus::ok, sequence, bytes};
}

EmergencyCaptureStatus EmergencyDiagnosticQueue::AcknowledgeDelivery(u64 sequence) noexcept {
  Guard guard(gate_);
  if (!guard.held) return EmergencyCaptureStatus::busy;
  if (!backing_.valid()) return EmergencyCaptureStatus::uninitialized;
  if (!queued_) return EmergencyCaptureStatus::empty;
  u64 head_sequence = 0;
  std::memcpy(&head_sequence, Slot(head_), sizeof(head_sequence));
  if (!sequence || sequence != head_sequence) return EmergencyCaptureStatus::stale_receipt;
  SecureZeroMemory(Slot(head_), stride_);
  head_ = head_ == capacity_ - 1 ? 0 : head_ + 1;
  --queued_;
  ++acknowledged_;
  return EmergencyCaptureStatus::ok;
}

EmergencyCaptureSnapshot EmergencyDiagnosticQueue::Snapshot() const noexcept {
  Guard guard(gate_);
  if (!guard.held) return {EmergencyCaptureStatus::busy};
  if (!backing_.valid()) return {};
  return {EmergencyCaptureStatus::ok, capacity_, queued_, backing_.size(),
          captured_, acknowledged_, dropped_, !closed_ && captured_ < receipt_limit_};
}

EmergencyCaptureStatus EmergencyDiagnosticQueue::CloseCapture() noexcept {
  Guard guard(gate_);
  if (!guard.held) return EmergencyCaptureStatus::busy;
  if (!backing_.valid()) return EmergencyCaptureStatus::uninitialized;
  closed_ = true;
  return EmergencyCaptureStatus::ok;
}

EmergencyDiagnosticDelivery DeliverEmergencyDiagnostic(
    EmergencyDiagnosticQueue& queue, std::span<std::byte> scratch,
    EmergencyDiagnosticSink sink, void* context) noexcept {
  EmergencyDiagnosticDelivery result;
  if (!sink) {
    result.record.status = EmergencyCaptureStatus::invalid_request;
    return result;
  }
  result.record = queue.TryRead(scratch);
  if (!result.record.ok()) return result;
  const auto receipt = sink(context, scratch.first(result.record.bytes));
  if (receipt.bytes > result.record.bytes) {
    result.record.status = EmergencyCaptureStatus::invalid_sink_receipt;
    return result;
  }
  result.delivered_bytes = receipt.bytes;
  if (!receipt.complete || receipt.bytes != result.record.bytes) {
    result.record.status = receipt.bytes == 0 ? EmergencyCaptureStatus::sink_failed
                                            : EmergencyCaptureStatus::partial_delivery;
    return result;
  }
  result.delivered = true;
  result.record.status = queue.AcknowledgeDelivery(result.record.sequence);
  result.retired = result.record.ok();
  return result;
}

}  // namespace scratchbird::core::memory
