// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "allocator_lifetime_hot_path.hpp"

#include <utility>

namespace scratchbird::engine::executor {
namespace agents = scratchbird::core::agents;
namespace memory = scratchbird::core::memory;

namespace {

ExecutorQueryShape ShapeFor(AllocatorHotPathObjectKind kind) {
  switch (kind) {
    case AllocatorHotPathObjectKind::kRowBatch:
    case AllocatorHotPathObjectKind::kPlanNode:
    case AllocatorHotPathObjectKind::kEvidenceObject:
    case AllocatorHotPathObjectKind::kCursorFrame:
    case AllocatorHotPathObjectKind::kResultFrame:
      return ExecutorQueryShape::relational;
    case AllocatorHotPathObjectKind::kDmlLocatorStream:
      return ExecutorQueryShape::dml;
  }
  return ExecutorQueryShape::relational;
}

void AddCommonEvidence(const AllocatorHotPathRequest& request,
                       AllocatorHotPathResult* result) {
  result->evidence.push_back("allocator_hot_path.route_label=" +
                             request.route_label);
  result->evidence.push_back(
      "allocator_hot_path.primitive=query_memory_arena");
  result->evidence.push_back(
      "allocator_hot_path.mga_finality_authority=engine_transaction_inventory");
  result->evidence.push_back("allocator_hot_path.row_identity_authority=false");
  result->evidence.push_back("allocator_hot_path.visibility_authority=false");
  result->evidence.push_back("allocator_hot_path.finality_authority=false");
  result->evidence.push_back("allocator_hot_path.authorization_authority=false");
  result->evidence.push_back("allocator_hot_path.recovery_authority=false");
  result->evidence.push_back(
      std::string("allocator_hot_path.runtime_consumed=") +
      (request.runtime_consumed ? "true" : "false"));
  result->evidence.push_back(
      std::string("allocator_hot_path.transaction_inventory_proof_present=") +
      (request.authority.transaction_inventory_authoritative ? "true"
                                                             : "false"));
  result->evidence.push_back(
      std::string("allocator_hot_path.security_recheck_required=") +
      (request.authority.security_recheck_required ? "true" : "false"));
}

AllocatorHotPathResult Refuse(const AllocatorHotPathRequest& request,
                              std::string code,
                              std::string detail) {
  AllocatorHotPathResult result;
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  AddCommonEvidence(request, &result);
  result.evidence.push_back("allocator_hot_path.fail_closed=true");
  result.evidence.push_back("allocator_hot_path.refused=" +
                            result.diagnostic_code);
  return result;
}

AllocatorHotPathResult Fallback(const AllocatorHotPathRequest& request,
                                std::string code,
                                std::string detail,
                                std::vector<std::string> evidence = {}) {
  AllocatorHotPathResult result;
  result.ok = true;
  result.fallback_used = true;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  AddCommonEvidence(request, &result);
  result.evidence.push_back("allocator_hot_path.fallback_used=true");
  result.evidence.push_back("allocator_hot_path.exact_fallback_available=true");
  result.evidence.push_back("allocator_hot_path.exact_fallback_reason=" +
                            result.diagnostic_code);
  result.evidence.insert(result.evidence.end(), evidence.begin(), evidence.end());
  return result;
}

bool FamilySeen(const std::vector<AllocatorHotPathObjectRequest>& objects,
                AllocatorHotPathObjectKind kind) {
  for (const auto& object : objects) {
    if (object.kind == kind) {
      return true;
    }
  }
  return false;
}

bool AllRequiredFamiliesSeen(
    const std::vector<AllocatorHotPathObjectRequest>& objects) {
  return FamilySeen(objects, AllocatorHotPathObjectKind::kRowBatch) &&
         FamilySeen(objects, AllocatorHotPathObjectKind::kPlanNode) &&
         FamilySeen(objects, AllocatorHotPathObjectKind::kEvidenceObject) &&
         FamilySeen(objects, AllocatorHotPathObjectKind::kCursorFrame) &&
         FamilySeen(objects, AllocatorHotPathObjectKind::kResultFrame) &&
         FamilySeen(objects, AllocatorHotPathObjectKind::kDmlLocatorStream);
}

std::uint64_t Saved(std::uint64_t before, std::uint64_t after) {
  return before > after ? before - after : 0;
}

}  // namespace

const char* AllocatorHotPathObjectKindName(AllocatorHotPathObjectKind kind) {
  switch (kind) {
    case AllocatorHotPathObjectKind::kRowBatch:
      return "row_batch";
    case AllocatorHotPathObjectKind::kPlanNode:
      return "plan_node";
    case AllocatorHotPathObjectKind::kEvidenceObject:
      return "evidence_object";
    case AllocatorHotPathObjectKind::kCursorFrame:
      return "cursor_frame";
    case AllocatorHotPathObjectKind::kResultFrame:
      return "result_frame";
    case AllocatorHotPathObjectKind::kDmlLocatorStream:
      return "dml_locator_stream";
  }
  return "unknown";
}

AllocatorHotPathResult ExecuteAllocatorHotPath(
    const AllocatorHotPathRequest& request) {
  if (request.route_label.empty()) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_ROUTE_LABEL_REQUIRED",
                  "route label is required");
  }
  if (!request.runtime_consumed) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_NO_RUNTIME",
                  "allocator hot path must be runtime consumed");
  }
  if (request.result_contract_hash.empty() ||
      request.result_contract_hash != request.fallback_result_contract_hash) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_RESULT_MISMATCH",
                  "optimized and exact fallback result/evidence contract "
                  "hashes must match");
  }
  if (!request.exact_fallback_available) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_EXACT_FALLBACK_UNAVAILABLE",
                  "exact fallback is required before allocator hot-path "
                  "admission");
  }
  if (request.authority.parser_client_or_reference_allocator_authority) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_UNSAFE_AUTHORITY",
                  "parser, client, and reference allocator authority is refused");
  }
  if (request.authority.allocator_visibility_or_finality_authority ||
      request.authority.allocator_recovery_authority ||
      request.authority.allocator_authorization_authority) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_UNSAFE_AUTHORITY",
                  "allocation metadata cannot own row identity, visibility, "
                  "finality, authorization, or recovery");
  }
  if (!request.authority.engine_mga_snapshot_bound ||
      !request.authority.transaction_inventory_authoritative) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_MGA_UNPROVEN",
                  "engine MGA snapshot and transaction inventory proof are "
                  "required");
  }
  if (!request.authority.security_recheck_required) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_SECURITY_UNPROVEN",
                  "security recheck evidence is required");
  }
  if (request.arena_generation == 0 ||
      request.expected_arena_generation == 0 ||
      request.arena_generation != request.expected_arena_generation) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_STALE_GENERATION",
                  "arena generation is stale");
  }
  if (request.route_epoch == 0 || request.owner_route_epoch == 0 ||
      request.route_epoch != request.owner_route_epoch ||
      request.cross_route_ownership_transfer || request.use_after_scope_observed) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_SCOPE_UNSAFE",
                  "cross-route ownership transfer or use-after-scope is "
                  "refused");
  }
  if (!request.profiler.measured || request.profiler.source_label.empty() ||
      request.profiler.sample_count == 0 ||
      request.profiler.baseline_allocation_count <=
          request.profiler.arena_allocation_count ||
      request.profiler.baseline_allocation_bytes <
          request.profiler.arena_allocation_bytes) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_PROFILER_MISSING",
                  "measured allocation-count and allocation-byte evidence is "
                  "required");
  }
  if (!AllRequiredFamiliesSeen(request.objects)) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_OBJECT_FAMILY_MISSING",
                  "row batches, plan nodes, evidence objects, cursor frames, "
                  "result frames, and DML locator streams are all required");
  }
  if (request.arena == nullptr) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_ARENA_REQUIRED",
                  "query memory arena is required");
  }

  auto governance_request = request.resource_governance;
  governance_request.expected_family =
      agents::ResourceGovernanceFamily::kQueryMemoryArena;
  const auto governance = agents::AdmitResourceGovernance(governance_request);
  if (governance.action == agents::ResourceGovernanceAction::kFailClosed) {
    auto refused = Refuse(request,
                          "ORH_ALLOCATOR_LIFETIME_RESOURCE_REFUSED",
                          governance.diagnostic_code);
    refused.evidence.insert(refused.evidence.end(), governance.evidence.begin(),
                            governance.evidence.end());
    return refused;
  }
  if (governance.action == agents::ResourceGovernanceAction::kCancel) {
    auto refused = Refuse(request, "ORH_ALLOCATOR_LIFETIME_CANCELLED",
                          governance.diagnostic_code);
    refused.evidence.insert(refused.evidence.end(), governance.evidence.begin(),
                            governance.evidence.end());
    return refused;
  }
  if (governance.action ==
          agents::ResourceGovernanceAction::kExactScalarFallback ||
      governance.action == agents::ResourceGovernanceAction::kSlowdownDegrade) {
    return Fallback(request,
                    "ORH_ALLOCATOR_LIFETIME_RESOURCE_PRESSURE_FALLBACK",
                    governance.diagnostic_code,
                    governance.evidence);
  }

  AllocatorHotPathResult result;
  std::vector<std::string> grant_ids;
  grant_ids.reserve(request.objects.size());
  for (const auto& object : request.objects) {
    ExecutorQueryMemoryRequest memory_request;
    memory_request.shape = ShapeFor(object.kind);
    memory_request.bytes = object.bytes;
    memory_request.purpose = std::string("orh281.") +
                             AllocatorHotPathObjectKindName(object.kind) +
                             "." + object.stable_id;
    memory_request.resource_governance = request.resource_governance;
    const auto grant = RequestExecutorQueryMemory(request.arena,
                                                 std::move(memory_request));
    result.evidence.insert(result.evidence.end(), grant.evidence.begin(),
                           grant.evidence.end());
    if (!grant.ok() || !grant.arena_result.grant.has_value()) {
      auto refused = Refuse(request, "ORH_ALLOCATOR_LIFETIME_GRANT_REFUSED",
                            grant.diagnostic.diagnostic_code);
      refused.evidence.insert(refused.evidence.end(), result.evidence.begin(),
                              result.evidence.end());
      for (const auto& acquired_grant_id : grant_ids) {
        const auto release =
            ReleaseExecutorQueryMemory(request.arena, acquired_grant_id);
        refused.evidence.insert(refused.evidence.end(),
                                release.evidence.begin(),
                                release.evidence.end());
      }
      refused.evidence.push_back(
          "allocator_hot_path.partial_failure_cleanup_attempted=true");
      return refused;
    }
    const auto grant_id = grant.arena_result.grant->grant_id;
    grant_ids.push_back(grant_id);
    result.grant_ids.push_back(grant_id);
    result.evidence.push_back(
        std::string("allocator_hot_path.allocated_family=") +
        AllocatorHotPathObjectKindName(object.kind));
  }

  for (const auto& grant_id : grant_ids) {
    const auto release = ReleaseExecutorQueryMemory(request.arena, grant_id);
    result.evidence.insert(result.evidence.end(), release.evidence.begin(),
                           release.evidence.end());
    if (!release.ok()) {
      return Refuse(request, "ORH_ALLOCATOR_LIFETIME_RELEASE_REFUSED",
                    release.diagnostic.diagnostic_code);
    }
  }

  const auto snapshot = request.arena->Snapshot();
  if (snapshot.active_grant_count != 0 || snapshot.leak_count != 0 ||
      snapshot.current_bytes != 0) {
    return Refuse(request, "ORH_ALLOCATOR_LIFETIME_LEAK_DETECTED",
                  "arena grants were not released cleanly");
  }

  result.ok = true;
  result.benchmark_clean = true;
  result.diagnostic_code = "ORH_ALLOCATOR_LIFETIME_OK";
  result.detail = "allocator lifetime hot path accepted";
  result.allocation_count_saved =
      Saved(request.profiler.baseline_allocation_count,
            request.profiler.arena_allocation_count);
  result.allocation_bytes_saved =
      Saved(request.profiler.baseline_allocation_bytes,
            request.profiler.arena_allocation_bytes);
  AddCommonEvidence(request, &result);
  result.evidence.push_back("allocator_hot_path.benchmark_clean=true");
  result.evidence.push_back("allocator_hot_path.result_contract_hash=" +
                            request.result_contract_hash);
  result.evidence.push_back("allocator_hot_path.profiler_source_label=" +
                            request.profiler.source_label);
  result.evidence.push_back("allocator_hot_path.profiler_sample_count=" +
                            std::to_string(request.profiler.sample_count));
  result.evidence.push_back("allocator_hot_path.allocation_count_saved=" +
                            std::to_string(result.allocation_count_saved));
  result.evidence.push_back("allocator_hot_path.allocation_bytes_saved=" +
                            std::to_string(result.allocation_bytes_saved));
  result.evidence.push_back("allocator_hot_path.active_grants_after_release=0");
  result.evidence.push_back("allocator_hot_path.leak_count=0");
  result.evidence.push_back("allocator_hot_path.no_dangling_references=true");
  result.evidence.push_back("allocator_hot_path.cross_epoch_reuse=false");
  result.evidence.push_back("allocator_hot_path.ownership_transfer_safe=true");
  result.evidence.insert(result.evidence.end(), governance.evidence.begin(),
                         governance.evidence.end());
  return result;
}

}  // namespace scratchbird::engine::executor
