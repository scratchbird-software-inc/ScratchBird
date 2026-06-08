// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "late_materialization_covering_scan_runtime.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

namespace idx = scratchbird::core::index;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;

std::string UuidText(const TypedUuid& value) {
  return value.valid() ? uuid::UuidToString(value.value) : std::string{};
}

std::string BindingKey(std::string row_uuid, std::string version_uuid) {
  return std::move(row_uuid) + "|" + std::move(version_uuid);
}

void AddCommonAcceptedEvidence(const IndexedPhysicalOperatorResult& stream,
                               std::vector<std::string>* evidence) {
  evidence->push_back("irc061.physical_row_id_stream_consumed=true");
  evidence->push_back("irc061.physical_row_id_stream.locator_count=" +
                      std::to_string(stream.locators.size()));
  evidence->push_back("irc061.row_order_preserved=true");
  evidence->push_back("irc061.row_version_uuid_binding_preserved=true");
  evidence->push_back("irc061.mga_visibility_recheck.engine_owned=true");
  evidence->push_back("irc061.security_authorization_recheck.engine_owned=true");
  evidence->push_back("irc061.redaction_recheck.engine_owned=true");
  evidence->push_back("irc061.finality_authority=engine_transaction_inventory");
  evidence->push_back("irc061.index_payload_visibility_authority=false");
  evidence->push_back("irc061.index_payload_authorization_authority=false");
  evidence->push_back("irc061.index_payload_finality_authority=false");
  evidence->push_back("irc061.index_payload_cleanup_authority=false");
  evidence->push_back("irc061.index_payload_recovery_authority=false");
  evidence->push_back("irc061.parser_or_donor_authority=false");
  evidence->push_back("irc061.full_table_scan_or_materialization=false");
  evidence->push_back("irc061.descriptor_or_map_scan_fallback=false");
}

std::string PhysicalStreamFailureDetail(
    const IndexedPhysicalOperatorResult& stream) {
  if (!stream.ok) {
    return stream.diagnostic_detail.empty() ? "indexed_physical_stream_refused"
                                            : stream.diagnostic_detail;
  }
  if (!stream.runtime_route_capability) {
    return "indexed_physical_runtime_route_required";
  }
  if (stream.table_scan_consumed) {
    return "indexed_physical_stream_must_not_consume_table_scan";
  }
  return {};
}

std::string LocatorFailureDetail(const IndexedPhysicalOperatorLocator& locator) {
  if (!locator.from_physical_index) {
    return "physical_index_locator_required";
  }
  if (locator.row_uuid.empty() || locator.version_uuid.empty()) {
    return "physical_row_version_binding_required";
  }
  if (!locator.mga_recheck_required) {
    return "locator_mga_recheck_required";
  }
  if (!locator.security_recheck_required) {
    return "locator_security_recheck_required";
  }
  return {};
}

std::string CommonProofFailureDetail(
    const IndexRuntimeEngineRecheckProof& proof) {
  if (!proof.physical_tree_present) {
    return "physical_index_tree_required";
  }
  if (!proof.plan_safe) {
    return "stale_or_unsafe_plan";
  }
  if (!proof.durable_mga_inventory_proof) {
    return "durable_mga_inventory_proof_required";
  }
  if (!proof.mga_visibility_rechecked_by_engine) {
    return "mga_visibility_recheck_required";
  }
  if (!proof.security_authorized_by_engine) {
    return "security_authorization_recheck_required";
  }
  if (!proof.redaction_checked_by_engine) {
    return "redaction_recheck_required";
  }
  if (!proof.payload_freshness_safe) {
    return "unsafe_payload_freshness";
  }
  if (proof.descriptor_or_map_scan_fallback) {
    return "descriptor_or_map_scan_fallback_forbidden";
  }
  if (proof.full_table_scan_or_materialization) {
    return "full_table_scan_materialization_forbidden";
  }
  if (proof.parser_or_donor_authority) {
    return "parser_or_donor_authority_forbidden";
  }
  if (proof.index_payload_authority) {
    return "index_payload_authority_forbidden";
  }
  return {};
}

