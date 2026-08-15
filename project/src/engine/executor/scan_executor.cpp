// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"
#include "indexed_physical_operator.hpp"

#include "index_key_encoding.hpp"
#include "uuid.hpp"

#include <cctype>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::executor {

// The RCP-017 index-storage carriers deliberately remain source-local.  The
// direct QRY-004 proof declares the same standard-layout records, avoiding a
// second public execution ABI before the multi-node dispatch packet owns that
// activation surface.
struct CanonicalIndexStorageResolvedRowV1 {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalScanCandidateEvidence candidate;
  std::string version_uuid;
  bool engine_mga_visibility_rechecked = false;
  bool engine_security_rechecked = false;
  bool engine_residual_rechecked = false;
};

struct CanonicalSelectedIndexStorageRequestV1 {
  TypedPhysicalNodeDag physical_dag;
  std::uint64_t selected_physical_node_id = 0;
  std::string selected_alternative_uuid;
  std::string selected_index_uuid;
  std::string available_implementation_id;
  std::string relation_uuid;
  CanonicalExecutionMgaAuthority mga_authority;
  std::uint64_t selected_descriptor_generation = 0;
  std::uint64_t current_descriptor_generation = 0;
  std::vector<std::string> selected_key_descriptor_uuids;
  std::string selected_key_profile_id;
  std::vector<scratchbird::core::index::IndexKeyEncodingComponent>
      point_key_components;
  scratchbird::core::index::IndexKeySemanticProfile key_profile;
  const scratchbird::storage::page::IndexBtreePhysicalTree* physical_tree =
      nullptr;
  std::size_t maximum_candidate_count = 0;
  std::function<bool()> cancellation_requested;
  std::function<CanonicalIndexStorageResolvedRowV1(
      const IndexedPhysicalOperatorLocator&)>
      resolve_engine_row_version;
  std::string heap_fallback_alternative_uuid;
  bool physical_tree_engine_owned = false;
  bool resolver_engine_owned = false;
  bool selected_index_is_approximate = false;
  bool exact_fallback_recheck_authorized = false;
};

struct CanonicalSelectedIndexStorageResultV1 {
  DescriptorRuntimeDiagnostic diagnostic;
  CanonicalScanAccessResult scan_result;
  std::vector<scratchbird::core::platform::byte> encoded_point_key;
  std::string selected_alternative_uuid;
  std::string selected_index_uuid;
  std::size_t physical_locator_count = 0;
  std::size_t resolved_row_version_count = 0;
  bool exact_key_encoded = false;
  bool exact_selected_index_bound = false;
  bool data_access_observation_known = false;
  bool data_access_observed = false;
  bool exact_fallback_recheck_applied = false;
  bool governed_heap_replan_required = false;
  std::string governed_heap_fallback_alternative_uuid;
};

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

bool IsCanonicalIndexImplementation(const std::string_view implementation_id) {
  if (!implementation_id.starts_with("scan.index.") ||
      implementation_id.size() <= std::string_view("scan.index.").size() ||
      implementation_id.size() > 128) {
    return false;
  }
  return std::ranges::all_of(
      implementation_id, [](const unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
               ch == '.' || ch == '_' || ch == '-';
      });
}

}  // namespace

// QOW-SOURCE-QRY-004-ACCESS-V1
// QOW-SOURCE-QRY-007-SCAN-V1
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
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kScan ||
      !selected_node->input_physical_node_ids.empty()) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected access is not a leaf scan node"));
  }

  const bool index_scan =
      IsCanonicalIndexImplementation(selected_node->implementation_id);
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

