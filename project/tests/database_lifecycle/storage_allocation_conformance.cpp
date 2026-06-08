// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "page_allocation_lifecycle.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

u64 NextMillis() {
  static u64 next = 1779501900000ull;
  return ++next;
}

TypedUuid NewUuid(UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "DBLC-013AF UUID generation failed");
  return generated.value;
}

page::PageAllocationRequest BaseRequest(const page::PageAllocationLedger& ledger) {
  page::PageAllocationRequest request;
  request.database_uuid = ledger.database_uuid;
  request.filespace_uuid = ledger.filespace_uuid;
  request.owner_object_uuid = NewUuid(UuidKind::object);
  request.creator_transaction_uuid = NewUuid(UuidKind::transaction);
  request.creator_local_transaction_id = 10;
  request.page_family = "data";
  request.page_count = 4;
  request.engine_authoritative = true;
  return request;
}

page::PageAllocationLedger NewLedger() {
  page::PageAllocationLedger ledger;
  ledger.database_uuid = NewUuid(UuidKind::database);
  ledger.filespace_uuid = NewUuid(UuidKind::filespace);
  ledger.free_extents.push_back({100, 16});
  return ledger;
}

void TestAllocationAndMGAReuse() {
  auto ledger = NewLedger();
  const auto allocated = page::ReservePageAllocation(&ledger, BaseRequest(ledger));
  Require(allocated.ok(), "DBLC-013AF allocation was refused");
  Require(allocated.allocation.start_page == 100, "DBLC-013AF allocation start mismatch");
  Require(allocated.allocation.page_count == 4, "DBLC-013AF allocation count mismatch");
  Require(ledger.free_extents.size() == 1 && ledger.free_extents.front().start_page == 104,
          "DBLC-013AF free map did not consume extent");

  page::PageReleaseRequest release;
  release.allocation_uuid = allocated.allocation.allocation_uuid;
  release.cleanup_horizon_local_transaction_id = 11;
  release.engine_mga_authoritative = true;
  const auto reusable = page::MarkPageAllocationReusable(&ledger, release);
  Require(reusable.ok() && reusable.changed,
          "DBLC-013AF reusable-pending transition failed");
  Require(reusable.allocation.state == page::PageAllocationLifecycleState::reusable_pending_mga,
          "DBLC-013AF reusable-pending state mismatch");

  release.cleanup_horizon_local_transaction_id = 10;
  const auto blocked = page::ReclaimReusablePageAllocation(&ledger, release);
  Require(!blocked.ok(), "DBLC-013AF reclaim before MGA horizon was accepted");
  Require(blocked.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-BLOCKED-BY-MGA-HORIZON",
          "DBLC-013AF wrong MGA horizon diagnostic");

  release.cleanup_horizon_local_transaction_id = 20;
  const auto reclaimed = page::ReclaimReusablePageAllocation(&ledger, release);
  Require(reclaimed.ok() && reclaimed.changed, "DBLC-013AF reclaim after MGA horizon failed");
  Require(reclaimed.allocation.state == page::PageAllocationLifecycleState::reusable_free,
          "DBLC-013AF reclaim state mismatch");
  Require(!ledger.free_extents.empty(), "DBLC-013AF reclaimed extent missing from free map");
}