std::string DiagnosticForDetail(const std::string& detail) {
  static const std::map<std::string, std::string> diagnostics = {
      {"physical_index_tree_required", "SB-IRC061-PHYSICAL-TREE-REQUIRED"},
      {"stale_or_unsafe_plan", "SB-IRC061-STALE-OR-UNSAFE-PLAN"},
      {"durable_mga_inventory_proof_required",
       "SB-IRC061-MGA-PROOF-REQUIRED"},
      {"mga_visibility_recheck_required",
       "SB-IRC061-MGA-RECHECK-REQUIRED"},
      {"security_authorization_recheck_required",
       "SB-IRC061-SECURITY-RECHECK-REQUIRED"},
      {"redaction_recheck_required",
       "SB-IRC061-REDACTION-RECHECK-REQUIRED"},
      {"unsafe_payload_freshness", "SB-IRC061-UNSAFE-PAYLOAD-FRESHNESS"},
      {"descriptor_or_map_scan_fallback_forbidden",
       "SB-IRC061-DESCRIPTOR-MAP-SCAN-FALLBACK-FORBIDDEN"},
      {"full_table_scan_materialization_forbidden",
       "SB-IRC061-FULL-SCAN-FALLBACK-FORBIDDEN"},
      {"parser_or_donor_authority_forbidden",
       "SB-IRC061-PARSER-DONOR-AUTHORITY-FORBIDDEN"},
      {"index_payload_authority_forbidden",
       "SB-IRC061-INDEX-PAYLOAD-AUTHORITY-FORBIDDEN"},
      {"indexed_physical_stream_refused",
       "SB-IRC061-INDEXED-PHYSICAL-STREAM-REFUSED"},
      {"indexed_physical_runtime_route_required",
       "SB-IRC061-INDEXED-PHYSICAL-STREAM-REQUIRED"},
      {"indexed_physical_stream_must_not_consume_table_scan",
       "SB-IRC061-INDEXED-PHYSICAL-STREAM-TABLE-SCAN-FORBIDDEN"},
      {"physical_index_locator_required",
       "SB-IRC061-PHYSICAL-LOCATOR-REQUIRED"},
      {"physical_row_version_binding_required",
       "SB-IRC061-ROW-VERSION-BINDING-REQUIRED"},
      {"locator_mga_recheck_required",
       "SB-IRC061-LOCATOR-MGA-RECHECK-REQUIRED"},
      {"locator_security_recheck_required",
       "SB-IRC061-LOCATOR-SECURITY-RECHECK-REQUIRED"},
      {"base_row_fetcher_required", "SB-IRC061-BASE-ROW-FETCHER-REQUIRED"},
      {"base_row_fetch_refused", "SB-IRC061-BASE-ROW-FETCH-REFUSED"},
      {"base_row_locator_binding_mismatch",
       "SB-IRC061-BASE-ROW-BINDING-MISMATCH"},
      {"base_row_provider_scan_fallback_forbidden",
       "SB-IRC061-BASE-ROW-SCAN-FALLBACK-FORBIDDEN"},
      {"covering_payload_missing", "SB-IRC061-COVERING-PAYLOAD-MISSING"},
      {"covering_payload_not_admitted",
       "SB-IRC061-COVERING-PAYLOAD-NOT-ADMITTED"},
      {"covering_projection_only_required",
       "SB-IRC061-COVERING-PROJECTION-ONLY-REQUIRED"},
      {"covering_payload_result_frame_required",
       "SB-IRC061-COVERING-RESULT-FRAME-REQUIRED"},
      {"covering_payload_recheck_proof_missing",
       "SB-IRC061-COVERING-RECHECK-PROOF-REQUIRED"},
      {"covering_payload_authority_forbidden",
       "SB-IRC061-COVERING-PAYLOAD-AUTHORITY-FORBIDDEN"},
      {"covering_payload_duplicate",
       "SB-IRC061-COVERING-PAYLOAD-DUPLICATE"},
      {"covering_payload_unconsumed",
       "SB-IRC061-COVERING-PAYLOAD-UNCONSUMED"},
  };
  const auto found = diagnostics.find(detail);
  return found == diagnostics.end() ? "SB-IRC061-RUNTIME-REFUSED"
                                    : found->second;
}

LateMaterializationIndexedRuntimeResult FailLate(std::string detail) {
  LateMaterializationIndexedRuntimeResult result;
  result.ok = false;
  result.diagnostic_detail = std::move(detail);
  result.diagnostic_code = DiagnosticForDetail(result.diagnostic_detail);
  result.runtime_route_capability = false;
  result.benchmark_clean = false;
  result.full_table_scan_or_materialization = false;
  result.evidence.push_back("irc061.late_materialization.fail_closed=true");
  result.evidence.push_back("irc061.refused=" + result.diagnostic_code);
  result.evidence.push_back("irc061.runtime_route_capability=false");
  result.evidence.push_back("irc061.benchmark_clean=false");
  result.evidence.push_back("irc061.finality_authority=engine_transaction_inventory");
  result.evidence.push_back("irc061.index_payload_finality_authority=false");
  return result;
}

