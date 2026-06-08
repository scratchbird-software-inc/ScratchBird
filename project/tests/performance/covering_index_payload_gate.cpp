// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"
#include "covering_index_payload.hpp"
#include "index_family_registry.hpp"
#include "index_ordered_access.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace plan = scratchbird::engine::planner;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "covering_index_payload_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Has(const std::vector<std::string>& values, std::string_view value) {
  return std::any_of(values.begin(), values.end(), [&](const std::string& item) {
    return item == value;
  });
}

bool HasContaining(const std::vector<std::string>& values,
                   std::string_view value) {
  return std::any_of(values.begin(), values.end(), [&](const std::string& item) {
    return item.find(value) != std::string::npos;
  });
}

platform::TypedUuid Typed(platform::UuidKind kind, platform::byte seed) {
  platform::TypedUuid uuid;
  uuid.kind = kind;
  for (std::size_t i = 0; i < uuid.value.bytes.size(); ++i) {
    uuid.value.bytes[i] = static_cast<platform::byte>(
        seed + static_cast<platform::byte>(i + 1));
  }
  uuid.value.bytes[6] =
      static_cast<platform::byte>((uuid.value.bytes[6] & 0x0fu) | 0x70u);
  uuid.value.bytes[8] =
      static_cast<platform::byte>((uuid.value.bytes[8] & 0x3fu) | 0x80u);
  return uuid;
}

platform::TypedUuid ObjectUuid(platform::byte seed) {
  return Typed(platform::UuidKind::object, seed);
}

platform::TypedUuid RowUuid(platform::byte seed) {
  return Typed(platform::UuidKind::row, seed);
}

std::vector<platform::byte> Bytes(std::string_view text) {
  return std::vector<platform::byte>(text.begin(), text.end());
}

idx::CoveringIndexLargePayloadReference LargeRef(platform::byte seed) {
  idx::CoveringIndexLargePayloadReference ref;
  ref.payload_uuid = ObjectUuid(seed);
  ref.owner_object_uuid = ObjectUuid(static_cast<platform::byte>(seed + 1));
  ref.generation_scope_uuid =
      ObjectUuid(static_cast<platform::byte>(seed + 2));
  ref.generation = 7;
  ref.byte_count = 8192;
  ref.descriptor_hash = "large-descriptor:" + std::to_string(seed);
  return ref;
}

idx::CoveringIndexPayloadColumnRef Column(platform::byte seed,
                                          platform::byte type_seed,
                                          platform::u32 ordinal) {
  idx::CoveringIndexPayloadColumnRef column;
  column.column_uuid = ObjectUuid(seed);
  column.type_descriptor_uuid = ObjectUuid(type_seed);
  column.projection_ordinal = ordinal;
  column.required = true;
  return column;
}

idx::CoveringIndexPayloadColumnValue InlineValue(
    const idx::CoveringIndexPayloadColumnRef& column,
    std::string_view value) {
  idx::CoveringIndexPayloadColumnValue out;
  out.column_uuid = column.column_uuid;
  out.projection_ordinal = column.projection_ordinal;
  out.kind = idx::CoveringIndexPayloadValueKind::inline_value;
  out.encoded_value = Bytes(value);
  out.binary_result_frame_compatible = true;
  out.redaction_safe = true;
  out.unredacted_authorized = true;
  return out;
}

idx::CoveringIndexPayloadColumnValue LargeValue(
    const idx::CoveringIndexPayloadColumnRef& column,
    const idx::CoveringIndexLargePayloadReference& ref) {
  idx::CoveringIndexPayloadColumnValue out;
  out.column_uuid = column.column_uuid;
  out.projection_ordinal = column.projection_ordinal;
  out.kind = idx::CoveringIndexPayloadValueKind::large_payload_reference;
  out.large_payload = ref;
  out.binary_result_frame_compatible = true;
  out.redaction_safe = true;
  out.protected_value = true;
  out.redacted = true;
  return out;
}

