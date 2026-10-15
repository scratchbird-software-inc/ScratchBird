// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "datatype_blob_owned_persistence.hpp"
#include <stdexcept>

namespace scratchbird::tests {
// Dedicated local physical resources, not database/security/transaction proof.
// Binary fixture identities identify this actual test budget/owner only.
struct BlobPersistenceFixture {
  using Resource = core::memory::ReservationBackedMemoryResource;
  static core::memory::AllocationPolicy Policy() {
    auto policy = core::memory::DefaultLocalEngineMemoryPolicy();
    policy.hard_limit_bytes = policy.per_context_limit_bytes = 64*1024*1024;
    policy.soft_limit_bytes = 0;
    policy.zero_memory_on_allocate = policy.zero_memory_on_release = true;
    return policy;
  }
  static core::memory::MemoryBinaryUuid Id(unsigned tag) {
    core::memory::MemoryBinaryUuid id{};
    id[0] = 1; id[6] = 0x70; id[8] = 0x80; id[15] = static_cast<unsigned char>(tag);
    return id;
  }
  core::memory::MemoryManager manager{Policy()};
  core::memory::HierarchicalMemoryBudgetLedger ledger;
  core::memory::HierarchicalMemoryScopeRef scope{core::memory::HierarchicalMemoryScopeKind::process, {}, Id(1)};
  core::datatypes::BlobValidatedProfileHandleV3 profile{};
  BlobPersistenceFixture() {
    core::memory::HierarchicalMemoryBudget budget;
    budget.scope = scope; budget.hard_limit_bytes = 64*1024*1024;
    budget.provenance.source = core::memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
    budget.provenance.source_label = "explicit_standalone_physical_policy";
    if (!ledger.SetBudget(budget).ok()) throw std::runtime_error("BLOB fixture budget admission failed");
    const auto resolved = core::datatypes::BuildCurrentBlobValidatedProfileHandleV3(core::datatypes::kBlobV11ReceiptUuid);
    if (!resolved.ok()) throw std::runtime_error("BLOB fixture exact profile unavailable");
    profile = resolved.profile;
  }
  std::unique_ptr<Resource> Grant(std::size_t bytes = 2*1024*1024) {
    core::memory::ReservationBackedMemoryResourceRequest request;
    request.reservation_ledger = &ledger; request.memory_manager = &manager;
    request.scope_chain = {scope}; request.requested_bytes = bytes;
    request.category = core::memory::MemoryCategory::datatype_payload;
    request.route_label = "blob.persistence.fixture"; request.purpose = "owned datatype codec";
    request.binary_operation_uuid = Id(2);
    request.binary_ownership[core::memory::MemoryBinaryScopeKind::context] = Id(3);
    request.binary_ownership[core::memory::MemoryBinaryScopeKind::owner] = Id(4);
    request.provenance.source = core::memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
    request.provenance.source_label = "explicit_standalone_physical_policy";
    auto admitted = core::memory::AcquireReservationBackedMemoryResource(std::move(request));
    if (!admitted.ok()) throw std::runtime_error("BLOB fixture actual physical grant refused");
    return std::move(admitted.resource);
  }
  void Copy(std::span<const core::platform::byte> bytes,
      core::datatypes::BlobOwnedPersistenceBytesV3& output) {
    auto grant = Grant();
    if (!core::datatypes::CopyBlobPersistenceBytesV3(grant, bytes, output).ok() || grant)
      throw std::runtime_error("BLOB fixture owned copy failed");
  }
};
} // namespace scratchbird::tests