CoveringProjectionOnlyScanResult FailCovering(std::string detail) {
  CoveringProjectionOnlyScanResult result;
  result.ok = false;
  result.diagnostic_detail = std::move(detail);
  result.diagnostic_code = DiagnosticForDetail(result.diagnostic_detail);
  result.runtime_route_capability = false;
  result.benchmark_clean = false;
  result.projection_only = false;
  result.full_table_scan_or_materialization = false;
  result.evidence.push_back("irc061.covering_projection.fail_closed=true");
  result.evidence.push_back("irc061.refused=" + result.diagnostic_code);
  result.evidence.push_back("irc061.runtime_route_capability=false");
  result.evidence.push_back("irc061.benchmark_clean=false");
  result.evidence.push_back("irc061.finality_authority=engine_transaction_inventory");
  result.evidence.push_back("irc061.index_payload_finality_authority=false");
  return result;
}

std::string ValidateCommonStream(const IndexedPhysicalOperatorResult* stream,
                                 const IndexRuntimeEngineRecheckProof& proof) {
  if (stream == nullptr) {
    return "indexed_physical_stream_refused";
  }
  if (const auto proof_detail = CommonProofFailureDetail(proof);
      !proof_detail.empty()) {
    return proof_detail;
  }
  if (const auto stream_detail = PhysicalStreamFailureDetail(*stream);
      !stream_detail.empty()) {
    return stream_detail;
  }
  for (const auto& locator : stream->locators) {
    if (const auto locator_detail = LocatorFailureDetail(locator);
        !locator_detail.empty()) {
      return locator_detail;
    }
  }
  return {};
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 const std::string& expected) {
  return std::find(evidence.begin(), evidence.end(), expected) !=
         evidence.end();
}

bool CoveringAdmissionAuthoritySafe(
    const idx::CoveringIndexPayloadAdmission& admission) {
  return !admission.visibility_authority &&
         !admission.authorization_authority &&
         !admission.transaction_finality_authority &&
         !admission.cleanup_authority &&
         !admission.recovery_authority;
}

std::string CoveringAdmissionFailureDetail(
    const idx::CoveringIndexPayloadAdmission* admission) {
  if (admission == nullptr) {
    return "covering_payload_missing";
  }
  if (!admission->ok()) {
    return "covering_payload_not_admitted";
  }
  if (!admission->index_only_admitted ||
      admission->base_row_recheck_required ||
      admission->base_row_recheck_handoff_proven ||
      !admission->payload_projection_only) {
    return "covering_projection_only_required";
  }
  if (!admission->result_frame_compatible) {
    return "covering_payload_result_frame_required";
  }
  if (!CoveringAdmissionAuthoritySafe(*admission)) {
    return "covering_payload_authority_forbidden";
  }
  if (!HasEvidence(admission->evidence,
                   "covering_payload.required_rechecks_proven=true") ||
      !HasEvidence(admission->evidence,
                   "covering_payload.redaction_policy_epoch_current=true") ||
      !HasEvidence(admission->evidence,
                   "covering_payload.security_policy_epoch_current=true") ||
      !HasEvidence(admission->evidence,
                   "covering_payload.freshness_generation_current=true")) {
    return "covering_payload_recheck_proof_missing";
  }
  return {};
}

bool EngineEvidenceContains(
    const std::vector<scratchbird::engine::internal_api::EngineEvidenceReference>&
        evidence,
    std::string_view kind,
    std::string_view id) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const auto& item) {
                       return item.evidence_kind == kind &&
                              item.evidence_id.find(id) != std::string::npos;
                     });
}

std::string_view RequiredPhysicalOperatorName(
    IndexPlanShapeRequiredPath path) {
  switch (path) {
    case IndexPlanShapeRequiredPath::indexed_point_lookup:
      return "point_lookup";
    case IndexPlanShapeRequiredPath::indexed_range_scan:
      return "range_scan";
    case IndexPlanShapeRequiredPath::indexed_ordered_limit:
      return "ordered_limit";
    case IndexPlanShapeRequiredPath::late_materialization_row_id_stream:
    case IndexPlanShapeRequiredPath::covering_projection_only:
      return {};
  }
  return {};
}

enum class IndexPlanShapeExactBlocker {
  none,
  missing_physical_tree,
  stale_plan,
  missing_mga_security_redaction_proof,
  missing_covering_payload,
  missing_encoded_key_or_bounds,
  unsupported_physical_family
};

bool IsOneOf(std::string_view value,
             std::initializer_list<std::string_view> options) {
  return std::find(options.begin(), options.end(), value) != options.end();
}