// QOW-SOURCE-QRY-004-INDEX-STORAGE-V1
// Executes one exact optimizer-selected physical index point lookup.  The
// canonical key codec and physical B-tree produce locators only; an
// engine-owned resolver must then recheck MGA row-version visibility,
// security, exact locator identity, and residual SQL truth before any row can
// be published.  An unavailable selected index requests a governed replan to
// the already-published heap alternative and is never silently substituted.
CanonicalSelectedIndexStorageResultV1
ExecuteCanonicalSelectedIndexStorageAccessV1(
    const CanonicalSelectedIndexStorageRequestV1& request) {
  namespace idx = scratchbird::core::index;
  namespace uuid = scratchbird::core::uuid;

  CanonicalSelectedIndexStorageResultV1 result;
  const auto fallback_available =
      IsCanonicalUuid(request.heap_fallback_alternative_uuid) &&
      request.heap_fallback_alternative_uuid !=
          request.selected_alternative_uuid;
  const auto refuse = [&](std::string code,
                          std::string detail,
                          const bool selected_index_unavailable = false) {
    const bool observation_known = result.data_access_observation_known;
    const bool access_observed = result.data_access_observed;
    const auto encoded_key = result.encoded_point_key;
    const auto selected_alternative_uuid = result.selected_alternative_uuid;
    const auto selected_index_uuid = result.selected_index_uuid;
    const auto physical_locator_count = result.physical_locator_count;
    const auto resolved_row_version_count = result.resolved_row_version_count;
    const bool exact_selected_index_bound =
        result.exact_selected_index_bound;
    result = {};
    result.diagnostic =
        Refusal(std::move(code), std::move(detail));
    result.encoded_point_key = encoded_key;
    result.selected_alternative_uuid = selected_alternative_uuid;
    result.selected_index_uuid = selected_index_uuid;
    result.physical_locator_count = physical_locator_count;
    result.resolved_row_version_count = resolved_row_version_count;
    result.exact_key_encoded = !encoded_key.empty();
    result.exact_selected_index_bound = exact_selected_index_bound;
    result.data_access_observation_known = observation_known;
    result.data_access_observed = access_observed;
    result.governed_heap_replan_required =
        selected_index_unavailable && fallback_available;
    if (result.governed_heap_replan_required) {
      result.governed_heap_fallback_alternative_uuid =
          request.heap_fallback_alternative_uuid;
    }
    return result;
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code,
                  authority_validation.detail);
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kScan ||
      !selected_node->input_physical_node_ids.empty() ||
      !IsCanonicalIndexImplementation(selected_node->implementation_id) ||
      selected_node->selected_alternative_uuid !=
          request.selected_alternative_uuid ||
      !IsCanonicalUuid(request.selected_alternative_uuid) ||
      !IsCanonicalUuid(request.selected_index_uuid)) {
    return refuse("QOW-DIAG-QRY-004-INDEX-SELECTION-V1",
                  "selected physical index identity is not exact");
  }
  if (request.available_implementation_id !=
          selected_node->implementation_id ||
      !request.physical_tree_engine_owned || request.physical_tree == nullptr ||
      !request.physical_tree->index_uuid.valid() ||
      uuid::UuidToString(request.physical_tree->index_uuid.value) !=
          request.selected_index_uuid) {
    return refuse(
        "QOW-DIAG-QRY-004-SCAN-IMPLEMENTATION-UNAVAILABLE-V1",
        "selected physical index implementation or tree is unavailable",
        true);
  }
  if (request.selected_descriptor_generation == 0 ||
      request.current_descriptor_generation == 0 ||
      request.selected_descriptor_generation !=
          request.current_descriptor_generation) {
    return refuse("SB_DIAG_MGA_READ_INDEX_DESCRIPTOR_INVALID",
                  "selected index descriptor generation requires replanning",
                  true);
  }
  if (request.maximum_candidate_count == 0 ||
      request.point_key_components.empty() ||
      request.point_key_components.size() > 64 ||
      request.selected_key_descriptor_uuids.size() !=
          request.point_key_components.size() ||
      request.selected_key_profile_id.empty() ||
      request.selected_key_profile_id != request.key_profile.profile_id ||
      !request.key_profile.bytewise_stable ||
      !request.cancellation_requested ||
      !request.resolve_engine_row_version || !request.resolver_engine_owned) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "bounded key, cancellation, and engine resolver are required");
  }
  for (std::size_t ordinal = 0;
       ordinal < request.point_key_components.size(); ++ordinal) {
    const auto& component = request.point_key_components[ordinal];
    if (!IsCanonicalUuid(request.selected_key_descriptor_uuids[ordinal]) ||
        !component.type_descriptor_uuid.valid() ||
        uuid::UuidToString(component.type_descriptor_uuid.value) !=
            request.selected_key_descriptor_uuids[ordinal] ||
        component.ordinal != ordinal || component.type_descriptor_epoch == 0) {
      return refuse("QOW-DIAG-QRY-004-INDEX-KEY-IDENTITY-V1",
                    "selected index key descriptor identity is not exact",
                    true);
    }
  }
  if (request.selected_index_is_approximate &&
      !request.exact_fallback_recheck_authorized) {
    return refuse("QOW-DIAG-OPT-004-CAPABILITY-V1",
                  "approximate index lacks exact residual fallback authority",
                  true);
  }
  if (request.cancellation_requested()) {
    return refuse("QOW-DIAG-QRY-004-INDEX-CANCELLED-V1",
                  "index access cancelled before physical read");
  }

  const auto encoded =
      idx::EncodeIndexKey(request.point_key_components, request.key_profile);
  if (!encoded.ok() || encoded.encoded.empty()) {
    return refuse(encoded.diagnostic.diagnostic_code.empty()
                      ? "QOW-DIAG-QRY-004-INDEX-KEY-ENCODING-V1"
                      : encoded.diagnostic.diagnostic_code,
                  "canonical point-key encoding was refused");
  }
  if ((encoded.lossy || encoded.requires_recheck) &&
      !request.exact_fallback_recheck_authorized) {
    return refuse("QOW-DIAG-OPT-004-CAPABILITY-V1",
                  "lossy encoded key lacks exact residual fallback authority",
                  true);
  }
  result.encoded_point_key = encoded.encoded;
  result.exact_key_encoded = true;
  result.exact_selected_index_bound = true;
  result.selected_alternative_uuid = request.selected_alternative_uuid;
  result.selected_index_uuid = request.selected_index_uuid;

  IndexedPhysicalOperatorRequest physical;
  physical.kind = IndexedPhysicalOperatorKind::point_lookup;
  physical.physical_tree = request.physical_tree;
  physical.encoded_point_key = result.encoded_point_key;
  physical.plan_safe = true;
  physical.physical_tree_available = true;
  physical.encoded_key_proof = true;
  physical.durable_mga_inventory_proof = true;
  physical.mga_visibility_recheck_planned = true;
  physical.security_recheck_planned = true;
  physical.parser_or_reference_authority = false;
  physical.index_or_cache_finality_authority = false;

  // Crossing the physical operator boundary is observable even when the
  // point lookup returns no locators or a post-entry validation refuses.
  result.data_access_observation_known = true;
  result.data_access_observed = true;
  const auto physical_result = ExecuteIndexedPhysicalOperator(physical);
  if (!physical_result.ok) {
    return refuse(physical_result.diagnostic_code.empty()
                      ? "QOW-DIAG-QRY-004-INDEX-PHYSICAL-READ-V1"
                      : physical_result.diagnostic_code,
                  physical_result.diagnostic_detail,
                  true);
  }
  result.physical_locator_count = physical_result.locators.size();
  if (physical_result.locators.size() > request.maximum_candidate_count) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "physical index locator bound was exceeded");
  }

  std::vector<CanonicalScanCandidateEvidence> candidates;
  candidates.reserve(physical_result.locators.size());
  for (std::size_t ordinal = 0;
       ordinal < physical_result.locators.size(); ++ordinal) {
    if (request.cancellation_requested()) {
      return refuse("QOW-DIAG-QRY-004-INDEX-CANCELLED-V1",
                    "index access cancelled during row-version recheck");
    }
    const auto& locator = physical_result.locators[ordinal];
    if (!locator.from_physical_index || !locator.mga_recheck_required ||
        !locator.security_recheck_required ||
        locator.encoded_key != result.encoded_point_key) {
      return refuse("SB_DIAG_MGA_READ_CANDIDATE_INVALID",
                    "physical index locator lost exact recheck identity");
    }
    auto resolved = request.resolve_engine_row_version(locator);
    if (!resolved.diagnostic.ok) {
      return refuse(resolved.diagnostic.diagnostic_code,
                    resolved.diagnostic.detail);
    }
    const bool exact_locator_identity =
        resolved.candidate.record_uuid == locator.row_uuid &&
        resolved.version_uuid == locator.version_uuid &&
        resolved.candidate.relation_uuid == request.relation_uuid &&
        resolved.candidate.source ==
            CanonicalScanCandidateSource::kIndexEntry &&
        resolved.candidate.locator_identity_matches;
    if (!exact_locator_identity ||
        !resolved.engine_mga_visibility_rechecked ||
        !resolved.engine_security_rechecked ||
        !resolved.engine_residual_rechecked) {
      return refuse("SB_DIAG_MGA_READ_CANDIDATE_INVALID",
                    "engine row-version resolver did not preserve locator, "
                    "MGA, security, and residual identity");
    }
    candidates.push_back(std::move(resolved.candidate));
    ++result.resolved_row_version_count;
  }

  CanonicalScanAccessRequest scan_request;
  scan_request.physical_dag = request.physical_dag;
  scan_request.selected_physical_node_id =
      request.selected_physical_node_id;
  scan_request.available_implementation_id =
      request.available_implementation_id;
  scan_request.relation_uuid = request.relation_uuid;
  scan_request.mga_authority = request.mga_authority;
  scan_request.selected_descriptor_generation =
      request.selected_descriptor_generation;
  scan_request.current_descriptor_generation =
      request.current_descriptor_generation;
  scan_request.candidates = std::move(candidates);
  scan_request.maximum_candidate_count = request.maximum_candidate_count;
  result.scan_result = ExecuteCanonicalSelectedScanAccess(scan_request);
  if (!result.scan_result.diagnostic.ok) {
    auto scan_result = std::move(result.scan_result);
    const auto diagnostic = scan_result.diagnostic;
    const bool replan = scan_result.replan_required;
    auto refused = refuse(diagnostic.diagnostic_code, diagnostic.detail,
                          replan);
    refused.scan_result = std::move(scan_result);
    return refused;
  }
  if (request.cancellation_requested()) {
    return refuse("QOW-DIAG-QRY-004-INDEX-CANCELLED-V1",
                  "index access cancelled before atomic publication");
  }

  result.diagnostic = {};
  result.exact_fallback_recheck_applied =
      (request.selected_index_is_approximate || encoded.lossy ||
       encoded.requires_recheck) &&
      request.exact_fallback_recheck_authorized &&
      result.scan_result.authority.visibility_rechecks_complete;
  return result;
}

// QOW-SOURCE-QRY-004-HEAP-MGA-V1 lives with the bounded MGA relation facade.
// Keeping that adapter in the internal-API translation unit preserves this
// executor's contract-only link profile while the production path remains a
// typed executor entrypoint declared by descriptor_value_runtime.hpp.

}  // namespace scratchbird::engine::executor