idx::CoveringIndexPayloadAssemblyRequest BaseAssemblyRequest() {
  const auto c1 = Column(1, 41, 0);
  const auto c2 = Column(2, 42, 1);
  idx::CoveringIndexPayloadAssemblyRequest request;
  request.index_uuid = ObjectUuid(80);
  request.table_uuid = ObjectUuid(81);
  request.row_uuid = RowUuid(11);
  request.version_uuid = RowUuid(12);
  request.descriptor_result_contract_hash = "contract:customer:v1";
  request.payload_generation = 100;
  request.redaction_policy_epoch = 200;
  request.security_policy_epoch = 300;
  request.freshness_generation = 400;
  request.descriptor_columns = {c1, c2};
  request.projected_column_uuids = {c1.column_uuid, c2.column_uuid};
  request.values = {InlineValue(c1, "customer-1"), LargeValue(c2, LargeRef(90))};
  request.projection_only = true;
  request.result_contract_bound = true;
  return request;
}

idx::CoveringIndexPayloadAssemblyResult AssembleValidPayload() {
  auto assembled =
      idx::AssembleCoveringIndexPayload(BaseAssemblyRequest());
  Require(assembled.ok(), "valid covering payload assembly failed");
  Require(!assembled.record.physical_payload.empty(),
          "physical payload layout was not emitted");
  Require(HasContaining(assembled.evidence,
                        "covering_payload.row_shape_hash="),
          "row-shape hash evidence missing");
  Require(Has(assembled.evidence,
              "covering_payload.transaction_finality_authority=false"),
          "payload non-finality evidence missing");
  return assembled;
}

idx::CoveringIndexPayloadValidationRequest BaseValidationRequest(
    const idx::CoveringIndexPayloadRecord& record) {
  const auto c1 = Column(1, 41, 0);
  const auto c2 = Column(2, 42, 1);
  idx::CoveringIndexPayloadValidationRequest request;
  request.record = record;
  request.locator.encoded_key = Bytes("SBKO-key");
  request.locator.row_uuid = record.row_uuid;
  request.locator.version_uuid = record.version_uuid;
  request.locator.leaf_page_number = 17;
  request.locator.cell_ordinal = 3;
  request.locator.physical_btree_locator_scan = true;
  request.required_columns = {c1, c2};
  request.projected_column_uuids = {c1.column_uuid, c2.column_uuid};
  request.expected_large_payloads = {{c2.column_uuid, LargeRef(90)}};
  request.expected_descriptor_result_contract_hash =
      "contract:customer:v1";
  request.expected_payload_generation = 100;
  request.expected_redaction_policy_epoch = 200;
  request.expected_security_policy_epoch = 300;
  request.expected_freshness_generation = 400;
  request.descriptor_epoch_current = true;
  request.result_contract_current = true;
  request.redaction_epoch_current = true;
  request.security_epoch_current = true;
  request.freshness_current = true;
  request.result_frame_contract_proven = true;
  request.redaction_policy_safe = true;
  request.exact_predicate_recheck_planned = true;
  request.mga_visibility_recheck_planned = true;
  request.security_authorization_recheck_planned = true;
  request.exact_predicate_rechecked_by_engine = true;
  request.mga_visibility_rechecked_by_engine = true;
  request.security_authorized_by_engine = true;
  request.base_row_recheck_available = true;
  request.allow_index_only = true;
  return request;
}

template <typename Mutate>
void ExpectValidationBlocker(const idx::CoveringIndexPayloadRecord& record,
                             std::string_view blocker,
                             std::string_view label,
                             Mutate mutate) {
  auto request = BaseValidationRequest(record);
  mutate(request);
  const auto admission = idx::ValidateCoveringIndexPayloadForLocator(request);
  Require(!admission.ok() && admission.fail_closed, label);
  Require(Has(admission.blockers, blocker), label);
  Require(Has(admission.evidence, "covering_payload.fail_closed=true"),
          "fail-closed evidence missing");
}