IndexPlanShapeExactBlocker ExactBlockerFromDetail(std::string_view detail) {
  if (IsOneOf(detail,
              {"physical_index_tree_required",
               "physical_index_tree_available_required"})) {
    return IndexPlanShapeExactBlocker::missing_physical_tree;
  }
  if (detail == "stale_or_unsafe_plan") {
    return IndexPlanShapeExactBlocker::stale_plan;
  }
  if (IsOneOf(detail,
              {"durable_mga_inventory_proof_required",
               "mga_visibility_recheck_required",
               "security_recheck_required",
               "security_authorization_recheck_required",
               "redaction_recheck_required",
               "locator_mga_recheck_required",
               "locator_security_recheck_required",
               "covering_payload_recheck_proof_missing"})) {
    return IndexPlanShapeExactBlocker::missing_mga_security_redaction_proof;
  }
  if (IsOneOf(detail,
              {"covering_payload_missing",
               "covering_payload_not_admitted",
               "covering_projection_only_required",
               "covering_payload_result_frame_required",
               "covering_payload_authority_forbidden",
               "covering_payload_duplicate",
               "covering_payload_unconsumed",
               "unsafe_payload_freshness"})) {
    return IndexPlanShapeExactBlocker::missing_covering_payload;
  }
  if (IsOneOf(detail,
              {"encoded_point_key_required",
               "encoded_range_bounds_required",
               "nested_loop_encoded_point_key_required",
               "nested_loop_encoded_range_bounds_required",
               "runtime_filter_encoded_keys_required",
               "runtime_filter_encoded_key_proof_required",
               "ordered_limit_bound_required"})) {
    return IndexPlanShapeExactBlocker::missing_encoded_key_or_bounds;
  }
  if (detail == "unsafe_physical_index_tree") {
    return IndexPlanShapeExactBlocker::unsupported_physical_family;
  }
  return IndexPlanShapeExactBlocker::none;
}

IndexPlanShapeExactBlocker ExplicitExactBlocker(
    const IndexPlanShapeRegressionGuardRequest& request) {
  if (request.missing_physical_tree_blocker) {
    return IndexPlanShapeExactBlocker::missing_physical_tree;
  }
  if (request.stale_plan_blocker) {
    return IndexPlanShapeExactBlocker::stale_plan;
  }
  if (request.missing_mga_security_redaction_proof_blocker) {
    return IndexPlanShapeExactBlocker::missing_mga_security_redaction_proof;
  }
  if (request.missing_covering_payload_blocker) {
    return IndexPlanShapeExactBlocker::missing_covering_payload;
  }
  if (request.missing_encoded_key_or_bounds_blocker) {
    return IndexPlanShapeExactBlocker::missing_encoded_key_or_bounds;
  }
  if (request.unsupported_physical_family_blocker) {
    return IndexPlanShapeExactBlocker::unsupported_physical_family;
  }
  return IndexPlanShapeExactBlocker::none;
}

IndexPlanShapeExactBlocker RuntimeResultExactBlocker(
    const IndexPlanShapeRegressionGuardRequest& request) {
  if (request.physical_result != nullptr && !request.physical_result->ok) {
    return ExactBlockerFromDetail(request.physical_result->diagnostic_detail);
  }
  if (request.late_materialization_result != nullptr &&
      !request.late_materialization_result->ok) {
    return ExactBlockerFromDetail(
        request.late_materialization_result->diagnostic_detail);
  }
  if (request.covering_projection_result != nullptr &&
      !request.covering_projection_result->ok) {
    return ExactBlockerFromDetail(
        request.covering_projection_result->diagnostic_detail);
  }
  return IndexPlanShapeExactBlocker::none;
}

const char* ExactBlockerName(IndexPlanShapeExactBlocker blocker) {
  switch (blocker) {
    case IndexPlanShapeExactBlocker::missing_physical_tree:
      return "missing_physical_tree";
    case IndexPlanShapeExactBlocker::stale_plan:
      return "stale_plan";
    case IndexPlanShapeExactBlocker::missing_mga_security_redaction_proof:
      return "missing_mga_security_redaction_proof";
    case IndexPlanShapeExactBlocker::missing_covering_payload:
      return "missing_covering_payload";
    case IndexPlanShapeExactBlocker::missing_encoded_key_or_bounds:
      return "missing_encoded_key_or_bounds";
    case IndexPlanShapeExactBlocker::unsupported_physical_family:
      return "unsupported_physical_family";
    case IndexPlanShapeExactBlocker::none:
      return "none";
  }
  return "none";
}

