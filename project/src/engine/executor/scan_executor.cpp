// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cctype>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic Refusal(std::string code,
                                    std::string detail = {},
                                    const std::size_t candidate = 0) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  diagnostic.row_index = candidate;
  return diagnostic;
}

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

bool IsKnownSource(const CanonicalScanCandidateSource source) {
  return source == CanonicalScanCandidateSource::kRelationPage ||
         source == CanonicalScanCandidateSource::kIndexEntry;
}

bool IsKnownVisibility(const CanonicalMgaVisibilityDecision decision) {
  return decision == CanonicalMgaVisibilityDecision::kVisible ||
         decision == CanonicalMgaVisibilityDecision::kInvisible ||
         decision == CanonicalMgaVisibilityDecision::kIndeterminate;
}

bool IsKnownSecurity(const CanonicalMgaSecurityDecision decision) {
  return decision == CanonicalMgaSecurityDecision::kAllowed ||
         decision == CanonicalMgaSecurityDecision::kDenied ||
         decision == CanonicalMgaSecurityDecision::kIndeterminate;
}

bool IsKnownResidualTruth(
    const scratchbird::engine::internal_api::EngineSqlTruthValue truth) {
  using scratchbird::engine::internal_api::EngineSqlTruthValue;
  return truth == EngineSqlTruthValue::false_value ||
         truth == EngineSqlTruthValue::true_value ||
         truth == EngineSqlTruthValue::unknown;
}

}  // namespace

