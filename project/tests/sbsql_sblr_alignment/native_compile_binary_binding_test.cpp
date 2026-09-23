// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/native_compile/native_compile.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace n = scratchbird::engine::native_compile;
namespace m = scratchbird::core::memory;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  n::NativeCompileRequest request;
  request.principal_uuid = scratchbird::tests::FixtureUuid(1093, 1);
  request.target_object_uuid = scratchbird::tests::FixtureUuid(1093, 2);
  request.principal_uuid.bytes[8] = 0x80;
  request.principal_uuid.bytes[9] = 0;
  request.principal_uuid.bytes[15] = 0xff;
  request.database_uuid = scratchbird::tests::FixtureUuid(1094, 1);
  request.session_uuid = scratchbird::tests::FixtureUuid(1094, 2);
  request.statement_uuid = scratchbird::tests::FixtureUuid(1094, 3);
  request.module_payload = "module";
  request.security_context_present = true;
  request.catalog_generation_id = request.security_epoch = 1;
  request.policy_epoch = request.resource_epoch = 1;
  request.descriptors.push_back({scratchbird::tests::FixtureUuid(1093, 3), "scalar", "int64", "codec"});
  n::BackendInfo backend;
  backend.provider = "test-provider"; backend.target_triple = "test-target";
  backend.load_mode = "dynamic";
  n::Lowerability lowering;
  Check(n::CacheKeyComplete(request, backend));
  const auto material = n::CacheKeyMaterial(request, backend, lowering);
  Check(n::NativeArtifactInvalidatedByDependency(material, "principal_uuid", request.principal_uuid));
  Check(!n::NativeArtifactInvalidatedByDependency(material, "target_object_uuid", request.principal_uuid));
  auto changed = request;
  changed.principal_uuid.bytes[15] = 0xfe;
  Check(!n::NativeArtifactInvalidatedByDependency(material, "principal_uuid", changed.principal_uuid));
  Check(material != n::CacheKeyMaterial(changed, backend, lowering));
  Check(n::NativeArtifactInvalidatedByDependency(material.substr(0, material.size() - 1), "principal_uuid", changed.principal_uuid));
  Check(n::NativeArtifactInvalidatedByDependency("old=text", "principal_uuid", request.principal_uuid));
  changed = request;
  changed.descriptors[0].descriptor_uuid.bytes[15] ^= 1;
  Check(n::DescriptorSetDigest(changed) != n::DescriptorSetDigest(request));
  changed = request;
  changed.principal_uuid.bytes[8] = 0;
  Check(!n::CacheKeyComplete(changed, backend));
  // Former delimiter injection could collapse these two field sequences.
  changed = request;
  request.sblr_version = "v;opcode_registry_epoch=x";
  request.opcode_registry_epoch = "y";
  changed.sblr_version = "v";
  changed.opcode_registry_epoch = "x;opcode_registry_epoch=y";
  Check(n::CacheKeyMaterial(request, backend, lowering) !=
        n::CacheKeyMaterial(changed, backend, lowering));
  m::HierarchicalMemoryBudgetLedger budget;
  m::ForeignMemoryReservationLedger foreign;
  request.memory_accounting.reservation_ledger = &budget;
  request.memory_accounting.foreign_ledger = &foreign;
  auto accounting = n::BuildLlvmMemoryAccountingRequest(request, backend, true);
  Check(accounting.owner_id.empty() && accounting.owning_scope.empty());
  Check(accounting.binary_owner_uuid == request.principal_uuid.bytes);
  Check(accounting.binary_owning_scope_uuid == request.statement_uuid.bytes);
  Check(accounting.scope_chain[2].scope_id.empty() &&
        accounting.scope_chain[2].binary_scope_uuid == request.session_uuid.bytes);
  for (const auto& scope : accounting.scope_chain) {
    m::HierarchicalMemoryBudget limit;
    limit.scope = scope; limit.hard_limit_bytes = 16 * 1024 * 1024;
    limit.provenance = accounting.provenance;
    Check(budget.SetBudget(limit).ok());
  }
  auto first = m::AcquireLlvmMemoryAccountingReservation(accounting);
  Check(first.ok());
  auto second_request = accounting;
  second_request.binary_owner_uuid[15] = 0xfe;
  second_request.binary_owning_scope_uuid[15] ^= 1;
  auto second = m::AcquireLlvmMemoryAccountingReservation(second_request);
  Check(second.ok());
  auto snapshot = foreign.Snapshot();
  Check(snapshot.owning_scopes.size() == 2 && snapshot.active_reservations.size() == 8);
  for (const auto& active : snapshot.active_reservations) {
    Check(active.owner_id.empty() && active.owning_scope.empty());
    Check(active.binary_owner_uuid == accounting.binary_owner_uuid ||
          active.binary_owner_uuid == second_request.binary_owner_uuid);
  }
  Check(!foreign.CleanupOwner(std::string{}).status.ok());
  Check(foreign.CleanupOwner(accounting.binary_owner_uuid).status.ok());
  Check(foreign.Snapshot().active_reservations.size() == 4);
  for (const auto& active : foreign.Snapshot().active_reservations) {
    Check(active.binary_owner_uuid == second_request.binary_owner_uuid);
  }
  auto invalid = accounting;
  invalid.owner_id = "ambiguous";
  Check(!m::AcquireLlvmMemoryAccountingReservation(invalid).ok());
  invalid = accounting; invalid.binary_owning_scope_uuid[8] = 0;
  Check(!m::AcquireLlvmMemoryAccountingReservation(invalid).ok());
  Check(second.reservation->Release().ok());
  Check(foreign.Snapshot().active_reservations.empty());
}