const char* ExactBlockerDiagnostic(IndexPlanShapeExactBlocker blocker) {
  switch (blocker) {
    case IndexPlanShapeExactBlocker::missing_physical_tree:
      return "SB-IRC062-BLOCKED-MISSING-PHYSICAL-TREE";
    case IndexPlanShapeExactBlocker::stale_plan:
      return "SB-IRC062-BLOCKED-STALE-PLAN";
    case IndexPlanShapeExactBlocker::missing_mga_security_redaction_proof:
      return "SB-IRC062-BLOCKED-MISSING-MGA-SECURITY-REDACTION-PROOF";
    case IndexPlanShapeExactBlocker::missing_covering_payload:
      return "SB-IRC062-BLOCKED-MISSING-COVERING-PAYLOAD";
    case IndexPlanShapeExactBlocker::missing_encoded_key_or_bounds:
      return "SB-IRC062-BLOCKED-MISSING-ENCODED-KEY-OR-BOUNDS";
    case IndexPlanShapeExactBlocker::unsupported_physical_family:
      return "SB-IRC062-BLOCKED-UNSUPPORTED-PHYSICAL-FAMILY";
    case IndexPlanShapeExactBlocker::none:
      return "SB-IRC062-BLOCKED-UNKNOWN";
  }
  return "SB-IRC062-BLOCKED-UNKNOWN";
}

IndexPlanShapeRegressionGuardResult GuardFailure(
    const IndexPlanShapeRegressionGuardRequest& request,
    std::string code,
    std::string detail) {
  IndexPlanShapeRegressionGuardResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.diagnostic_detail = std::move(detail);
  result.benchmark_clean = false;
  result.scan_only_regression =
      request.table_scan_fallback || request.descriptor_scan_fallback ||
      request.map_scan_fallback;
  result.per_row_wrapper_regression = request.per_row_wrapper_execution;
  result.text_result_regression = request.text_result_materialization;
  result.statistics_only_regression =
      request.statistics_only_optimizer_route ||
      request.local_candidate_planning;
  result.evidence.push_back("irc062.route=" + request.route_name);
  result.evidence.push_back("irc062.required_path=" +
                            std::string(IndexPlanShapeRequiredPathName(
                                request.required_path)));
  result.evidence.push_back("irc062.fail_closed=true");
  result.evidence.push_back("irc062.diagnostic=" + result.diagnostic_code);
  result.evidence.push_back("irc062.benchmark_clean=false");
  result.evidence.push_back("irc062.finality_authority=engine_transaction_inventory");
  result.evidence.push_back("irc062.parser_or_donor_authority=false");
  result.evidence.push_back("irc062.index_cache_finality_authority=false");
  result.evidence.push_back(
      "irc062.stale_route_capability=" +
      std::string(request.stale_route_capability ? "true" : "false"));
  if (request.expected_route_capability_generation != 0 ||
      request.observed_route_capability_generation != 0) {
    result.evidence.push_back(
        "irc062.expected_route_capability_generation=" +
        std::to_string(request.expected_route_capability_generation));
    result.evidence.push_back(
        "irc062.observed_route_capability_generation=" +
        std::to_string(request.observed_route_capability_generation));
  }
  result.evidence.push_back(
      "irc062.invalid_route_family_use=" +
      std::string(request.invalid_route_family_use ? "true" : "false"));
  if (!request.invalid_route_family_detail.empty()) {
    result.evidence.push_back("irc062.invalid_route_family_detail=" +
                              request.invalid_route_family_detail);
  }
  return result;
}

IndexPlanShapeRegressionGuardResult GuardBlocker(
    const IndexPlanShapeRegressionGuardRequest& request,
    IndexPlanShapeExactBlocker blocker) {
  auto result = GuardFailure(request,
                             ExactBlockerDiagnostic(blocker),
                             ExactBlockerName(blocker));
  result.exact_blocker = true;
  result.evidence.push_back("irc062.exact_blocker=true");
  result.evidence.push_back("irc062.exact_blocker_detail=" +
                            result.diagnostic_detail);
  if (blocker == IndexPlanShapeExactBlocker::unsupported_physical_family &&
      !request.unsupported_physical_family.empty()) {
    result.evidence.push_back("irc062.unsupported_physical_family=" +
                              request.unsupported_physical_family);
  }
  return result;
}

bool PhysicalRouteConsumed(const IndexedPhysicalOperatorResult* result) {
  return result != nullptr && result->ok && result->runtime_route_capability &&
         !result->table_scan_consumed;
}

bool LateMaterializationRouteConsumed(
    const LateMaterializationIndexedRuntimeResult* result) {
  return result != nullptr && result->ok && result->runtime_route_capability &&
         !result->full_table_scan_or_materialization;
}

bool CoveringProjectionRouteConsumed(
    const CoveringProjectionOnlyScanResult* result) {
  return result != nullptr && result->ok && result->runtime_route_capability &&
         result->projection_only && !result->full_table_scan_or_materialization;
}