// QOW-SOURCE-QRY-004-ACCESS-V1
// Executes exactly one causally selected scan access node.  Index and relation
// access produce candidates only: engine-owned MGA visibility, security, exact
// locator identity, and residual SQL truth are rechecked before publication.
// This operator never acquires transaction, recovery, parser, index-finality,
// cache-finality, or WAL authority.
CanonicalScanAccessResult ExecuteCanonicalSelectedScanAccess(
    const CanonicalScanAccessRequest& request) {
  using scratchbird::engine::internal_api::EngineSqlTruthValue;

  CanonicalScanAccessResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic,
                          const bool replan = false) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    result.replan_required = replan;
    return result;
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (request.selected_physical_node_id == 0 || selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kScan ||
      !selected_node->input_physical_node_ids.empty()) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected access is not a leaf scan node"));
  }

  const bool index_scan = selected_node->implementation_id == "scan.index.v1";
  const bool relation_scan =
      selected_node->implementation_id == "scan.heap.v1";
  if ((!index_scan && !relation_scan) ||
      request.available_implementation_id !=
          selected_node->implementation_id) {
    return refuse(
        Refusal("QOW-DIAG-QRY-004-SCAN-IMPLEMENTATION-UNAVAILABLE-V1",
                "selected scan implementation is unavailable"),
        true);
  }

  if (!IsCanonicalUuid(request.relation_uuid)) {
    return refuse(Refusal("SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                          "relation UUID is not canonical"));
  }
  if (request.selected_descriptor_generation == 0 ||
      request.current_descriptor_generation == 0 ||
      request.selected_descriptor_generation !=
          request.current_descriptor_generation) {
    return refuse(
        Refusal(index_scan ? "SB_DIAG_MGA_READ_INDEX_DESCRIPTOR_INVALID"
                           : "SB_DIAG_MGA_READ_RELATION_DESCRIPTOR_INVALID",
                "selected access descriptor generation requires replanning"),
        true);
  }
  if (request.maximum_candidate_count == 0 ||
      request.candidates.size() > request.maximum_candidate_count) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "scan candidate bound was exceeded"));
  }

  result.accepted_record_uuids.reserve(request.candidates.size());
  result.accepted_row_version_ids.reserve(request.candidates.size());
  result.counters.candidate_count = request.candidates.size();
  std::unordered_set<std::string> candidate_uuids;
  for (std::size_t index = 0; index < request.candidates.size(); ++index) {
    const auto& candidate = request.candidates[index];
    if (!IsCanonicalUuid(candidate.candidate_uuid) ||
        !candidate_uuids.insert(candidate.candidate_uuid).second ||
        !IsCanonicalUuid(candidate.record_uuid) ||
        !IsCanonicalUuid(candidate.relation_uuid) ||
        candidate.relation_uuid != request.relation_uuid ||
        !IsCanonicalUuid(candidate.visibility_decision_uuid) ||
        candidate.creator_local_transaction_id == 0 ||
        candidate.row_version_id == 0 ||
        candidate.candidate_generation == 0 ||
        candidate.observed_generation == 0 ||
        !IsKnownSource(candidate.source)) {
      return refuse(Refusal("SB_DIAG_MGA_READ_CANDIDATE_INVALID",
                            "scan candidate identity is not bound", index));
    }
    if ((index_scan &&
         candidate.source != CanonicalScanCandidateSource::kIndexEntry) ||
        (relation_scan &&
         candidate.source != CanonicalScanCandidateSource::kRelationPage)) {
      return refuse(Refusal("SB_DIAG_MGA_READ_CANDIDATE_INVALID",
                            "candidate source does not match selected access",
                            index));
    }
    if (!IsKnownVisibility(candidate.visibility) ||
        candidate.visibility ==
            CanonicalMgaVisibilityDecision::kIndeterminate) {
      return refuse(Refusal("SB_DIAG_MGA_READ_VISIBILITY_DECISION_INVALID",
                            "visibility decision is invalid or indeterminate",
                            index));
    }
    if (candidate.visibility == CanonicalMgaVisibilityDecision::kVisible &&
        !CanonicalMgaCreatorVisibleToStatement(
            request.mga_authority.statement_context,
            candidate.creator_local_transaction_id)) {
      return refuse(Refusal(
          "SB_DIAG_MGA_READ_VISIBILITY_DECISION_INVALID",
          "visible scan candidate contradicts captured MGA vector", index));
    }
    if (!IsKnownSecurity(candidate.security_decision) ||
        candidate.security_decision ==
            CanonicalMgaSecurityDecision::kIndeterminate) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-SCAN-SECURITY-DECISION-V1",
          "security decision is invalid or indeterminate", index));
    }
    if (!IsKnownResidualTruth(candidate.residual_truth)) {
      return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                            "scan residual truth is unbound", index));
    }

    ++result.counters.visibility_recheck_count;
    if (candidate.visibility == CanonicalMgaVisibilityDecision::kInvisible) {
      ++result.counters.invisible_filtered_count;
      continue;
    }

    const bool locator_is_current =
        candidate.locator_identity_matches &&
        candidate.candidate_generation == candidate.observed_generation;
    if (!locator_is_current) {
      if (!index_scan) {
        return refuse(Refusal("SB_DIAG_MGA_READ_CANDIDATE_INVALID",
                              "relation-page candidate locator is stale",
                              index));
      }
      ++result.counters.stale_index_filtered_count;
      continue;
    }
    if (candidate.security_decision ==
        CanonicalMgaSecurityDecision::kDenied) {
      ++result.counters.security_filtered_count;
      continue;
    }
    if (candidate.residual_truth != EngineSqlTruthValue::true_value) {
      ++result.counters.residual_filtered_count;
      continue;
    }
    result.accepted_record_uuids.push_back(candidate.record_uuid);
    result.accepted_row_version_ids.push_back(candidate.row_version_id);
    ++result.counters.emitted_count;
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.authority.engine_mga_snapshot_bound = true;
  result.authority.visibility_rechecks_complete =
      result.counters.visibility_recheck_count ==
      result.counters.candidate_count;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-004-HEAP-MGA-V1 lives with the bounded MGA relation facade.
// Keeping that adapter in the internal-API translation unit preserves this
// executor's contract-only link profile while the production path remains a
// typed executor entrypoint declared by descriptor_value_runtime.hpp.

}  // namespace scratchbird::engine::executor