void IndexOnlyAdmissionWorks() {
  const auto assembled = AssembleValidPayload();
  const auto admission = idx::ValidateCoveringIndexPayloadForLocator(
      BaseValidationRequest(assembled.record));
  Require(admission.ok(), "index-only payload admission failed");
  Require(admission.index_only_admitted, "index-only admission not recorded");
  Require(!admission.base_row_recheck_required,
          "index-only path unexpectedly required base-row recheck");
  Require(Has(admission.evidence,
              "covering_payload.required_rechecks_proven=true"),
          "required recheck proof evidence missing");
  Require(Has(admission.evidence,
              "covering_payload.transaction_finality_authority=false"),
          "admission non-finality evidence missing");

  idx::OrderedOverlayRequest overlay;
  overlay.family = idx::IndexFamily::covering;
  overlay.overlay = idx::OrderedOverlayKind::covering;
  overlay.covering_payload_requested = true;
  overlay.covering_payload_columns = 2;
  overlay.covering_payload_admission = &admission;
  const auto decision = idx::DecideOrderedOverlayEligibility(overlay);
  Require(decision.ok(), "ordered overlay rejected valid covering admission");
  Require(decision.index_only_allowed,
          "ordered overlay did not consume index-only admission");
  Require(Has(decision.steps, "consume_covering_payload_admission"),
          "ordered overlay missing admission-consumption step");
}

void BaseRowRecheckHandoffWorks() {
  const auto assembled = AssembleValidPayload();
  auto request = BaseValidationRequest(assembled.record);
  request.allow_index_only = false;
  request.exact_predicate_rechecked_by_engine = false;
  request.mga_visibility_rechecked_by_engine = false;
  request.security_authorized_by_engine = false;
  const auto admission = idx::ValidateCoveringIndexPayloadForLocator(request);
  Require(admission.ok(), "base-row recheck handoff admission failed");
  Require(!admission.index_only_admitted,
          "base-row handoff was incorrectly index-only");
  Require(admission.base_row_recheck_required &&
              admission.base_row_recheck_handoff_proven,
          "base-row recheck handoff evidence missing");
  Require(Has(admission.evidence,
              "covering_payload.base_row_recheck_handoff=true"),
          "base-row recheck handoff evidence not emitted");

  idx::OrderedOverlayRequest overlay;
  overlay.family = idx::IndexFamily::covering;
  overlay.overlay = idx::OrderedOverlayKind::covering;
  overlay.covering_payload_requested = true;
  overlay.covering_payload_columns = 2;
  overlay.can_recheck_base_row = true;
  overlay.covering_payload_admission = &admission;
  const auto decision = idx::DecideOrderedOverlayEligibility(overlay);
  Require(decision.ok() && decision.requires_recheck &&
              !decision.index_only_allowed,
          "ordered overlay did not preserve base-row recheck handoff");
}