bool RequiredRouteConsumed(const IndexPlanShapeRegressionGuardRequest& request) {
  if (!PhysicalRouteConsumed(request.physical_result)) {
    return false;
  }
  if (request.required_path ==
      IndexPlanShapeRequiredPath::late_materialization_row_id_stream) {
    return LateMaterializationRouteConsumed(request.late_materialization_result);
  }
  if (request.required_path ==
      IndexPlanShapeRequiredPath::covering_projection_only) {
    return CoveringProjectionRouteConsumed(request.covering_projection_result);
  }
  return true;
}

}  // namespace

LateMaterializationIndexedRuntimeResult
ConsumeIndexedRowIdStreamForLateMaterialization(
    const IndexedPhysicalOperatorResult& physical_stream,
    const IndexRuntimeEngineRecheckProof& proof,
    const LateMaterializationIndexedRowProvider& row_provider) {
  if (const auto detail = ValidateCommonStream(&physical_stream, proof);
      !detail.empty()) {
    return FailLate(detail);
  }
  if (!row_provider) {
    return FailLate("base_row_fetcher_required");
  }

  LateMaterializationIndexedRuntimeResult result;
  result.ok = true;
  result.diagnostic_code = "SB-IRC061-OK";
  result.diagnostic_detail = "late_materialization_row_id_stream_consumed";
  result.runtime_route_capability = true;
  result.benchmark_clean = false;
  result.full_table_scan_or_materialization = false;
  AddCommonAcceptedEvidence(physical_stream, &result.evidence);
  result.evidence.push_back("irc061.late_materialization.base_row_recheck_handoff=true");
  result.evidence.push_back("irc061.late_materialization.row_id_stream_only=true");

  for (std::size_t ordinal = 0; ordinal < physical_stream.locators.size();
       ++ordinal) {
    const auto& locator = physical_stream.locators[ordinal];
    auto provided = row_provider(locator);
    result.evidence.insert(result.evidence.end(),
                           provided.evidence.begin(),
                           provided.evidence.end());
    if (!provided.ok) {
      auto failed = FailLate("base_row_fetch_refused");
      failed.diagnostic_code = provided.diagnostic_code.empty()
                                   ? failed.diagnostic_code
                                   : provided.diagnostic_code;
      failed.diagnostic_detail = provided.diagnostic_detail.empty()
                                     ? failed.diagnostic_detail
                                     : provided.diagnostic_detail;
      return failed;
    }
    if (provided.row.row_uuid != locator.row_uuid ||
        provided.row.version_uuid != locator.version_uuid) {
      return FailLate("base_row_locator_binding_mismatch");
    }
    if (provided.row.provider_full_table_scan_used ||
        provided.row.provider_descriptor_or_map_scan_used) {
      return FailLate("base_row_provider_scan_fallback_forbidden");
    }
    provided.row.stream_ordinal = static_cast<std::uint64_t>(ordinal);
    result.evidence.insert(result.evidence.end(),
                           provided.row.evidence.begin(),
                           provided.row.evidence.end());
    result.evidence.push_back("irc061.late_materialization.stream_row=" +
                              std::to_string(ordinal) + ":" +
                              locator.row_uuid + ":" +
                              locator.version_uuid);
    result.rows.push_back(std::move(provided.row));
  }

  result.evidence.push_back("irc061.runtime_route_capability=true");
  result.evidence.push_back("irc061.benchmark_clean=false");
  return result;
}

