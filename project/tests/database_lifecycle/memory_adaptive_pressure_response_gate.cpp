// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "memory.hpp"
#include "memory_pressure_response.hpp"
#include "page_cache.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace mem = scratchbird::core::memory;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::storage::disk::PageType;

constexpr std::uint32_t kPageSize = 16384;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence, std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TypedUuid MakeUuid(UuidKind kind, std::uint64_t offset) {
  auto generated = uuid::GenerateEngineIdentityV7(kind, CurrentUnixMillis() + offset);
  Require(generated.ok(), "MMCH-018 UUID generation failed");
  return generated.value;
}

mem::MemoryPressureObservation BaseObservation() {
  mem::MemoryPressureObservation observation;
  observation.route_label = "embedded.sql.select.hash_join";
  observation.operation_id = "MMCH-018";
  observation.current_bytes = 64;
  observation.soft_limit_bytes = 128;
  observation.hard_limit_bytes = 1024;
  observation.unified_budget_bytes = 64;
  observation.unified_budget_limit_bytes = 1024;
  observation.engine_mga_authoritative = true;
  return observation;
}

void RequirePressureEvidence(const mem::MemoryPressureDecision& decision) {
  Require(EvidenceHas(decision.evidence, "MMCH_ADAPTIVE_MEMORY_PRESSURE_RESPONSE"),
          "MMCH-018 evidence marker missing");
  Require(EvidenceHas(
              decision.evidence,
              "memory_pressure.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority"),
          "MMCH-018 authority boundary evidence missing");
}

void LowPressurePlansNoAction() {
  const auto decision =
      mem::PlanMemoryPressureResponse(mem::MemoryPressurePolicy{}, BaseObservation());
  Require(decision.ok(), "MMCH-018 low pressure decision failed");
  Require(decision.HasAction(mem::MemoryPressureActionKind::none),
          "MMCH-018 low pressure did not plan none");
  Require(decision.pressure_percent < 70,
          "MMCH-018 low pressure percent unexpected");
  RequirePressureEvidence(decision);
}

void HighPressurePlansDeterministicActions() {
  auto observation = BaseObservation();
  observation.current_bytes = 1000;
  observation.hard_limit_bytes = 1000;
  observation.unified_budget_bytes = 980;
  observation.unified_budget_limit_bytes = 1000;
  observation.page_cache_resident_bytes = 8192;
  observation.page_cache_target_bytes = 4096;
  observation.active_spill_bytes = 2048;
  observation.reclaimable_background_bytes = 1024;
  observation.spill_supported = true;
  observation.page_cache_shrink_supported = true;
  observation.background_cleanup_supported = true;
  observation.cancellation_supported = true;

  const auto decision =
      mem::PlanMemoryPressureResponse(mem::MemoryPressurePolicy{}, observation);
  Require(decision.ok(), "MMCH-018 high pressure decision failed");
  Require(decision.HasAction(mem::MemoryPressureActionKind::throttle),
          "MMCH-018 high pressure missing throttle");
  Require(decision.HasAction(mem::MemoryPressureActionKind::prefer_spill),
          "MMCH-018 high pressure missing spill preference");
  Require(decision.HasAction(mem::MemoryPressureActionKind::shrink_page_cache),
          "MMCH-018 high pressure missing page-cache shrink");
  Require(decision.HasAction(mem::MemoryPressureActionKind::background_cleanup),
          "MMCH-018 high pressure missing cleanup");
  Require(decision.HasAction(mem::MemoryPressureActionKind::cancel_query),
          "MMCH-018 high pressure missing cancellation");
  Require(decision.HasAction(mem::MemoryPressureActionKind::refuse_allocation),
          "MMCH-018 high pressure missing allocation refusal");
  RequirePressureEvidence(decision);
}

void UnsafeAuthorityFailsClosed() {
  auto observation = BaseObservation();
  observation.parser_or_donor_authority = true;
  auto decision = mem::PlanMemoryPressureResponse(mem::MemoryPressurePolicy{}, observation);
  Require(!decision.ok() && decision.fail_closed,
          "MMCH-018 parser/donor authority did not fail closed");
  Require(decision.diagnostic.diagnostic_code == "memory_pressure_unsafe_authority",
          "MMCH-018 unsafe authority diagnostic changed");
  RequirePressureEvidence(decision);

  observation = BaseObservation();
  observation.engine_mga_authoritative = false;
  decision = mem::PlanMemoryPressureResponse(mem::MemoryPressurePolicy{}, observation);
  Require(!decision.ok() && decision.fail_closed,
          "MMCH-018 missing MGA authority did not fail closed");
  Require(decision.diagnostic.diagnostic_code == "memory_pressure_missing_engine_mga_authority",
          "MMCH-018 missing MGA authority diagnostic changed");

  observation = BaseObservation();
  observation.route_label.clear();
  decision = mem::PlanMemoryPressureResponse(mem::MemoryPressurePolicy{}, observation);
  Require(!decision.ok() && decision.fail_closed,
          "MMCH-018 missing route label did not fail closed");
  Require(decision.diagnostic.diagnostic_code == "memory_pressure_missing_route_label",
          "MMCH-018 missing route diagnostic changed");
}