void TestRefusalsAndCompaction() {
  auto ledger = NewLedger();
  auto request = BaseRequest(ledger);
  request.engine_authoritative = false;
  const auto external = page::ReservePageAllocation(&ledger, request);
  Require(!external.ok(), "DBLC-013AF non-engine allocation was accepted");
  Require(external.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-NOT-ENGINE-AUTHORITATIVE",
          "DBLC-013AF non-engine diagnostic mismatch");

  request = BaseRequest(ledger);
  request.cluster_route_requested = true;
  const auto cluster = page::ReservePageAllocation(&ledger, request);
  Require(!cluster.ok(), "DBLC-013AF cluster allocation route was accepted");
  Require(cluster.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-CLUSTER-ROUTE-UNAVAILABLE",
          "DBLC-013AF cluster diagnostic mismatch");

  request = BaseRequest(ledger);
  request.page_count = 32;
  const auto too_large = page::ReservePageAllocation(&ledger, request);
  Require(!too_large.ok(), "DBLC-013AF over-allocation was accepted");
  Require(too_large.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-INSUFFICIENT-FREE-SPACE",
          "DBLC-013AF over-allocation diagnostic mismatch");

  const auto allocated = page::ReservePageAllocation(&ledger, BaseRequest(ledger));
  Require(allocated.ok(), "DBLC-013AF allocation for compaction failed");
  page::PageReleaseRequest release;
  release.allocation_uuid = allocated.allocation.allocation_uuid;
  release.cleanup_horizon_local_transaction_id = 20;
  release.engine_mga_authoritative = true;
  Require(page::MarkPageAllocationReusable(&ledger, release).ok(),
          "DBLC-013AF reusable-pending for compaction failed");

  page::PageCompactionRequest compact;
  compact.engine_authoritative = true;
  compact.shutdown_or_maintenance_fenced = true;
  const auto blocked_compaction = page::CompactPageFreeSpace(&ledger, compact);
  Require(!blocked_compaction.ok(), "DBLC-013AF compaction ignored pending MGA pages");
  Require(blocked_compaction.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-COMPACTION-BLOCKED-BY-MGA",
          "DBLC-013AF compaction diagnostic mismatch");
}

void TestRecoveryClassification() {
  auto ledger = NewLedger();
  const auto allocated = page::ReservePageAllocation(&ledger, BaseRequest(ledger));
  Require(allocated.ok(), "DBLC-013AF allocation for recovery failed");

  page::PageAllocationEntry incomplete = allocated.allocation;
  incomplete.allocation_uuid = NewUuid(UuidKind::object);
  incomplete.state = page::PageAllocationLifecycleState::reserved;
  ledger.allocations.push_back(incomplete);

  const auto recovery = page::ClassifyPageAllocationLedgerForRecovery(ledger);
  Require(recovery.ok(), "DBLC-013AF recovery classification failed");
  bool retained_allocated = false;
  bool failed_closed_reserved = false;
  for (const auto& classification : recovery.classifications) {
    retained_allocated = retained_allocated ||
                         classification.action == page::PageAllocationRecoveryAction::retain;
    failed_closed_reserved = failed_closed_reserved ||
                             classification.fail_closed;
  }
  Require(retained_allocated, "DBLC-013AF recovery did not retain allocated pages");
  Require(failed_closed_reserved, "DBLC-013AF recovery did not fail closed reserved pages");
}

void TestDurabilityOrderingRefusesNonDurableGeneration() {
  auto ledger = NewLedger();
  auto request = BaseRequest(ledger);
  request.durability_fence_satisfied = false;
  const auto non_durable = page::ReservePageAllocation(&ledger, request);
  Require(!non_durable.ok(), "DBLC-013AF non-durable allocation generation was accepted");
  Require(non_durable.diagnostic.diagnostic_code ==
              "SB-STORAGE-PAGE-ALLOCATION-DURABILITY-FENCE-REQUIRED",
          "DBLC-013AF non-durable allocation diagnostic mismatch");

  request = BaseRequest(ledger);
  const auto allocated = page::ReservePageAllocation(&ledger, request);
  Require(allocated.ok(), "DBLC-013AF durable allocation was refused");
  auto corrupt = allocated.allocation;
  corrupt.allocation_uuid = NewUuid(UuidKind::object);
  corrupt.published_page_generation = corrupt.durable_page_generation + 1;
  corrupt.durability_fence_satisfied = false;
  ledger.allocations.push_back(corrupt);

  const auto recovery = page::ClassifyPageAllocationLedgerForRecovery(ledger);
  Require(recovery.ok(), "DBLC-013AF durability recovery classification failed");
  bool failed_closed_non_durable = false;
  for (const auto& classification : recovery.classifications) {
    failed_closed_non_durable = failed_closed_non_durable ||
                                (classification.fail_closed &&
                                 classification.stable_reason ==
                                     "allocation references non-durable page generation");
  }
  Require(failed_closed_non_durable,
          "DBLC-013AF recovery did not fail closed non-durable page generation");
}

}  // namespace

int main() {
  TestAllocationAndMGAReuse();
  TestRefusalsAndCompaction();
  TestRecoveryClassification();
  TestDurabilityOrderingRefusesNonDurableGeneration();
  return EXIT_SUCCESS;
}