CoveringProjectionOnlyScanResult ExecuteCoveringProjectionOnlyScan(
    const CoveringProjectionOnlyScanRequest& request) {
  if (const auto detail = ValidateCommonStream(request.physical_stream,
                                              request.proof);
      !detail.empty()) {
    return FailCovering(detail);
  }

  std::map<std::string, const idx::CoveringIndexPayloadAdmission*> admissions;
  for (const auto* admission : request.admissions) {
    if (admission == nullptr) {
      return FailCovering("covering_payload_missing");
    }
    const auto key =
        BindingKey(UuidText(admission->record.row_uuid),
                   UuidText(admission->record.version_uuid));
    const auto inserted = admissions.emplace(key, admission);
    if (!inserted.second) {
      return FailCovering("covering_payload_duplicate");
    }
  }

  CoveringProjectionOnlyScanResult result;
  result.ok = true;
  result.diagnostic_code = "SB-IRC061-OK";
  result.diagnostic_detail = "covering_projection_only_scan_consumed";
  result.runtime_route_capability = true;
  result.benchmark_clean = false;
  result.projection_only = true;
  result.full_table_scan_or_materialization = false;
  AddCommonAcceptedEvidence(*request.physical_stream, &result.evidence);
  result.evidence.push_back("irc061.covering_projection_only_scan=true");
  result.evidence.push_back("irc061.covering_payload_path=IRC-032");
  result.evidence.push_back("irc061.covering_full_table_scan_used=false");
  result.evidence.push_back("irc061.covering_base_row_materialization_used=false");

  for (std::size_t ordinal = 0; ordinal < request.physical_stream->locators.size();
       ++ordinal) {
    const auto& locator = request.physical_stream->locators[ordinal];
    const auto found =
        admissions.find(BindingKey(locator.row_uuid, locator.version_uuid));
    const auto* admission =
        found == admissions.end() ? nullptr : found->second;
    if (const auto detail = CoveringAdmissionFailureDetail(admission);
        !detail.empty()) {
      return FailCovering(detail);
    }

    CoveringProjectionRow row;
    row.row_uuid = locator.row_uuid;
    row.version_uuid = locator.version_uuid;
    row.stream_ordinal = static_cast<std::uint64_t>(ordinal);
    for (const auto& value : admission->record.values) {
      CoveringProjectionCell cell;
      cell.projection_ordinal = value.projection_ordinal;
      cell.kind = value.kind;
      cell.encoded_value = value.encoded_value;
      cell.redacted = value.redacted;
      row.cells.push_back(std::move(cell));
    }
    result.evidence.insert(result.evidence.end(),
                           admission->evidence.begin(),
                           admission->evidence.end());
    result.evidence.push_back("irc061.covering.stream_row=" +
                              std::to_string(ordinal) + ":" +
                              locator.row_uuid + ":" +
                              locator.version_uuid);
    result.rows.push_back(std::move(row));
    admissions.erase(found);
  }

  if (!admissions.empty()) {
    return FailCovering("covering_payload_unconsumed");
  }

  result.evidence.push_back("irc061.runtime_route_capability=true");
  result.evidence.push_back("irc061.benchmark_clean=false");
  return result;
}

const char* IndexPlanShapeRequiredPathName(IndexPlanShapeRequiredPath path) {
  switch (path) {
    case IndexPlanShapeRequiredPath::indexed_point_lookup:
      return "indexed_point_lookup";
    case IndexPlanShapeRequiredPath::indexed_range_scan:
      return "indexed_range_scan";
    case IndexPlanShapeRequiredPath::indexed_ordered_limit:
      return "indexed_ordered_limit";
    case IndexPlanShapeRequiredPath::late_materialization_row_id_stream:
      return "late_materialization_row_id_stream";
    case IndexPlanShapeRequiredPath::covering_projection_only:
      return "covering_projection_only";
  }
  return "unknown";
}