void FailClosedCasesAreExact() {
  const auto assembled = AssembleValidPayload();
  const auto& record = assembled.record;

  ExpectValidationBlocker(record, "covering_payload_epoch_stale",
                          "stale descriptor epoch was not refused", [](auto& r) {
                            r.descriptor_epoch_current = false;
                          });
  ExpectValidationBlocker(record, "covering_payload_result_contract_mismatch",
                          "result contract mismatch was not refused", [](auto& r) {
                            r.expected_descriptor_result_contract_hash = "wrong";
                          });
  ExpectValidationBlocker(record, "covering_payload_epoch_mismatch",
                          "redaction epoch mismatch was not refused", [](auto& r) {
                            r.expected_redaction_policy_epoch = 201;
                          });
  ExpectValidationBlocker(record, "covering_payload_missing_column",
                          "missing payload column was not refused", [](auto& r) {
                            r.record.values.pop_back();
                          });
  ExpectValidationBlocker(record, "covering_payload_column_set_invalid",
                          "duplicate payload column was not refused", [](auto& r) {
                            r.record.values.push_back(r.record.values.front());
                          });
  ExpectValidationBlocker(record, "covering_payload_unknown_column",
                          "unknown payload column was not refused", [](auto& r) {
                            auto extra = r.record.values.front();
                            extra.column_uuid = ObjectUuid(9);
                            r.record.values.push_back(extra);
                          });
  ExpectValidationBlocker(record, "covering_payload_row_version_mismatch",
                          "row UUID mismatch was not refused", [](auto& r) {
                            r.locator.row_uuid = RowUuid(99);
                          });
  ExpectValidationBlocker(record, "covering_payload_generation_mismatch",
                          "payload generation mismatch was not refused", [](auto& r) {
                            r.expected_payload_generation = 101;
                          });
  ExpectValidationBlocker(record, "covering_payload_large_descriptor_mismatch",
                          "large descriptor mismatch was not refused", [](auto& r) {
                            r.expected_large_payloads.front().descriptor =
                                LargeRef(91);
                          });
  ExpectValidationBlocker(record, "covering_payload_physical_layout_mismatch",
                          "physical payload body mismatch was not refused", [](auto& r) {
                            r.record.physical_payload.push_back(0xff);
                          });
  ExpectValidationBlocker(record, "covering_payload_expected_proof_missing",
                          "missing expected proof was not refused", [](auto& r) {
                            r.expected_descriptor_result_contract_hash.clear();
                          });
  ExpectValidationBlocker(record, "covering_payload_mga_security_recheck_missing",
                          "missing MGA/security recheck was not refused", [](auto& r) {
                            r.mga_visibility_recheck_planned = false;
                            r.mga_visibility_rechecked_by_engine = false;
                          });
  ExpectValidationBlocker(record, "covering_payload_unsafe_authority",
                          "unsafe authority drift was not refused", [](auto& r) {
                            r.parser_or_donor_finality_authority = true;
                          });
  ExpectValidationBlocker(record, "covering_payload_redaction_policy_unsafe",
                          "unsafe redaction policy was not refused", [](auto& r) {
                            r.record.values.front().redaction_safe = false;
                          });
  ExpectValidationBlocker(record, "covering_payload_base_row_recheck_missing",
                          "missing base-row handoff was not refused", [](auto& r) {
                            r.allow_index_only = false;
                            r.exact_predicate_rechecked_by_engine = false;
                            r.mga_visibility_rechecked_by_engine = false;
                            r.security_authorized_by_engine = false;
                            r.base_row_recheck_available = false;
                          });
}

void AssemblyRefusalsAreExact() {
  auto duplicate = BaseAssemblyRequest();
  duplicate.projected_column_uuids.push_back(
      duplicate.projected_column_uuids.front());
  auto result = idx::AssembleCoveringIndexPayload(duplicate);
  Require(!result.ok() && result.fail_closed,
          "duplicate projected column assembly was not refused");
  Require(Has(result.refusal_reasons, "SB_COVERING_PAYLOAD.COLUMN_SET_INVALID"),
          "duplicate assembly refusal code mismatch");

  auto unknown = BaseAssemblyRequest();
  unknown.projected_column_uuids.push_back(ObjectUuid(30));
  result = idx::AssembleCoveringIndexPayload(unknown);
  Require(!result.ok() && result.fail_closed,
          "unknown projected column assembly was not refused");
  Require(Has(result.refusal_reasons, "SB_COVERING_PAYLOAD.UNKNOWN_COLUMN"),
          "unknown assembly refusal code mismatch");

  auto unsafe = BaseAssemblyRequest();
  unsafe.client_finality_authority = true;
  result = idx::AssembleCoveringIndexPayload(unsafe);
  Require(!result.ok() && result.fail_closed,
          "unsafe authority assembly was not refused");
  Require(Has(result.refusal_reasons, "SB_COVERING_PAYLOAD.UNSAFE_AUTHORITY"),
          "unsafe authority assembly refusal code mismatch");
}

