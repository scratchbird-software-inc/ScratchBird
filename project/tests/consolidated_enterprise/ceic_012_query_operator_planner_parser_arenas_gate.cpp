// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-012 focused validation for reservation-backed query/planner/parser arenas.
#include "background_memory_reclamation.hpp"
#include "hierarchical_memory_budget_ledger.hpp"
#include "reservation_backed_executor_memory_bridge.hpp"
#include "reservation_backed_memory_resource.hpp"
#include "reservation_backed_optimizer_memory_bridge.hpp"
#include "reservation_backed_planner_memory_bridge.hpp"
#include "sblr_parser_memory_handoff.hpp"
#include "sb_udr_runtime.hpp"
#include "vectorized_result_batch.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace memory = scratchbird::core::memory;
namespace executor = scratchbird::engine::executor;
namespace optimizer = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;
namespace sblr = scratchbird::engine::sblr;
namespace udr = scratchbird::udr::runtime;

using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

bool Contains(const std::vector<std::string>& evidence, std::string_view needle) {
  for (const auto& entry : evidence) {
    if (entry.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

memory::HierarchicalMemoryBudgetProvenance RuntimeProvenance() {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source =
      memory::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  provenance.source_label = "ceic_012_runtime_policy";
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

std::vector<memory::HierarchicalMemoryScopeRef> ScopeChain(
    memory::HierarchicalMemoryScopeKind leaf_kind,
    const std::string& leaf_id) {
  return {
      {memory::HierarchicalMemoryScopeKind::process, "ceic-012-process"},
      {memory::HierarchicalMemoryScopeKind::database, "ceic-012-database"},
      {memory::HierarchicalMemoryScopeKind::tenant, "ceic-012-tenant"},
      {memory::HierarchicalMemoryScopeKind::session, "ceic-012-session"},
      {memory::HierarchicalMemoryScopeKind::transaction, "ceic-012-transaction"},
      {memory::HierarchicalMemoryScopeKind::statement, "ceic-012-statement"},
      {memory::HierarchicalMemoryScopeKind::query, "ceic-012-query"},
      {leaf_kind, leaf_id},
  };
}

void SetBudget(memory::HierarchicalMemoryBudgetLedger* ledger,
               memory::HierarchicalMemoryScopeKind kind,
               const std::string& scope_id,
               u64 hard_limit) {
  memory::HierarchicalMemoryBudget budget;
  budget.scope = {kind, scope_id};
  budget.hard_limit_bytes = hard_limit;
  budget.soft_limit_bytes = 0;
  budget.provenance = RuntimeProvenance();
  Require(ledger->SetBudget(std::move(budget)).ok(),
          "CEIC-012 budget setup failed");
}

memory::ReservationBackedMemoryResourceAcquireResult AcquireResource(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::MemoryManager* manager,
    memory::ReservationBackedMemoryConsumerKind consumer,
    memory::HierarchicalMemoryScopeKind leaf_kind,
    memory::MemoryCategory category,
    const std::string& route,
    u64 bytes) {
  SetBudget(ledger, leaf_kind, route, bytes * 2);

  memory::ReservationBackedMemoryResourceRequest request;
  request.consumer_kind = consumer;
  request.reservation_ledger = ledger;
  request.memory_manager = manager;
  request.scope_chain = ScopeChain(leaf_kind, route);
  request.category = category;
  request.memory_class = "ceic_012." +
                         std::string(memory::ReservationBackedMemoryConsumerKindName(
                             consumer));
  request.requested_bytes = bytes;
  request.owner_id = route;
  request.route_label = route;
  request.operation_id = route + ".operation";
  request.purpose = route + ".scratch";
  request.provenance = RuntimeProvenance();
  request.authority.engine_mga_authoritative = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_or_policy_checked = true;
  return memory::AcquireReservationBackedMemoryResource(std::move(request));
}

Status OkStatus() {
  return {scratchbird::core::platform::StatusCode::ok,
          scratchbird::core::platform::Severity::info,
          scratchbird::core::platform::Subsystem::memory};
}

udr::UdrCallResult Ceic012UdrEntrypoint(const udr::UdrCallInput& input) {
  return {true,
          "udr-result:" + input.payload,
          "{\"diagnostic\":\"UDR.OK\",\"ceic_012\":\"entrypoint_invoked\"}"};
}

void ValidateResourceReleased(const memory::HierarchicalMemoryBudgetLedger& ledger,
                              const char* label) {
  const auto snapshot = ledger.Snapshot();
  Require(snapshot.current_bytes == 0, std::string(label) + " leaked current bytes");
  Require(snapshot.reserved_bytes == 0, std::string(label) + " leaked reserved bytes");
  Require(snapshot.active_reservation_count == 0,
          std::string(label) + " leaked active reservations");
}

}  // namespace

int main() {
  memory::AllocationPolicy policy;
  policy.policy_name = "ceic_012_reservation_backed_resource";
  policy.byte_limit = 2ull * 1024ull * 1024ull;
  policy.hard_limit_bytes = 2ull * 1024ull * 1024ull;
  policy.track_allocations = true;

  memory::MemoryManager manager(policy);
  memory::HierarchicalMemoryBudgetLedger ledger;
  SetBudget(&ledger,
            memory::HierarchicalMemoryScopeKind::process,
            "ceic-012-process",
            2ull * 1024ull * 1024ull);

  auto executor_resource = AcquireResource(
      &ledger,
      &manager,
      memory::ReservationBackedMemoryConsumerKind::executor_operator,
      memory::HierarchicalMemoryScopeKind::operator_scope,
      memory::MemoryCategory::executor_query_reserved,
      "ceic-012-executor-operator",
      4096);
  Require(executor_resource.ok(), "CEIC-012 executor resource acquisition failed");
  executor::ExecutorOperatorMemoryAuthority executor_authority;
  executor_authority.engine_mga_snapshot_bound = true;
  executor_authority.transaction_inventory_authoritative = true;
  executor_authority.security_recheck_required = true;
  const auto executor_result =
      executor::AllocateExecutorOperatorFromReservedResource(
          executor_resource.resource.get(),
          executor::ExecutorMemoryOperatorKind::hash_join,
          512,
          "hash_join.scratch",
          executor_authority);
  Require(executor_result.ok(), "CEIC-012 executor allocation failed");
  Require(Contains(executor_result.evidence, "after_reservation=true"),
          "CEIC-012 executor missing after-reservation evidence");
  Require(executor_resource.resource->Release().ok(),
          "CEIC-012 executor resource release failed");
  ValidateResourceReleased(ledger, "executor");

  auto planner_resource = AcquireResource(
      &ledger,
      &manager,
      memory::ReservationBackedMemoryConsumerKind::planner_temporary,
      memory::HierarchicalMemoryScopeKind::query,
      memory::MemoryCategory::executor_query_reserved,
      "ceic-012-planner",
      4096);
  Require(planner_resource.ok(), "CEIC-012 planner resource acquisition failed");
  const auto planner_result =
      planner::BuildPlannerTemporaryWorkFromReservedResource(
          planner_resource.resource.get(),
          "ceic-012-planner",
          8,
          false,
          false);
  Require(planner_result.ok(), "CEIC-012 planner temporary work failed");
  Require(Contains(planner_result.evidence, "resource_passed=true"),
          "CEIC-012 planner missing resource evidence");
  Require(planner_resource.resource->Release().ok(),
          "CEIC-012 planner resource release failed");
  ValidateResourceReleased(ledger, "planner");

  auto optimizer_resource = AcquireResource(
      &ledger,
      &manager,
      memory::ReservationBackedMemoryConsumerKind::optimizer_temporary,
      memory::HierarchicalMemoryScopeKind::query,
      memory::MemoryCategory::executor_query_reserved,
      "ceic-012-optimizer",
      4096);
  Require(optimizer_resource.ok(), "CEIC-012 optimizer resource acquisition failed");
  const auto optimizer_result =
      optimizer::BuildOptimizerTemporaryWorkFromReservedResource(
          optimizer_resource.resource.get(),
          "ceic-012-optimizer",
          8,
          true,
          false,
          false);
  Require(optimizer_result.ok(), "CEIC-012 optimizer temporary work failed");
  Require(Contains(optimizer_result.evidence, "after_reservation=true"),
          "CEIC-012 optimizer missing after-reservation evidence");
  Require(optimizer_resource.resource->Release().ok(),
          "CEIC-012 optimizer resource release failed");
  ValidateResourceReleased(ledger, "optimizer");

  auto parser_resource = AcquireResource(
      &ledger,
      &manager,
      memory::ReservationBackedMemoryConsumerKind::parser_handoff,
      memory::HierarchicalMemoryScopeKind::statement,
      memory::MemoryCategory::parser_handoff_reserved,
      "ceic-012-parser-handoff",
      4096);
  Require(parser_resource.ok(), "CEIC-012 parser resource acquisition failed");
  const auto parser_result = sblr::BuildSblrParserHandoffBuffer(
      parser_resource.resource.get(),
      "dml.select_rows",
      "sblr-envelope-payload",
      true,
      false,
      false);
  Require(parser_result.ok(), "CEIC-012 parser handoff failed");
  Require(Contains(parser_result.evidence, "translation_buffer_only"),
          "CEIC-012 parser missing translation-only evidence");
  Require(parser_resource.resource->Release().ok(),
          "CEIC-012 parser resource release failed");
  ValidateResourceReleased(ledger, "parser");

  udr::ResetRuntimeForTest();
  udr::UdrPackageDescriptor descriptor;
  descriptor.package_uuid = "ceic-012-udr";
  descriptor.package_name = "CEIC 012 UDR";
  descriptor.abi_version = "sb_udr_v1";
  descriptor.source_revision = "ceic-012";
  descriptor.binary_hash = "sha256:ceic012";
  descriptor.signature_policy = "test-valid-signature";
  descriptor.capability_role = "test.udr";
  descriptor.trusted_cpp = true;
  descriptor.entrypoints.push_back({"invoke", "test.invoke", &Ceic012UdrEntrypoint});
  Require(udr::RegisterPackage(descriptor).ok,
          "CEIC-012 UDR package registration failed");
  Require(udr::LoadPackage(descriptor.package_uuid).ok,
          "CEIC-012 UDR package load failed");

  auto udr_resource = AcquireResource(
      &ledger,
      &manager,
      memory::ReservationBackedMemoryConsumerKind::udr_invocation,
      memory::HierarchicalMemoryScopeKind::plugin,
      memory::MemoryCategory::udr_reserved,
      "ceic-012-udr",
      4096);
  Require(udr_resource.ok(), "CEIC-012 UDR resource acquisition failed");
  const auto udr_result = udr::InvokePackageWithReservedWorkspace(
      {descriptor.package_uuid, "invoke", "payload", "context"},
      udr_resource.resource.get(),
      256,
      true,
      false,
      false);
  Require(udr_result.ok, "CEIC-012 UDR reserved workspace invocation failed");
  Require(udr_resource.resource->Release().ok(),
          "CEIC-012 UDR resource release failed");
  ValidateResourceReleased(ledger, "udr");

  auto background_resource = AcquireResource(
      &ledger,
      &manager,
      memory::ReservationBackedMemoryConsumerKind::background_maintenance,
      memory::HierarchicalMemoryScopeKind::background,
      memory::MemoryCategory::cleanup,
      "ceic-012-background",
      4096);
  Require(background_resource.ok(),
          "CEIC-012 background resource acquisition failed");
  memory::BackgroundMemoryReclamationPolicy reclamation_policy;
  memory::BackgroundMemoryReclamationRequest reclamation_request;
  reclamation_request.route_label = "ceic-012-background";
  reclamation_request.operation_id = "ceic-012-background.operation";
  reclamation_request.engine_mga_authoritative = true;
  memory::BackgroundMemoryReclamationWorkItem work_item;
  work_item.kind = memory::BackgroundMemoryReclamationWorkKind::completed_query_state;
  work_item.label = "completed-query-state";
  work_item.estimated_reclaim_bytes = 128;
  work_item.reclaim = [](std::vector<std::string>* evidence) {
    evidence->push_back("ceic_012.background_callback=true");
    return OkStatus();
  };
  reclamation_request.work_items.push_back(std::move(work_item));
  const auto background_result =
      memory::RunBackgroundMemoryReclamationWithReservedResource(
          reclamation_policy,
          reclamation_request,
          background_resource.resource.get(),
          256);
  Require(background_result.ok(), "CEIC-012 background reclamation failed");
  Require(Contains(background_result.evidence, "reserved_resource_passed=true"),
          "CEIC-012 background missing resource evidence");
  Require(background_resource.resource->Release().ok(),
          "CEIC-012 background resource release failed");
  ValidateResourceReleased(ledger, "background");

  auto result_frame_resource = AcquireResource(
      &ledger,
      &manager,
      memory::ReservationBackedMemoryConsumerKind::result_frame,
      memory::HierarchicalMemoryScopeKind::operator_scope,
      memory::MemoryCategory::executor_query_reserved,
      "ceic-012-result-frame",
      4096);
  Require(result_frame_resource.ok(),
          "CEIC-012 result-frame resource acquisition failed");
  const u64 row_count = 4;
  const auto validity =
      executor::MakeResultBatchValidityBitmap(row_count, {});
  std::vector<std::uint8_t> fixed_data(4 * sizeof(int), 0x2a);
  std::vector<executor::ResultBatchColumn> columns;
  columns.push_back(executor::MakeFixedWidthResultBatchColumn(
      "value",
      row_count,
      sizeof(int),
      fixed_data,
      validity));
  const auto result_frame =
      executor::FinalizeVectorizedResultBatchFromReservedResource(
          result_frame_resource.resource.get(),
          row_count,
          std::move(columns),
          true,
          false,
          false,
          false);
  Require(result_frame.ok(), "CEIC-012 result-frame finalization failed");
  Require(Contains(result_frame.evidence, "reserved_resource_passed=true"),
          "CEIC-012 result-frame missing resource evidence");
  Require(result_frame_resource.resource->Release().ok(),
          "CEIC-012 result-frame resource release failed");
  ValidateResourceReleased(ledger, "result-frame");

  const auto missing_executor =
      executor::AllocateExecutorOperatorFromReservedResource(
          nullptr,
          executor::ExecutorMemoryOperatorKind::scan,
          64,
          "missing.resource",
          executor_authority);
  Require(!missing_executor.ok() && missing_executor.fail_closed,
          "CEIC-012 missing executor resource did not fail closed");

  auto unsafe_request = memory::ReservationBackedMemoryResourceRequest{};
  unsafe_request.consumer_kind =
      memory::ReservationBackedMemoryConsumerKind::parser_handoff;
  unsafe_request.reservation_ledger = &ledger;
  unsafe_request.memory_manager = &manager;
  unsafe_request.scope_chain =
      ScopeChain(memory::HierarchicalMemoryScopeKind::statement, "ceic-012-unsafe");
  unsafe_request.category = memory::MemoryCategory::parser_handoff_reserved;
  unsafe_request.memory_class = "ceic_012.unsafe";
  unsafe_request.requested_bytes = 1024;
  unsafe_request.owner_id = "ceic-012-unsafe";
  unsafe_request.route_label = "ceic-012-unsafe";
  unsafe_request.operation_id = "ceic-012-unsafe.operation";
  unsafe_request.provenance = RuntimeProvenance();
  unsafe_request.authority.parser_or_donor_finality_authority = true;
  const auto unsafe_resource =
      memory::AcquireReservationBackedMemoryResource(std::move(unsafe_request));
  Require(!unsafe_resource.ok() && unsafe_resource.fail_closed,
          "CEIC-012 unsafe parser/donor authority did not fail closed");
  ValidateResourceReleased(ledger, "unsafe-authority");

  auto small_resource = AcquireResource(
      &ledger,
      &manager,
      memory::ReservationBackedMemoryConsumerKind::executor_operator,
      memory::HierarchicalMemoryScopeKind::operator_scope,
      memory::MemoryCategory::executor_query_reserved,
      "ceic-012-small",
      64);
  Require(small_resource.ok(), "CEIC-012 small resource acquisition failed");
  const auto overflow =
      executor::AllocateExecutorOperatorFromReservedResource(
          small_resource.resource.get(),
          executor::ExecutorMemoryOperatorKind::sort,
          128,
          "reservation.overflow",
          executor_authority);
  Require(!overflow.ok() && overflow.fail_closed,
          "CEIC-012 reservation overflow did not fail closed");
  Require(small_resource.resource->Release().ok(),
          "CEIC-012 small resource release failed");
  ValidateResourceReleased(ledger, "overflow");

  {
    auto raii_resource = AcquireResource(
        &ledger,
        &manager,
        memory::ReservationBackedMemoryConsumerKind::executor_operator,
        memory::HierarchicalMemoryScopeKind::operator_scope,
        memory::MemoryCategory::executor_query_reserved,
        "ceic-012-raii",
        512);
    Require(raii_resource.ok(), "CEIC-012 RAII resource acquisition failed");
    const auto raii_allocation =
        executor::AllocateExecutorOperatorFromReservedResource(
            raii_resource.resource.get(),
            executor::ExecutorMemoryOperatorKind::scan,
            128,
            "raii.cleanup",
            executor_authority);
    Require(raii_allocation.ok(), "CEIC-012 RAII allocation failed");
  }
  ValidateResourceReleased(ledger, "raii");

  const auto memory_snapshot = manager.Snapshot();
  Require(memory_snapshot.current_bytes == 0,
          "CEIC-012 MemoryManager leaked bytes after resource releases");
  Require(memory_snapshot.active_allocation_count == 0,
          "CEIC-012 MemoryManager leaked allocations after resource releases");

  return 0;
}
