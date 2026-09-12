// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "typed_result_transport_codec.hpp"
#include <memory_resource>

namespace scratchbird::wire {

// Move-only process-private packet storage. The supplied memory resource must
// outlive this object and all borrowed views of encoded. No resource or result
// authority is created here. Use a real reservation-backed resource when the
// owning route requires physically admitted storage.
//
// The packet is serialized once into this resource. Source rows and descriptor
// validation/control metadata remain caller-owned or ordinary allocations;
// they are not implicitly charged by choosing a packet allocator.
struct TypedResultResourceBuffer {
  explicit TypedResultResourceBuffer(std::pmr::memory_resource& resource)
      : encoded(&resource) {}
  TypedResultResourceBuffer(const TypedResultResourceBuffer&) = delete;
  TypedResultResourceBuffer& operator=(const TypedResultResourceBuffer&) = delete;
  TypedResultResourceBuffer(TypedResultResourceBuffer&&) noexcept = default;
  TypedResultResourceBuffer& operator=(TypedResultResourceBuffer&&) = delete;

  TypedResultCodecStatus status = TypedResultCodecStatus::invalid_argument;
  std::string diagnostic_code;
  std::string detail;
  std::pmr::vector<byte> encoded;
  TypedResultEvidenceHash descriptor_evidence_sha256{};
  TypedResultEvidenceHash batch_evidence_sha256{};
  bool ok() const { return status == TypedResultCodecStatus::ok; }
};

// Same exact validation and wire bytes as EncodeTypedResultBatch, without
// returning another copy of the source batch. Refusal exposes no packet;
// allocation exceptions propagate after staged storage has been released.
// The engine publication boundary must handle those exceptions before commit.
TypedResultResourceBuffer EncodeTypedResultBatchBuffer(
    const TypedResultBatch& batch,
    const TypedResultRowDescriptor& descriptor,
    const TypedResultCarrierBinding& carrier_binding,
    std::pmr::memory_resource& resource,
    u64 maximum_bytes = 16ull * 1024ull * 1024ull);

}  // namespace scratchbird::wire
