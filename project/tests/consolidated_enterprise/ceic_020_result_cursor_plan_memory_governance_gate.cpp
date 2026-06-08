// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-020 focused validation for result cursor, plan-cache, and prepared
// statement memory governance.
#include "hierarchical_memory_budget_ledger.hpp"
#include "optimizer_plan_cache.hpp"
#include "prepared_execution_template.hpp"
#include "result_cursor_plan_memory_governance.hpp"
#include "streaming_cursor_manager.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace executor = scratchbird::engine::executor;
namespace internal = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;
namespace optimizer = scratchbird::engine::optimizer;
namespace wire = scratchbird::wire;

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

bool Contains(const std::vector<std::string>& evidence,
              std::string_view needle) {
  for (const auto& row : evidence) {
    if (row.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool ContainsReason(const std::vector<std::string>& reasons,
                    std::string_view needle) {
  return Contains(reasons, needle);
}

memory::HierarchicalMemoryScopeRef Scope(
    memory::HierarchicalMemoryScopeKind kind,
    std::string id) {
  return {kind, std::move(id)};
}

memory::HierarchicalMemoryBudgetProvenance RuntimeProvenance(
    std::string label = "ceic_020_result_cursor_plan_memory_governance") {
  memory::HierarchicalMemoryBudgetProvenance provenance;
  provenance.source =
      memory::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
  provenance.source_label = std::move(label);
  provenance.engine_mga_authoritative = true;
  provenance.memory_evidence_only = true;
  return provenance;
}

void SetBudget(memory::HierarchicalMemoryBudgetLedger* ledger,
               memory::HierarchicalMemoryScopeKind kind,
               const std::string& scope_id,
               u64 hard) {
  memory::HierarchicalMemoryBudget budget;
  budget.scope = Scope(kind, scope_id);
  budget.hard_limit_bytes = hard;
  budget.provenance = RuntimeProvenance("ceic_020_budget");
  Require(ledger->SetBudget(std::move(budget)).ok(),
          "CEIC-020 budget setup failed");
}

void SeedBudgets(memory::HierarchicalMemoryBudgetLedger* ledger,
                 const memory::ResultCursorPlanMemoryScope& scope,
                 u64 hard = 64ull * 1024ull) {
  SetBudget(ledger, memory::HierarchicalMemoryScopeKind::process,
            "local-process", hard);
  SetBudget(ledger, memory::HierarchicalMemoryScopeKind::database,
            scope.database_id, hard);
  SetBudget(ledger, memory::HierarchicalMemoryScopeKind::tenant,
            scope.tenant_id, hard);
  SetBudget(ledger, memory::HierarchicalMemoryScopeKind::user,
            scope.user_id, hard);
  SetBudget(ledger, memory::HierarchicalMemoryScopeKind::session,
            scope.session_id, hard);
  SetBudget(ledger, memory::HierarchicalMemoryScopeKind::transaction,
            scope.transaction_id, hard);
  SetBudget(ledger, memory::HierarchicalMemoryScopeKind::statement,
            scope.statement_id, hard);
  SetBudget(ledger, memory::HierarchicalMemoryScopeKind::query,
            scope.query_id, hard);
}

memory::ResultCursorPlanMemoryScope BaseScope(std::string suffix = "a") {
  memory::ResultCursorPlanMemoryScope scope;
  scope.database_id = "ceic020-db-" + suffix;
  scope.tenant_id = "ceic020-tenant-" + suffix;
  scope.user_id = "ceic020-user-" + suffix;
  scope.role_id = "ceic020-role-" + suffix;
  scope.session_id = "ceic020-session-" + suffix;
  scope.connection_id = "ceic020-connection-" + suffix;
  scope.transaction_id = "ceic020-transaction-" + suffix;
  scope.statement_id = "ceic020-statement-" + suffix;
  scope.query_id = "ceic020-query-" + suffix;
  scope.cursor_id = "ceic020-cursor-" + suffix;
  scope.plan_cache_key = "ceic020-plan-" + suffix;
  scope.prepared_statement_id = "ceic020-prepared-" + suffix;
  scope.descriptor_snapshot_id = "ceic020-descriptor-" + suffix;
  return scope;
}

memory::ResultCursorPlanMemoryEpochs Epochs(u64 base = 20) {
  memory::ResultCursorPlanMemoryEpochs epochs;
  epochs.catalog_epoch = base;
  epochs.security_epoch = base + 1;
  epochs.redaction_epoch = base + 2;
  epochs.policy_epoch = base + 3;
  epochs.resource_epoch = base + 4;
  epochs.descriptor_epoch = base + 5;
  epochs.memory_policy_epoch = base + 6;
  return epochs;
}

memory::ResultCursorPlanMemoryPolicy Policy() {
  memory::ResultCursorPlanMemoryPolicy policy;
  policy.max_result_frame_bytes = 512;
  policy.max_outstanding_frames_per_connection = 4;
  policy.max_outstanding_frames_per_session = 4;
  policy.max_outstanding_frames_per_query = 3;
  policy.max_outstanding_frames_per_cursor = 2;
  policy.max_cursor_bytes_per_connection = 4096;
  policy.max_cursor_bytes_per_session = 4096;
  policy.max_cursor_bytes_per_query = 2048;
  policy.max_plan_cache_bytes_per_database = 4096;
  policy.max_plan_cache_bytes_per_tenant = 4096;
  policy.max_plan_cache_bytes_per_user = 4096;
  policy.max_plan_cache_bytes_per_session = 2048;
  policy.max_prepared_statement_bytes_per_database = 4096;
  policy.max_prepared_statement_bytes_per_tenant = 4096;
  policy.max_prepared_statement_bytes_per_user = 4096;
  policy.max_prepared_statement_bytes_per_session = 2048;
  policy.max_descriptor_snapshot_bytes_per_database = 2048;
  policy.max_descriptor_snapshot_bytes_per_session = 1024;
  return policy;
}

memory::ResultCursorPlanMemoryLeaseRequest LeaseRequest(
    memory::HierarchicalMemoryBudgetLedger* ledger,
    memory::ResultCursorPlanMemorySurface surface,
    memory::ResultCursorPlanMemoryScope scope,
    u64 bytes) {
  memory::ResultCursorPlanMemoryLeaseRequest request;
  request.surface = surface;
  request.ledger = ledger;
  request.policy = Policy();
  request.scope = std::move(scope);
  request.epochs = Epochs();
  request.provenance = RuntimeProvenance();
  request.memory_class =
      std::string("ceic_020.") +
      memory::ResultCursorPlanMemorySurfaceName(surface);
  request.owner_id = request.memory_class + ".owner";
  request.route_label = "ceic020.route";
  request.requested_bytes = bytes;
  return request;
}

void RequireLedgerEmpty(const memory::HierarchicalMemoryBudgetLedger& ledger,
                        std::string_view label) {
  const auto snapshot = ledger.Snapshot();
  Require(snapshot.current_bytes == 0,
          std::string(label) + " leaked current bytes");
  Require(snapshot.reserved_bytes == 0,
          std::string(label) + " leaked reserved bytes");
  Require(snapshot.active_reservation_count == 0,
          std::string(label) + " leaked active reservations");
}

void DirectGovernorSurfaceLimitsAndCleanup() {
  auto scope = BaseScope("direct");
  memory::HierarchicalMemoryBudgetLedger ledger;
  SeedBudgets(&ledger, scope);
  memory::ResultCursorPlanMemoryGovernor governor;

  auto cursor = governor.Acquire(LeaseRequest(
      &ledger, memory::ResultCursorPlanMemorySurface::cursor, scope, 512));
  Require(cursor.ok(), "CEIC-020 cursor lease was refused");
  Require(Contains(cursor.evidence,
                   "CEIC-020_RESULT_CURSOR_PLAN_CACHE_PREPARED_MEMORY_GOVERNANCE"),
          "CEIC-020 cursor evidence marker missing");
  Require(Contains(cursor.evidence,
                   "security_authorization_recovery_parser_donor"),
          "CEIC-020 authority boundary evidence missing authorization");

  auto frame1 = governor.Acquire(LeaseRequest(
      &ledger, memory::ResultCursorPlanMemorySurface::result_frame, scope, 128));
  auto frame2 = governor.Acquire(LeaseRequest(
      &ledger, memory::ResultCursorPlanMemorySurface::result_frame, scope, 128));
  auto frame3 = governor.Acquire(LeaseRequest(
      &ledger, memory::ResultCursorPlanMemorySurface::result_frame, scope, 128));
  Require(frame1.ok() && frame2.ok(),
          "CEIC-020 initial result frame leases were refused");
  Require(!frame3.ok() && frame3.backpressure_required,
          "CEIC-020 result-frame backpressure did not fail closed");
  Require(governor.Snapshot().result_frame_count == 2,
          "CEIC-020 frame counter mismatch");

  auto frame_release = governor.ReleaseResultFramesByCursor(
      scope.cursor_id, memory::ResultCursorPlanMemoryReleaseReason::explicit_release);
  Require(frame_release.released_lease_count == 2 &&
              frame_release.released_bytes == 256,
          "CEIC-020 result-frame release by cursor failed");
  auto cursor_release = governor.ReleaseByCursor(
      scope.cursor_id, memory::ResultCursorPlanMemoryReleaseReason::close);
  Require(cursor_release.released_lease_count == 1,
          "CEIC-020 cursor release failed");
  Require(governor.Snapshot().active_lease_count == 0,
          "CEIC-020 active leases remained after cursor close");

  auto timeout = governor.Acquire(LeaseRequest(
      &ledger, memory::ResultCursorPlanMemorySurface::cursor, scope, 64));
  Require(timeout.ok(), "CEIC-020 timeout setup cursor lease refused");
  auto timeout_release = governor.Release(
      timeout.lease_id, memory::ResultCursorPlanMemoryReleaseReason::timeout);
  Require(timeout_release.released_lease_count == 1,
          "CEIC-020 timeout release did not release cursor lease");

  auto disconnect = governor.Acquire(LeaseRequest(
      &ledger, memory::ResultCursorPlanMemorySurface::cursor, scope, 64));
  Require(disconnect.ok(), "CEIC-020 disconnect setup cursor lease refused");
  auto disconnect_release = governor.ReleaseByConnection(
      scope.connection_id, memory::ResultCursorPlanMemoryReleaseReason::disconnect);
  Require(disconnect_release.released_lease_count == 1,
          "CEIC-020 disconnect release did not release cursor lease");

  auto rollback = governor.Acquire(LeaseRequest(
      &ledger, memory::ResultCursorPlanMemorySurface::cursor, scope, 64));
  Require(rollback.ok(), "CEIC-020 rollback setup cursor lease refused");
  auto rollback_release = governor.ReleaseByTransaction(
      scope.transaction_id, memory::ResultCursorPlanMemoryReleaseReason::rollback);
  Require(rollback_release.released_lease_count == 1,
          "CEIC-020 rollback release did not release cursor lease");

  RequireLedgerEmpty(ledger, "direct governor");
}

void ExpiryAndClusterAuthorityFailClosed() {
  auto scope = BaseScope("expiry");
  memory::HierarchicalMemoryBudgetLedger ledger;
  SeedBudgets(&ledger, scope);
  memory::ResultCursorPlanMemoryGovernor governor;

  auto expiring = LeaseRequest(
      &ledger, memory::ResultCursorPlanMemorySurface::plan_cache_entry, scope, 256);
  expiring.lease_expires_at_ms = 100;
  auto acquired = governor.Acquire(std::move(expiring));
  Require(acquired.ok(), "CEIC-020 expiring plan cache lease refused");
  auto cleanup = governor.CleanupExpiredLeases(101);
  Require(cleanup.released_lease_count == 1 && cleanup.released_bytes == 256,
          "CEIC-020 expired lease cleanup did not release the lease");
  RequireLedgerEmpty(ledger, "expired cleanup");

  auto cluster = LeaseRequest(
      &ledger, memory::ResultCursorPlanMemorySurface::plan_cache_entry, scope, 128);
  cluster.cluster_route_requested = true;
  auto cluster_result = governor.Acquire(std::move(cluster));
  Require(!cluster_result.ok(),
          "CEIC-020 cluster-local memory governance did not fail closed");

  auto unsafe = LeaseRequest(
      &ledger, memory::ResultCursorPlanMemorySurface::plan_cache_entry, scope, 128);
  unsafe.authority.parser_authority = true;
  auto unsafe_result = governor.Acquire(std::move(unsafe));
  Require(!unsafe_result.ok(),
          "CEIC-020 unsafe parser authority did not fail closed");
}

wire::StreamingCursorState StreamingState(
    memory::ResultCursorPlanMemoryGovernor* governor,
    memory::HierarchicalMemoryBudgetLedger* ledger,
    const memory::ResultCursorPlanMemoryScope& scope) {
  wire::StreamingCursorState state;
  state.cursor_id = scope.cursor_id;
  state.plan_result_contract_hash = "ceic020-result-contract";
  state.catalog_epoch = 20;
  state.descriptor_epoch = 25;
  state.transaction_snapshot_class = "engine_mga_snapshot";
  state.transaction_uuid = scope.transaction_id;
  state.local_transaction_id = 200;
  state.snapshot_visible_through_local_transaction_id = 200;
  state.security_epoch = 21;
  state.redaction_epoch = 22;
  state.route_kind = "embedded";
  state.expiry_deadline_unix_millis = 1000;
  state.memory_governor = governor;
  state.memory_ledger = ledger;
  state.memory_policy = Policy();
  state.memory_scope = scope;
  state.memory_epochs = Epochs();
  return state;
}

void StreamingCursorFramesReserveAndRelease() {
  auto scope = BaseScope("stream");
  memory::HierarchicalMemoryBudgetLedger ledger;
  SeedBudgets(&ledger, scope);
  memory::ResultCursorPlanMemoryGovernor governor;
  wire::StreamingCursorManager manager;

  wire::StreamingCursorOpenRequest open;
  open.state = StreamingState(&governor, &ledger, scope);
  open.now_unix_millis = 10;
  open.cursor_memory_bytes = 256;
  open.require_memory_governance = true;
  auto opened = manager.OpenCursor(open);
  Require(opened.ok() && !opened.state.memory_lease_id.empty(),
          "CEIC-020 streaming cursor open did not reserve cursor memory");

  wire::StreamingCursorCreditState credit;
  credit.frame_credit = 3;
  credit.row_credit = 100;
  credit.byte_credit = 2048;
  auto credited = manager.GrantCredit(scope.cursor_id, credit);
  Require(credited.ok(), "CEIC-020 cursor credit failed");

  wire::StreamingCursorFrameDelivery delivery;
  delivery.expected = wire::StreamingCursorBindingFromState(credited.state);
  delivery.row_count = 10;
  delivery.byte_count = 128;
  delivery.now_unix_millis = 20;
  delivery.require_memory_governance = true;
  auto first = manager.RecordFrameDelivery(delivery);
  Require(first.ok(), "CEIC-020 first frame delivery failed");
  delivery.expected = wire::StreamingCursorBindingFromState(first.state);
  auto second = manager.RecordFrameDelivery(delivery);
  Require(second.ok(), "CEIC-020 second frame delivery failed");
  delivery.expected = wire::StreamingCursorBindingFromState(second.state);
  auto third = manager.RecordFrameDelivery(delivery);
  Require(!third.ok() &&
              ContainsReason(third.refusal_reasons,
                             "result_frame_memory_backpressure"),
          "CEIC-020 frame delivery did not backpressure at cursor limit");

  auto released_frames = manager.GrantCredit(scope.cursor_id, credit);
  Require(released_frames.ok() &&
              released_frames.state.outstanding_frame_count == 0 &&
              released_frames.state.outstanding_frame_bytes == 0,
          "CEIC-020 credit refresh did not release outstanding frames");
  auto canceled = manager.CancelCursor(scope.cursor_id);
  Require(canceled.ok(), "CEIC-020 cursor cancel failed");
  Require(governor.Snapshot().active_lease_count == 0,
          "CEIC-020 streaming cursor leaked leases after cancel");
  RequireLedgerEmpty(ledger, "streaming cursor");
}

optimizer::OptimizerPlanCacheKeyInput PlanKey(std::string suffix = "a") {
  optimizer::OptimizerPlanCacheKeyInput input;
  input.operation_id = "dml.select_rows";
  input.sblr_digest = "sblr:ceic020:" + suffix;
  input.descriptor_set_digest = "descriptor:set:" + suffix;
  input.statistics_snapshot_id = "stats:snapshot:" + suffix;
  input.catalog_stats_digest = "stats:catalog:" + suffix;
  input.cost_profile_id = "cost:enterprise:" + suffix;
  input.executor_capability_set_id = "executor:capabilities:" + suffix;
  input.route_capability_digest = "route:capabilities:" + suffix;
  input.security_policy_digest = "security:policy:" + suffix;
  input.redaction_route_digest = "redaction:route:" + suffix;
  input.normalized_optimizer_controls_digest = "optimizer:controls:" + suffix;
  input.parameter_shape_digest = "parameter:shape:" + suffix;
  input.memory_grant_class = "memory:class:" + suffix;
  input.memory_grant_digest = "memory:grant:" + suffix;
  input.catalog_epoch = 100;
  input.stats_epoch = 101;
  input.security_epoch = 102;
  input.redaction_epoch = 103;
  input.policy_epoch = 104;
  input.resource_epoch = 105;
  input.name_resolution_epoch = 106;
  input.memory_policy_epoch = 107;
  input.memory_feedback_generation = 108;
  input.compatibility_epoch = 109;
  input.format_compatibility_epoch = 110;
  input.route_epoch = 111;
  input.object_uuids = {"object:" + suffix};
  input.dependency_digests = {
      input.descriptor_set_digest,
      input.catalog_stats_digest,
      input.security_policy_digest,
      input.memory_grant_digest,
  };
  return input;
}

optimizer::CachedOptimizerPlan Plan(std::string suffix = "a") {
  optimizer::CachedOptimizerPlan plan;
  plan.key_input = PlanKey(suffix);
  plan.cache_key = optimizer::BuildOptimizerPlanCacheKey(plan.key_input);
  plan.result.ok = true;
  plan.result.plan_id = "plan:" + suffix;
  plan.result.diagnostic_code = "SB_OPTIMIZER_PLAN_OK";
  plan.created_epoch = 99;
  plan.metadata_only = true;
  plan.mga_visibility_recheck_required = true;
  plan.security_recheck_required = true;
  return plan;
}

void OptimizerPlanCacheMemoryIsGoverned() {
  auto scope = BaseScope("plan");
  memory::HierarchicalMemoryBudgetLedger ledger;
  SeedBudgets(&ledger, scope);
  memory::ResultCursorPlanMemoryGovernor governor;
  optimizer::OptimizerPlanCache cache;

  optimizer::OptimizerPlanCacheMemoryGovernanceRequest governance;
  governance.governor = &governor;
  governance.ledger = &ledger;
  governance.policy = Policy();
  governance.scope = scope;
  governance.epochs = Epochs(100);
  governance.provenance = RuntimeProvenance("ceic_020_optimizer_plan_cache");
  governance.estimated_plan_bytes = 512;
  auto put = cache.PutEnterpriseGoverned(Plan("plan"), governance);
  Require(put.ok && Contains(put.evidence,
                             "CEIC-020_OPTIMIZER_PLAN_CACHE_MEMORY_GOVERNED"),
          "CEIC-020 governed optimizer plan cache put failed");
  Require(governor.Snapshot().plan_cache_bytes == 512,
          "CEIC-020 optimizer plan cache bytes missing");
  governance.estimated_plan_bytes = 256;
  auto replace = cache.PutEnterpriseGoverned(Plan("plan"), governance);
  Require(replace.ok && governor.Snapshot().plan_cache_bytes == 256,
          "CEIC-020 governed optimizer plan replacement leaked prior lease");
  auto shrunk = cache.ShrinkGovernedMemory(scope.database_id, 0, &governor);
  Require(shrunk.invalidated_count == 1,
          "CEIC-020 optimizer plan cache shrink did not invalidate one plan");
  Require(governor.Snapshot().plan_cache_bytes == 0,
          "CEIC-020 optimizer plan cache shrink leaked memory");
  RequireLedgerEmpty(ledger, "optimizer plan cache");
}

internal::EngineDescriptor Descriptor(std::string suffix) {
  internal::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = "descriptor-uuid-" + suffix;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "INTEGER";
  descriptor.encoded_descriptor = "int32:" + suffix;
  return descriptor;
}

executor::PreparedTemplateAdmission Admission(std::string suffix = "a") {
  executor::PreparedTemplateAdmission admission;
  admission.key.operation_id = "dml.select_rows";
  admission.key.sblr_digest_or_trace_key = "sblr:prepared:" + suffix;
  admission.key.descriptor_set_digest = "prepared:descriptor-set:" + suffix;
  admission.key.epochs.catalog_epoch = 200;
  admission.key.epochs.security_epoch = 201;
  admission.key.epochs.policy_resource_epoch = 202;
  admission.key.epochs.name_resolution_epoch = 203;
  admission.key.dependency_uuids = {"prepared-object:" + suffix};

  executor::PreparedDescriptorSlot slot;
  slot.stable_name = "slot:" + suffix;
  slot.descriptor = Descriptor(suffix);
  slot.ordinal = 0;
  admission.descriptor_slots.push_back(slot);
  admission.result_shape.result_kind = "rowset";
  admission.result_shape.columns.push_back(slot);
  admission.result_shape.digest =
      executor::PreparedResultShapeDigest(admission.result_shape);
  admission.key.result_shape_digest = admission.result_shape.digest;
  admission.policy_metadata.security_policy_digest = "security:" + suffix;
  admission.policy_metadata.visibility_policy_digest = "visibility:" + suffix;
  admission.policy_metadata.authorization_policy_digest = "authz:" + suffix;
  return admission;
}

void PreparedTemplateMemoryIsGoverned() {
  auto scope = BaseScope("prepared");
  memory::HierarchicalMemoryBudgetLedger ledger;
  SeedBudgets(&ledger, scope);
  memory::ResultCursorPlanMemoryGovernor governor;
  executor::PreparedTemplateCache cache;

  executor::PreparedTemplateMemoryGovernanceRequest governance;
  governance.governor = &governor;
  governance.ledger = &ledger;
  governance.policy = Policy();
  governance.scope = scope;
  governance.epochs = Epochs(200);
  governance.provenance = RuntimeProvenance("ceic_020_prepared_template");
  governance.estimated_template_bytes = 384;
  governance.estimated_descriptor_snapshot_bytes = 128;

  auto prepared = cache.PrepareGoverned(Admission("prepared"), governance);
  Require(prepared.ok && prepared.prepared_template != nullptr,
          "CEIC-020 governed prepared template failed");
  Require(prepared.prepared_template->memory_governed &&
              !prepared.prepared_template->prepared_memory_lease_id.empty() &&
              !prepared.prepared_template->descriptor_snapshot_memory_lease_id.empty(),
          "CEIC-020 prepared template did not retain lease IDs");
  Require(Contains(prepared.prepared_template->memory_governance_evidence,
                   "CEIC-020_PREPARED_TEMPLATE_MEMORY_GOVERNED"),
          "CEIC-020 prepared template evidence missing");
  Require(governor.Snapshot().prepared_statement_count == 1 &&
              governor.Snapshot().descriptor_snapshot_count == 1,
          "CEIC-020 prepared/descriptor snapshot counts missing");

  auto reused = cache.PrepareGoverned(Admission("prepared"), governance);
  Require(reused.ok && reused.reused_existing_template,
          "CEIC-020 governed prepared template was not reused");
  auto invalidated = cache.InvalidateGovernedByEpoch(Epochs(300), &governor);
  Require(invalidated == 1,
          "CEIC-020 governed prepared template epoch invalidation failed");
  Require(governor.Snapshot().prepared_statement_count == 0 &&
              governor.Snapshot().descriptor_snapshot_count == 0,
          "CEIC-020 governed prepared template invalidation leaked memory");
  RequireLedgerEmpty(ledger, "prepared template");
}

}  // namespace

int main() {
  DirectGovernorSurfaceLimitsAndCleanup();
  ExpiryAndClusterAuthorityFailClosed();
  StreamingCursorFramesReserveAndRelease();
  OptimizerPlanCacheMemoryIsGoverned();
  PreparedTemplateMemoryIsGoverned();

  std::cout << "CEIC-020 result cursor, plan-cache, and prepared-statement "
               "memory governance gate passed\n";
  return EXIT_SUCCESS;
}