mem::MemoryManager Manager(std::uint64_t pages) {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "mmch018_adaptive_pressure";
  policy.hard_limit_bytes = pages * kPageSize;
  policy.soft_limit_bytes = pages * kPageSize;
  policy.per_context_limit_bytes = pages * kPageSize;
  policy.page_buffer_pool_limit_bytes = pages * kPageSize;
  policy.reject_over_soft_limit = false;
  return mem::MemoryManager(policy);
}

page::PageCachePolicy CachePolicy(std::uint64_t pages) {
  page::PageCachePolicy policy;
  policy.max_resident_pages = pages;
  policy.max_resident_bytes = pages * kPageSize;
  policy.require_memory_manager_frames = true;
  policy.allow_dirty_eviction = false;
  return policy;
}

page::PageCacheEntry Entry(const TypedUuid& database_uuid,
                           const TypedUuid& filespace_uuid,
                           std::uint64_t index) {
  page::PageCacheEntry entry;
  entry.database_uuid = database_uuid;
  entry.filespace_uuid = filespace_uuid;
  entry.page_uuid = MakeUuid(UuidKind::page, 100 + index);
  entry.page_type = PageType::row_data;
  entry.page_number = index;
  entry.page_generation = 1;
  entry.page_size = kPageSize;
  return entry;
}

page::PageCacheLifecycleInput LifecycleInput(const TypedUuid& database_uuid,
                                             const TypedUuid& filespace_uuid,
                                             std::uint64_t target_pages) {
  page::PageCacheLifecycleInput input;
  input.database_uuid = database_uuid;
  input.filespace_uuid = filespace_uuid;
  input.database_lifecycle_state = "opened";
  input.policy_generation = 18;
  input.checkpoint_generation = 180;
  input.target_resident_pages = target_pages;
  input.tx2_activation_committed = true;
  input.cache_runtime_started = true;
  input.engine_agent_active = true;
  input.writeback_allowed = true;
  input.checkpoint_allowed = true;
  input.standalone_mode = true;
  input.cluster_authority_available = false;
  return input;
}

void PageCacheShrinkActionConsumesRealPath() {
  auto manager = Manager(4);
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  auto admit_policy = CachePolicy(3);
  const auto database_uuid = MakeUuid(UuidKind::database, 1);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 2);

  Require(page::AdmitPageCacheEntry(&ledger, admit_policy, Entry(database_uuid, filespace_uuid, 1)).ok(),
          "MMCH-018 page-cache setup 1 failed");
  Require(page::AdmitPageCacheEntry(&ledger, admit_policy, Entry(database_uuid, filespace_uuid, 2)).ok(),
          "MMCH-018 page-cache setup 2 failed");
  Require(page::AdmitPageCacheEntry(&ledger, admit_policy, Entry(database_uuid, filespace_uuid, 3)).ok(),
          "MMCH-018 page-cache setup 3 failed");

  auto observation = BaseObservation();
  observation.current_bytes = 850;
  observation.hard_limit_bytes = 1000;
  observation.unified_budget_bytes = 850;
  observation.unified_budget_limit_bytes = 1000;
  observation.page_cache_resident_bytes = 3ull * kPageSize;
  observation.page_cache_target_bytes = kPageSize;
  observation.page_cache_shrink_supported = true;
  const auto decision =
      mem::PlanMemoryPressureResponse(mem::MemoryPressurePolicy{}, observation);
  Require(decision.ok() &&
              decision.HasAction(mem::MemoryPressureActionKind::shrink_page_cache),
          "MMCH-018 pressure did not select page-cache shrink");

  auto pressure = page::ApplyPageCacheMemoryPressure(
      &ledger,
      CachePolicy(1),
      LifecycleInput(database_uuid, filespace_uuid, 1));
  Require(pressure.ok() && pressure.publication.memory_pressure_handled,
          "MMCH-018 real page-cache pressure path did not complete");
  Require(pressure.snapshot.resident_pages == 1 &&
              pressure.snapshot.memory_manager_frame_count == 1 &&
              pressure.evicted_pages == 2,
          "MMCH-018 page-cache pressure did not shrink frames");
  Require(manager.Snapshot().page_buffer_current_bytes == kPageSize,
          "MMCH-018 page-cache shrink did not release MemoryManager bytes");
}

}  // namespace

int main() {
  std::cout << "MMCH-018 authority_note=adaptive_memory_pressure_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority"
            << '\n';
  LowPressurePlansNoAction();
  HighPressurePlansDeterministicActions();
  UnsafeAuthorityFailsClosed();
  PageCacheShrinkActionConsumesRealPath();
  return EXIT_SUCCESS;
}