opt::OptimizerStatsIdentity FreshIdentity(const std::string& object_uuid,
                                          const std::string& statistic_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = 32;
  identity.catalog_epoch = 32;
  identity.transaction_visibility_epoch = 32;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

const opt::PlanCandidate* FindCandidate(
    const std::vector<opt::PlanCandidate>& candidates,
    const std::string& id) {
  const auto found = std::find_if(candidates.begin(), candidates.end(),
                                  [&](const opt::PlanCandidate& candidate) {
                                    return candidate.candidate_id == id;
                                  });
  return found == candidates.end() ? nullptr : &*found;
}

void OptimizerCarriesPayloadProofWithoutRuntimeAdmission() {
  opt::AccessPathPlanningRequest request;
  request.relation_uuid = "rel.covering.payload";
  request.predicate_kind = "scalar_eq";
  request.descriptor_digest = "desc:covering";
  request.projected_column_uuids = {"col.covered"};
  request.visibility_proven = true;
  request.grants_proven = true;
  request.index_visibility_native = true;
  opt::TableCardinalityStats table;
  table.identity = FreshIdentity(request.relation_uuid, "table.stats");
  table.row_count = 100;
  table.visible_row_count = 100;
  table.page_count = 4;
  table.average_row_bytes = 32;
  request.table_stats = table;
  opt::IndexStats index;
  index.identity = FreshIdentity("idx.covering.payload", "index.stats");
  index.index_uuid = "idx.covering.payload";
  index.relation_uuid = request.relation_uuid;
  index.index_family = "btree";
  index.key_column_uuids = {"col.covered"};
  index.covered_column_uuids = {"col.covered"};
  index.covering = true;
  index.height = 2;
  index.leaf_pages = 8;
  index.distinct_keys = 90;
  index.visibility_coverage = 1.0;
  request.candidate_indexes = {index};

  const auto missing_proof_candidates =
      opt::GenerateFullAccessPathCandidates(request);
  const auto* missing_proof =
      FindCandidate(missing_proof_candidates,
                    "CAND-OPT-COVERING:idx.covering.payload");
  Require(missing_proof != nullptr,
          "covering optimizer candidate missing without payload proof");
  Require(!missing_proof->cost.selectable,
          "covering candidate without physical payload proof was selectable");
  Require(missing_proof->cost.rejection_reason ==
              "covering_payload_proof_missing",
          "covering missing-payload-proof blocker was not exact");

  request.covering_payload.physical_payload_proof_present = true;
  request.covering_payload.freshness_proven = true;
  request.covering_payload.redaction_safe = true;
  request.covering_payload.result_contract_proven = true;
  request.covering_payload.base_row_recheck_handoff_proven = true;
  request.covering_payload.index_only_admitted = false;
  request.covering_payload.runtime_route_consumption_required = true;
  request.covering_payload.evidence = {
      "covering_payload.test_physical_payload_layout=true"};

  const auto candidates = opt::GenerateFullAccessPathCandidates(request);
  const auto* covering =
      FindCandidate(candidates, "CAND-OPT-COVERING:idx.covering.payload");
  Require(covering != nullptr, "covering optimizer candidate missing");
  Require(!covering->cost.selectable,
          "covering candidate advertised runtime route consumption");
  Require(covering->cost.rejection_reason ==
              "covering_runtime_route_consumption_pending",
          "covering runtime blocker was not exact");
  Require(Has(covering->runtime_evidence,
              "covering_payload.physical_payload_proof_present=true"),
          "covering payload proof evidence missing from optimizer route");
  Require(Has(covering->runtime_evidence,
              "covering_payload.runtime_route_consumption_pending=true"),
          "runtime-pending evidence missing from optimizer route");
}

void CapabilityRuntimeCompleteWithPayloadLayout() {
  const auto* state =
      idx::FindBuiltinIndexFamilyPhysicalCapabilityState(idx::IndexFamily::covering);
  Require(state != nullptr, "covering family capability state missing");
  Require(state->runtime_available,
          "covering family runtime availability missing");
  Require(state->benchmark_clean,
          "covering family benchmark-clean missing");
  Require(state->physically_complete(),
          "covering family physical implementation incomplete");
  Require(state->blocker == idx::IndexFamilyPhysicalCapabilityBlocker::none,
          "covering family retained a blocker");
  Require(state->blocker_diagnostic_code.empty(),
          "covering family retained a blocker diagnostic");
}

}  // namespace

int main() {
  IndexOnlyAdmissionWorks();
  BaseRowRecheckHandoffWorks();
  FailClosedCasesAreExact();
  AssemblyRefusalsAreExact();
  OptimizerCarriesPayloadProofWithoutRuntimeAdmission();
  CapabilityRuntimeCompleteWithPayloadLayout();
  std::cout << "covering_index_payload_gate=passed\n";
  return EXIT_SUCCESS;
}