IndexPlanShapeRegressionGuardResult EvaluateIndexPlanShapeRegressionGuard(
    const IndexPlanShapeRegressionGuardRequest& request) {
  const auto runtime_blocker = RuntimeResultExactBlocker(request);
  if (runtime_blocker != IndexPlanShapeExactBlocker::none) {
    return GuardBlocker(request, runtime_blocker);
  }
  const auto explicit_blocker = ExplicitExactBlocker(request);
  if (explicit_blocker != IndexPlanShapeExactBlocker::none) {
    if (RequiredRouteConsumed(request)) {
      return GuardFailure(
          request,
          "SB-IRC062-CONFLICTING-BLOCKER-AND-PHYSICAL-ROUTE",
          "exact_blocker_conflicts_with_consumed_physical_route");
    }
    return GuardBlocker(request, explicit_blocker);
  }

  if (request.benchmark_clean_claim) {
    return GuardFailure(request,
                        "SB-IRC062-BENCHMARK-CLEAN-CLAIM-FORBIDDEN",
                        "benchmark_clean_family_gate_not_proven");
  }
  if (request.donor_dominance_claim) {
    return GuardFailure(request,
                        "SB-IRC062-DONOR-DOMINANCE-CLAIM-FORBIDDEN",
                        "donor_dominance_claim_not_a_plan_shape_authority");
  }
  if (request.stale_route_capability ||
      (request.expected_route_capability_generation != 0 &&
       request.observed_route_capability_generation != 0 &&
       request.expected_route_capability_generation !=
           request.observed_route_capability_generation)) {
    return GuardFailure(request,
                        "SB-IRC062-STALE-ROUTE-CAPABILITY",
                        "stale_route_capability_without_exact_blocker");
  }
  if (request.invalid_route_family_use) {
    return GuardFailure(request,
                        "SB-IRC062-INVALID-ROUTE-FAMILY-USE",
                        request.invalid_route_family_detail.empty()
                            ? "invalid_route_family_without_exact_blocker"
                            : request.invalid_route_family_detail);
  }
  if (request.table_scan_fallback ||
      (request.physical_result != nullptr &&
       request.physical_result->table_scan_consumed)) {
    return GuardFailure(request,
                        "SB-IRC062-TABLE-SCAN-FALLBACK-REGRESSION",
                        "table_scan_fallback_without_exact_blocker");
  }
  if (request.descriptor_scan_fallback || request.map_scan_fallback) {
    return GuardFailure(request,
                        "SB-IRC062-DESCRIPTOR-MAP-SCAN-FALLBACK-REGRESSION",
                        "descriptor_or_map_scan_fallback_without_exact_blocker");
  }
  if (request.per_row_wrapper_execution) {
    return GuardFailure(request,
                        "SB-IRC062-PER-ROW-WRAPPER-REGRESSION",
                        "per_row_wrapper_execution_without_exact_blocker");
  }
  if (request.text_result_materialization) {
    return GuardFailure(request,
                        "SB-IRC062-TEXT-RESULT-REGRESSION",
                        "text_result_materialization_without_exact_blocker");
  }
  if (request.statistics_only_optimizer_route ||
      request.local_candidate_planning) {
    return GuardFailure(request,
                        "SB-IRC062-STATISTICS-ONLY-LOCAL-CANDIDATE-REGRESSION",
                        "statistics_only_or_local_candidate_planning_without_exact_blocker");
  }
  if (request.contract_only_evidence) {
    return GuardFailure(request,
                        "SB-IRC062-CONTRACT-ONLY-NO-RUNTIME-EVIDENCE",
                        "contract_only_evidence_without_runtime_consumption");
  }

  if (!PhysicalRouteConsumed(request.physical_result)) {
    return GuardFailure(request,
                        "SB-IRC062-PHYSICAL-ROUTE-MISSING-EXACT-BLOCKER",
                        "physical_route_required_without_exact_blocker");
  }

  const auto required_operator = RequiredPhysicalOperatorName(request.required_path);
  if (!required_operator.empty() &&
      !EngineEvidenceContains(request.physical_result->evidence,
                              "indexed_physical_operator",
                              required_operator)) {
    return GuardFailure(request,
                        "SB-IRC062-PHYSICAL-ROUTE-SHAPE-MISMATCH",
                        "required_indexed_operator_shape_not_consumed");
  }

  if (request.required_path ==
          IndexPlanShapeRequiredPath::late_materialization_row_id_stream &&
      !LateMaterializationRouteConsumed(request.late_materialization_result)) {
    return GuardFailure(request,
                        "SB-IRC062-LATE-MATERIALIZATION-ROUTE-MISSING-EXACT-BLOCKER",
                        "late_materialization_route_required_without_exact_blocker");
  }
  if (request.required_path ==
          IndexPlanShapeRequiredPath::covering_projection_only &&
      !CoveringProjectionRouteConsumed(request.covering_projection_result)) {
    return GuardFailure(request,
                        "SB-IRC062-COVERING-ROUTE-MISSING-EXACT-BLOCKER",
                        "covering_projection_route_required_without_exact_blocker");
  }

  IndexPlanShapeRegressionGuardResult result;
  result.ok = true;
  result.diagnostic_code = "SB-IRC062-OK";
  result.diagnostic_detail = "physical_index_route_shape_consumed";
  result.physical_route_consumed = true;
  result.benchmark_clean = false;
  result.evidence.push_back("irc062.route=" + request.route_name);
  result.evidence.push_back("irc062.required_path=" +
                            std::string(IndexPlanShapeRequiredPathName(
                                request.required_path)));
  result.evidence.push_back("irc062.physical_route_consumed=true");
  result.evidence.push_back("irc062.table_scan_fallback=false");
  result.evidence.push_back("irc062.descriptor_scan_fallback=false");
  result.evidence.push_back("irc062.map_scan_fallback=false");
  result.evidence.push_back("irc062.per_row_wrapper_execution=false");
  result.evidence.push_back("irc062.text_result_materialization=false");
  result.evidence.push_back("irc062.statistics_only_optimizer_route=false");
  result.evidence.push_back("irc062.local_candidate_planning=false");
  result.evidence.push_back("irc062.contract_only_evidence=false");
  result.evidence.push_back("irc062.donor_dominance_claim=false");
  result.evidence.push_back("irc062.stale_route_capability=false");
  result.evidence.push_back("irc062.invalid_route_family_use=false");
  result.evidence.push_back("irc062.benchmark_clean=false");
  result.evidence.push_back("irc062.finality_authority=engine_transaction_inventory");
  result.evidence.push_back("irc062.parser_or_donor_authority=false");
  result.evidence.push_back("irc062.index_cache_finality_authority=false");
  return result;
}

}  // namespace scratchbird::engine::executor
