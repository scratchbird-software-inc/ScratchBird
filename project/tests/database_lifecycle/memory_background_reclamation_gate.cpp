// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "background_memory_reclamation.hpp"
#include "memory.hpp"
#include "metric_registry.hpp"
#include "page_cache.hpp"
#include "temp_workspace_lifecycle.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

namespace mem = scratchbird::core::memory;
namespace metrics = scratchbird::core::metrics;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::Status;
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

void RequireReclamationEvidence(const mem::BackgroundMemoryReclamationResult& result) {
  Require(EvidenceHas(result.evidence, "MMCH_BACKGROUND_MEMORY_RECLAMATION"),
          "MMCH-019 evidence marker missing");
  Require(EvidenceHas(
              result.evidence,
              "background_reclamation.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"),
          "MMCH-019 authority boundary evidence missing");
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

TypedUuid MakeUuid(UuidKind kind, std::uint64_t offset) {
  auto generated = uuid::GenerateEngineIdentityV7(kind, CurrentUnixMillis() + offset);
  Require(generated.ok(), "MMCH-019 UUID generation failed");
  return generated.value;
}

mem::AllocationPolicy MemoryPolicy(std::uint64_t bytes) {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "mmch019_background_reclamation";
  policy.hard_limit_bytes = bytes;
  policy.soft_limit_bytes = bytes;
  policy.per_context_limit_bytes = bytes;
  policy.page_buffer_pool_limit_bytes = bytes;
  policy.reject_over_soft_limit = false;
  return policy;
}

mem::BackgroundMemoryReclamationRequest BaseRequest() {
  mem::BackgroundMemoryReclamationRequest request;
  request.route_label = "engine.maintenance.memory.background";
  request.operation_id = "MMCH-019";
  request.engine_mga_authoritative = true;
  return request;
}

void IdleArenaDiagnosticsAndQueryStateAreReclaimed() {
  mem::MemoryManager manager(MemoryPolicy(1024 * 1024));
  mem::MemoryTag tag;
  tag.purpose = "mmch019_idle_arena";
  tag.context_id = "mmch019_arena";
  auto arena = manager.CreateArena(tag);
  Require(arena.Allocate(32768, 16).ok(), "MMCH-019 arena allocation failed");
  Require(manager.Snapshot().current_bytes == 32768,
          "MMCH-019 arena setup did not charge memory");

  std::uint64_t diagnostics = 2;
  std::uint64_t completed_query_states = 1;
  auto request = BaseRequest();
  request.work_items.push_back({
      mem::BackgroundMemoryReclamationWorkKind::idle_arena,
      "idle_arena",
      32768,
      true,
      true,
      false,
      false,
      false,
      false,
      [&arena](std::vector<std::string>* evidence) -> Status {
        auto released = arena.Reset();
        if (evidence != nullptr) {
          evidence->push_back("MMCH-019 idle_arena_reset=true");
        }
        return released.status;
      }});
  request.work_items.push_back({
      mem::BackgroundMemoryReclamationWorkKind::old_diagnostic,
      "old_diagnostics",
      512,
      true,
      true,
      false,
      false,
      false,
      false,
      [&diagnostics](std::vector<std::string>* evidence) -> Status {
        diagnostics = 0;
        if (evidence != nullptr) {
          evidence->push_back("MMCH-019 old_diagnostics_reclaimed=true");
        }
        return {scratchbird::core::platform::StatusCode::ok,
                scratchbird::core::platform::Severity::info,
                scratchbird::core::platform::Subsystem::memory};
      }});
  request.work_items.push_back({
      mem::BackgroundMemoryReclamationWorkKind::completed_query_state,
      "completed_query_state",
      256,
      true,
      true,
      false,
      false,
      false,
      false,
      [&completed_query_states](std::vector<std::string>* evidence) -> Status {
        completed_query_states = 0;
        if (evidence != nullptr) {
          evidence->push_back("MMCH-019 completed_query_state_reclaimed=true");
        }
        return {scratchbird::core::platform::StatusCode::ok,
                scratchbird::core::platform::Severity::info,
                scratchbird::core::platform::Subsystem::memory};
      }});

  auto result = mem::RunBackgroundMemoryReclamation(
      mem::BackgroundMemoryReclamationPolicy{},
      request);
  Require(result.ok(), "MMCH-019 background reclamation failed");
  Require(result.counters.reclaimed_count == 3 &&
              result.counters.retained_count == 0 &&
              result.counters.reclaimed_bytes == 33536,
          "MMCH-019 background reclamation counters mismatch");
  Require(manager.Snapshot().current_bytes == 0,
          "MMCH-019 idle arena memory was not released");
  Require(diagnostics == 0 && completed_query_states == 0,
          "MMCH-019 diagnostic or query state was not reclaimed");
  Require(EvidenceHas(result.evidence, "background_reclamation.metrics_published=true"),
          "MMCH-019 metrics publication evidence missing");
  bool metric_seen = false;
  for (const auto& sample : metrics::DefaultMetricRegistry().SnapshotHistory()) {
    if (sample.family == "sb_memory_background_reclaimed_bytes_total") {
      metric_seen = true;
      break;
    }
  }
  Require(metric_seen, "MMCH-019 metric sample was not published");
  RequireReclamationEvidence(result);
}

void BudgetsCancellationAndAuthorityFailClosed() {
  auto request = BaseRequest();
  std::uint64_t reclaimed = 0;
  request.work_items.push_back({
      mem::BackgroundMemoryReclamationWorkKind::old_diagnostic,
      "first",
      100,
      true,
      true,
      false,
      false,
      false,
      false,
      [&reclaimed](std::vector<std::string>*) -> Status {
        ++reclaimed;
        return {scratchbird::core::platform::StatusCode::ok,
                scratchbird::core::platform::Severity::info,
                scratchbird::core::platform::Subsystem::memory};
      }});
  request.work_items.push_back({
      mem::BackgroundMemoryReclamationWorkKind::old_diagnostic,
      "second",
      100,
      true,
      true,
      false,
      false,
      false,
      false,
      [&reclaimed](std::vector<std::string>*) -> Status {
        ++reclaimed;
        return {scratchbird::core::platform::StatusCode::ok,
                scratchbird::core::platform::Severity::info,
                scratchbird::core::platform::Subsystem::memory};
      }});
  mem::BackgroundMemoryReclamationPolicy policy;
  policy.max_items_per_run = 1;
  auto result = mem::RunBackgroundMemoryReclamation(policy, request);
  Require(result.ok() &&
              result.counters.reclaimed_count == 1 &&
              result.counters.retained_count == 1 &&
              reclaimed == 1,
          "MMCH-019 item budget did not retain extra work");

  request = BaseRequest();
  request.work_items.push_back({
      mem::BackgroundMemoryReclamationWorkKind::spill_record,
      "cancel_unsafe",
      1,
      true,
      false,
      false,
      false,
      false,
      false,
      [](std::vector<std::string>*) -> Status {
        Fail("MMCH-019 cancellation-unsafe callback executed");
      }});
  policy = mem::BackgroundMemoryReclamationPolicy{};
  policy.cancellation_requested = true;
  result = mem::RunBackgroundMemoryReclamation(policy, request);
  Require(result.ok() &&
              result.cancelled &&
              result.counters.cancelled_count == 1 &&
              result.counters.retained_count == 1,
          "MMCH-019 cancellation did not retain unsafe work");

  request = BaseRequest();
  request.work_items.push_back({
      mem::BackgroundMemoryReclamationWorkKind::old_diagnostic,
      "unsafe_authority",
      1,
      true,
      true,
      true,
      false,
      false,
      false,
      nullptr});
  result = mem::RunBackgroundMemoryReclamation(
      mem::BackgroundMemoryReclamationPolicy{},
      request);
  Require(!result.ok() &&
              result.fail_closed &&
              result.diagnostic.diagnostic_code == "background_reclamation_unsafe_authority",
          "MMCH-019 unsafe authority did not fail closed");
  RequireReclamationEvidence(result);
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
  input.policy_generation = 19;
  input.checkpoint_generation = 190;
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

std::filesystem::path TempRoot() {
  return std::filesystem::temp_directory_path() /
         ("scratchbird_mmch019_" + std::to_string(CurrentUnixMillis()));
}

mem::TempWorkspacePolicy TempPolicy(const std::filesystem::path& root) {
  mem::TempWorkspacePolicy policy;
  policy.policy_name = "mmch019_temp";
  policy.root_path = root;
  policy.filespace_quota_bytes = 1024 * 1024;
  policy.session_quota_bytes = 1024 * 1024;
  policy.transaction_quota_bytes = 1024 * 1024;
  policy.statement_quota_bytes = 1024 * 1024;
  policy.operation_quota_bytes = 1024 * 1024;
  policy.disk_reservation_mode = mem::TempWorkspaceDiskReservationMode::logical_quota_only;
  policy.sparse_file_reservation = false;
  policy.cleanup_files_on_release = true;
  return policy;
}

mem::TempWorkspaceAllocationRequest SpillRequest() {
  mem::TempWorkspaceAllocationRequest request;
  request.storage_class = mem::TempStorageClass::spill_file;
  request.lifetime = mem::TempWorkspaceLifetime::operation_lifetime;
  request.owner.temp_object_uuid = "mmch019-temp";
  request.owner.database_id = "db";
  request.owner.engine_id = "engine";
  request.owner.session_id = "session";
  request.owner.transaction_id = "txn";
  request.owner.statement_id = "stmt";
  request.owner.operation_id = "mmch019-spill";
  request.owner.policy_generation = 1;
  request.bytes = 4096;
  request.purpose = "mmch019_spill";
  return request;
}

void PageCacheAndSpillRecordsUseRealSubsystemPaths() {
  mem::MemoryManager manager(MemoryPolicy(8ull * kPageSize));
  page::PageCacheLedger ledger;
  page::BindPageCacheMemoryManager(&ledger, &manager);
  const auto database_uuid = MakeUuid(UuidKind::database, 1);
  const auto filespace_uuid = MakeUuid(UuidKind::filespace, 2);
  auto admit_policy = CachePolicy(3);
  Require(page::AdmitPageCacheEntry(&ledger, admit_policy, Entry(database_uuid, filespace_uuid, 1)).ok(),
          "MMCH-019 page-cache setup 1 failed");
  Require(page::AdmitPageCacheEntry(&ledger, admit_policy, Entry(database_uuid, filespace_uuid, 2)).ok(),
          "MMCH-019 page-cache setup 2 failed");
  Require(page::AdmitPageCacheEntry(&ledger, admit_policy, Entry(database_uuid, filespace_uuid, 3)).ok(),
          "MMCH-019 page-cache setup 3 failed");

  const auto root = TempRoot();
  std::filesystem::remove_all(root);
  mem::TempWorkspaceLifecycleManager temp(TempPolicy(root));
  auto spill = temp.AllocateSpillFile(SpillRequest());
  Require(spill.ok(), "MMCH-019 spill setup failed");
  Require(temp.Snapshot().active_bytes == 4096,
          "MMCH-019 spill setup did not account active bytes");

  auto request = BaseRequest();
  request.work_items.push_back({
      mem::BackgroundMemoryReclamationWorkKind::clean_page_cache_frame,
      "page_cache_shrink",
      2ull * kPageSize,
      true,
      true,
      false,
      false,
      false,
      false,
      [&ledger, database_uuid, filespace_uuid](std::vector<std::string>* evidence) -> Status {
        auto pressure = page::ApplyPageCacheMemoryPressure(
            &ledger,
            CachePolicy(1),
            LifecycleInput(database_uuid, filespace_uuid, 1));
        if (evidence != nullptr) {
          evidence->insert(evidence->end(), pressure.evidence.begin(), pressure.evidence.end());
        }
        return pressure.status;
      }});
  request.work_items.push_back({
      mem::BackgroundMemoryReclamationWorkKind::spill_record,
      "spill_cleanup",
      4096,
      true,
      true,
      false,
      false,
      false,
      false,
      [&temp](std::vector<std::string>* evidence) -> Status {
        auto cleanup = temp.CleanupOperation("mmch019-spill");
        if (evidence != nullptr) {
          evidence->push_back("MMCH-019 spill_cleanup_cleaned_count=" +
                              std::to_string(cleanup.cleaned_count));
        }
        return cleanup.status;
      }});

  auto result = mem::RunBackgroundMemoryReclamation(
      mem::BackgroundMemoryReclamationPolicy{},
      request);
  Require(result.ok() && result.counters.reclaimed_count == 2,
          "MMCH-019 page-cache/spill reclamation failed");
  Require(page::SnapshotPageCache(ledger).resident_pages == 1 &&
              manager.Snapshot().page_buffer_current_bytes == kPageSize,
          "MMCH-019 page-cache reclamation did not release frames");
  Require(temp.Snapshot().active_bytes == 0,
          "MMCH-019 spill cleanup did not release temp bytes");
  RequireReclamationEvidence(result);
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  std::cout << "MMCH-019 authority_note=background_memory_reclamation_evidence_only;"
               "not_transaction_finality_visibility_security_recovery_parser_reference_or_benchmark_authority"
            << '\n';
  IdleArenaDiagnosticsAndQueryStateAreReclaimed();
  BudgetsCancellationAndAuthorityFailClosed();
  PageCacheAndSpillRecordsUseRealSubsystemPaths();
  return EXIT_SUCCESS;
}
