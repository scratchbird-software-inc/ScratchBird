// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "covering_index_payload.hpp"
#include "index_key_encoding.hpp"
#include "late_materialization_covering_scan_runtime.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "index_plan_shape_regression_guard_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Has(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 millis,
                                  platform::byte suffix) {
  const auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  auto value = generated.value;
  value.bytes[15] = suffix;
  const auto typed = uuid::MakeTypedUuid(kind, value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

platform::TypedUuid StableTypedUuid(platform::UuidKind kind,
                                    platform::byte seed) {
  platform::TypedUuid value;
  value.kind = kind;
  for (std::size_t i = 0; i < value.value.bytes.size(); ++i) {
    value.value.bytes[i] =
        static_cast<platform::byte>(seed + static_cast<platform::byte>(i));
  }
  value.value.bytes[6] =
      static_cast<platform::byte>((value.value.bytes[6] & 0x0fu) | 0x70u);
  value.value.bytes[8] =
      static_cast<platform::byte>((value.value.bytes[8] & 0x3fu) | 0x80u);
  return value;
}

std::string UuidText(platform::UuidKind kind,
                     platform::u64 millis,
                     platform::byte suffix) {
  return uuid::UuidToString(GeneratedUuid(kind, millis, suffix).value);
}

platform::TypedUuid ParseTyped(platform::UuidKind kind,
                               const std::string& text) {
  const auto parsed = uuid::ParseDurableEngineIdentityUuid(kind, text);
  Require(parsed.ok(), "typed uuid parse failed");
  return parsed.value;
}

std::vector<platform::byte> Bytes(std::string_view text) {
  return std::vector<platform::byte>(text.begin(), text.end());
}

std::vector<platform::byte> EncodedKey(const std::string& index_uuid,
                                       const std::string& key) {
  const auto descriptor_uuid =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           index_uuid);
  Require(descriptor_uuid.ok(), "index uuid parse for key encoding failed");
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = descriptor_uuid.value;
  component.payload.assign(key.begin(), key.end());
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "test key encoding failed");
  return encoded.encoded;
}

page::IndexBtreePhysicalScanBound Bound(const std::string& index_uuid,
                                        const std::string& key,
                                        bool inclusive = true) {
  page::IndexBtreePhysicalScanBound bound;
  bound.unbounded = false;
  bound.inclusive = inclusive;
  bound.encoded_key = EncodedKey(index_uuid, key);
  return bound;
}

page::IndexBtreePhysicalTree MakeTree(const std::string& index_uuid) {
  auto initialized = page::InitializeIndexBtreePhysicalTree(
      ParseTyped(platform::UuidKind::object, index_uuid), 768);
  Require(initialized.ok(), "physical btree init failed");
  return std::move(initialized.tree);
}

page::IndexBtreeCell Cell(const std::string& index_uuid,
                          const std::string& key,
                          const std::string& row_uuid,
                          const std::string& version_uuid) {
  page::IndexBtreeCell cell;
  cell.key_ordinal = 0;
  cell.encoded_key = EncodedKey(index_uuid, key);
  cell.row_uuid = ParseTyped(platform::UuidKind::row, row_uuid);
  cell.version_uuid = ParseTyped(platform::UuidKind::row, version_uuid);
  return cell;
}

void InsertCell(page::IndexBtreePhysicalTree* tree,
                const page::IndexBtreeCell& cell) {
  page::IndexBtreePhysicalInsertRequest request;
  request.cell = cell;
  const auto inserted = page::InsertIndexBtreeCell(tree, request);
  Require(inserted.ok(), "physical insert failed");
}

exec::IndexedPhysicalOperatorRequest BaseRequest(
    exec::IndexedPhysicalOperatorKind kind,
    const page::IndexBtreePhysicalTree* tree) {
  exec::IndexedPhysicalOperatorRequest request;
  request.kind = kind;
  request.physical_tree = tree;
  request.plan_safe = true;
  request.physical_tree_available = true;
  request.encoded_key_proof = true;
  request.encoded_bounds_proof = true;
  request.durable_mga_inventory_proof = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  return request;
}

struct Fixture {
  std::string index_uuid =
      UuidText(platform::UuidKind::object, 1700620000000ull, 0x41);
  std::string table_uuid =
      UuidText(platform::UuidKind::object, 1700620001000ull, 0x42);
  std::string row_alpha =
      UuidText(platform::UuidKind::row, 1700620100000ull, 0x51);
  std::string row_bravo =
      UuidText(platform::UuidKind::row, 1700620101000ull, 0x52);
  std::string row_charlie =
      UuidText(platform::UuidKind::row, 1700620102000ull, 0x53);
  std::string version_alpha =
      UuidText(platform::UuidKind::row, 1700620200000ull, 0x61);
  std::string version_bravo =
      UuidText(platform::UuidKind::row, 1700620201000ull, 0x62);
  std::string version_charlie =
      UuidText(platform::UuidKind::row, 1700620202000ull, 0x63);
  page::IndexBtreePhysicalTree tree = MakeTree(index_uuid);

  Fixture() {
    InsertCell(&tree, Cell(index_uuid, "alpha", row_alpha, version_alpha));
    InsertCell(&tree, Cell(index_uuid, "bravo", row_bravo, version_bravo));
    InsertCell(&tree, Cell(index_uuid, "charlie", row_charlie, version_charlie));
  }
};

exec::IndexedPhysicalOperatorResult PointStream(const Fixture& fixture) {
  auto request = BaseRequest(exec::IndexedPhysicalOperatorKind::point_lookup,
                             &fixture.tree);
  request.encoded_point_key = EncodedKey(fixture.index_uuid, "bravo");
  return exec::ExecuteIndexedPhysicalOperator(request);
}

exec::IndexedPhysicalOperatorResult RangeStream(const Fixture& fixture) {
  auto request = BaseRequest(exec::IndexedPhysicalOperatorKind::range_scan,
                             &fixture.tree);
  request.lower_bound = Bound(fixture.index_uuid, "alpha");
  request.upper_bound = Bound(fixture.index_uuid, "charlie");
  return exec::ExecuteIndexedPhysicalOperator(request);
}

exec::IndexedPhysicalOperatorResult OrderedLimitStream(const Fixture& fixture) {
  auto request = BaseRequest(exec::IndexedPhysicalOperatorKind::ordered_limit,
                             &fixture.tree);
  request.limit = 2;
  return exec::ExecuteIndexedPhysicalOperator(request);
}

idx::CoveringIndexPayloadColumnRef Column(platform::byte seed,
                                          platform::u32 ordinal) {
  idx::CoveringIndexPayloadColumnRef column;
  column.column_uuid = StableTypedUuid(platform::UuidKind::object, seed);
  column.type_descriptor_uuid = StableTypedUuid(
      platform::UuidKind::object, static_cast<platform::byte>(seed + 40));
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

idx::CoveringIndexPayloadAdmission AdmitPayload(
    const Fixture& fixture,
    const exec::IndexedPhysicalOperatorLocator& locator,
    std::string_view first,
    std::string_view second) {
  const auto c1 = Column(1, 0);
  const auto c2 = Column(2, 1);
  idx::CoveringIndexPayloadAssemblyRequest assembly;
  assembly.index_uuid = ParseTyped(platform::UuidKind::object,
                                   fixture.index_uuid);
  assembly.table_uuid = ParseTyped(platform::UuidKind::object,
                                   fixture.table_uuid);
  assembly.row_uuid = ParseTyped(platform::UuidKind::row, locator.row_uuid);
  assembly.version_uuid = ParseTyped(platform::UuidKind::row,
                                     locator.version_uuid);
  assembly.descriptor_result_contract_hash = "contract:irc062:v1";
  assembly.payload_generation = 10;
  assembly.redaction_policy_epoch = 20;
  assembly.security_policy_epoch = 30;
  assembly.freshness_generation = 40;
  assembly.descriptor_columns = {c1, c2};
  assembly.projected_column_uuids = {c1.column_uuid, c2.column_uuid};
  assembly.values = {InlineValue(c1, first), InlineValue(c2, second)};
  assembly.projection_only = true;
  assembly.result_contract_bound = true;
  const auto assembled = idx::AssembleCoveringIndexPayload(assembly);
  Require(assembled.ok(), "covering payload assembly failed");

  idx::CoveringIndexPayloadValidationRequest validation;
  validation.record = assembled.record;
  validation.locator.encoded_key = Bytes("encoded-key");
  validation.locator.row_uuid = assembled.record.row_uuid;
  validation.locator.version_uuid = assembled.record.version_uuid;
  validation.locator.leaf_page_number = 7;
  validation.locator.cell_ordinal = 1;
  validation.locator.physical_btree_locator_scan = true;
  validation.required_columns = {c1, c2};
  validation.projected_column_uuids = {c1.column_uuid, c2.column_uuid};
  validation.expected_descriptor_result_contract_hash = "contract:irc062:v1";
  validation.expected_payload_generation = 10;
  validation.expected_redaction_policy_epoch = 20;
  validation.expected_security_policy_epoch = 30;
  validation.expected_freshness_generation = 40;
  validation.descriptor_epoch_current = true;
  validation.result_contract_current = true;
  validation.redaction_epoch_current = true;
  validation.security_epoch_current = true;
  validation.freshness_current = true;
  validation.result_frame_contract_proven = true;
  validation.redaction_policy_safe = true;
  validation.exact_predicate_recheck_planned = true;
  validation.mga_visibility_recheck_planned = true;
  validation.security_authorization_recheck_planned = true;
  validation.exact_predicate_rechecked_by_engine = true;
  validation.mga_visibility_rechecked_by_engine = true;
  validation.security_authorized_by_engine = true;
  validation.base_row_recheck_available = true;
  validation.allow_index_only = true;
  const auto admitted = idx::ValidateCoveringIndexPayloadForLocator(validation);
  Require(admitted.ok(), "covering payload admission failed");
  return admitted;
}

exec::LateMaterializationIndexedRuntimeResult LateResult(
    const exec::IndexedPhysicalOperatorResult& stream) {
  return exec::ConsumeIndexedRowIdStreamForLateMaterialization(
      stream, {},
      [](const exec::IndexedPhysicalOperatorLocator& locator) {
        exec::LateMaterializationIndexedProviderResult out;
        out.ok = true;
        out.row.row_uuid = locator.row_uuid;
        out.row.version_uuid = locator.version_uuid;
        out.row.projected_values = {"base:" + locator.row_uuid};
        return out;
      });
}

exec::CoveringProjectionOnlyScanResult CoveringResult(
    const Fixture& fixture,
    const exec::IndexedPhysicalOperatorResult& stream) {
  std::vector<idx::CoveringIndexPayloadAdmission> admissions;
  admissions.push_back(AdmitPayload(fixture, stream.locators[0], "a", "one"));
  admissions.push_back(AdmitPayload(fixture, stream.locators[1], "b", "two"));
  admissions.push_back(AdmitPayload(fixture, stream.locators[2], "c", "three"));

  exec::CoveringProjectionOnlyScanRequest request;
  request.physical_stream = &stream;
  for (const auto& admission : admissions) {
    request.admissions.push_back(&admission);
  }
  return exec::ExecuteCoveringProjectionOnlyScan(request);
}

void RequireGuardOk(const exec::IndexPlanShapeRegressionGuardResult& result,
                    std::string_view route) {
  Require(result.ok, "plan-shape guard rejected physical route");
  Require(result.physical_route_consumed,
          "plan-shape guard did not report physical route consumption");
  Require(!result.exact_blocker, "positive route reported exact blocker");
  Require(!result.scan_only_regression, "positive route reported scan fallback");
  Require(!result.per_row_wrapper_regression,
          "positive route reported per-row wrapper");
  Require(!result.statistics_only_regression,
          "positive route reported statistics-only route");
  Require(!result.benchmark_clean, "plan-shape guard claimed benchmark-clean");
  Require(Has(result.evidence, "irc062.route=" + std::string(route)),
          "route evidence missing");
  Require(Has(result.evidence, "irc062.physical_route_consumed=true"),
          "physical route evidence missing");
  Require(Has(result.evidence, "irc062.benchmark_clean=false"),
          "benchmark-clean false evidence missing");
}

void ExpectRefusal(const exec::IndexPlanShapeRegressionGuardRequest& request,
                   std::string_view diagnostic,
                   bool exact_blocker) {
  const auto result = exec::EvaluateIndexPlanShapeRegressionGuard(request);
  Require(!result.ok, "plan-shape guard did not fail closed");
  Require(result.diagnostic_code == diagnostic,
          "plan-shape guard diagnostic mismatch");
  Require(result.exact_blocker == exact_blocker,
          "plan-shape guard exact blocker classification mismatch");
  Require(!result.benchmark_clean, "refusal claimed benchmark-clean");
}

exec::IndexPlanShapeRegressionGuardRequest GuardRequest(
    std::string route,
    exec::IndexPlanShapeRequiredPath path,
    const exec::IndexedPhysicalOperatorResult* stream) {
  exec::IndexPlanShapeRegressionGuardRequest request;
  request.route_name = std::move(route);
  request.required_path = path;
  request.physical_result = stream;
  return request;
}

void PositiveRoutesReportPhysicalConsumption() {
  const Fixture fixture;

  const auto point = PointStream(fixture);
  Require(point.ok, "point physical route failed");
  RequireGuardOk(
      exec::EvaluateIndexPlanShapeRegressionGuard(GuardRequest(
          "indexed_point_lookup", exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
          &point)),
      "indexed_point_lookup");

  const auto range = RangeStream(fixture);
  Require(range.ok, "range physical route failed");
  RequireGuardOk(
      exec::EvaluateIndexPlanShapeRegressionGuard(GuardRequest(
          "indexed_range_scan", exec::IndexPlanShapeRequiredPath::indexed_range_scan,
          &range)),
      "indexed_range_scan");

  const auto ordered = OrderedLimitStream(fixture);
  Require(ordered.ok, "ordered-limit physical route failed");
  RequireGuardOk(
      exec::EvaluateIndexPlanShapeRegressionGuard(GuardRequest(
          "indexed_ordered_limit",
          exec::IndexPlanShapeRequiredPath::indexed_ordered_limit,
          &ordered)),
      "indexed_ordered_limit");

  const auto late = LateResult(range);
  exec::IndexPlanShapeRegressionGuardRequest late_request = GuardRequest(
      "late_materialization_row_id_stream",
      exec::IndexPlanShapeRequiredPath::late_materialization_row_id_stream,
      &range);
  late_request.late_materialization_result = &late;
  RequireGuardOk(exec::EvaluateIndexPlanShapeRegressionGuard(late_request),
                 "late_materialization_row_id_stream");

  const auto covering = CoveringResult(fixture, range);
  exec::IndexPlanShapeRegressionGuardRequest covering_request = GuardRequest(
      "covering_projection_only",
      exec::IndexPlanShapeRequiredPath::covering_projection_only,
      &range);
  covering_request.covering_projection_result = &covering;
  RequireGuardOk(exec::EvaluateIndexPlanShapeRegressionGuard(covering_request),
                 "covering_projection_only");
}

void ScanAndWrapperRegressionsFailClosed() {
  const Fixture fixture;
  const auto stream = PointStream(fixture);
  Require(stream.ok, "physical stream for negative coverage failed");

  auto request = GuardRequest("table_scan_regression",
                              exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                              &stream);
  request.table_scan_fallback = true;
  ExpectRefusal(request,
                "SB-IRC062-TABLE-SCAN-FALLBACK-REGRESSION",
                false);

  request = GuardRequest("descriptor_scan_regression",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &stream);
  request.descriptor_scan_fallback = true;
  ExpectRefusal(request,
                "SB-IRC062-DESCRIPTOR-MAP-SCAN-FALLBACK-REGRESSION",
                false);

  request = GuardRequest("map_scan_regression",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &stream);
  request.map_scan_fallback = true;
  ExpectRefusal(request,
                "SB-IRC062-DESCRIPTOR-MAP-SCAN-FALLBACK-REGRESSION",
                false);

  request = GuardRequest("per_row_wrapper_regression",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &stream);
  request.per_row_wrapper_execution = true;
  ExpectRefusal(request, "SB-IRC062-PER-ROW-WRAPPER-REGRESSION", false);

  request = GuardRequest("statistics_only_regression",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &stream);
  request.statistics_only_optimizer_route = true;
  ExpectRefusal(request,
                "SB-IRC062-STATISTICS-ONLY-LOCAL-CANDIDATE-REGRESSION",
                false);

  request = GuardRequest("local_candidate_regression",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &stream);
  request.local_candidate_planning = true;
  ExpectRefusal(request,
                "SB-IRC062-STATISTICS-ONLY-LOCAL-CANDIDATE-REGRESSION",
                false);

  request = GuardRequest("benchmark_clean_claim",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &stream);
  request.benchmark_clean_claim = true;
  ExpectRefusal(request, "SB-IRC062-BENCHMARK-CLEAN-CLAIM-FORBIDDEN", false);

  request = GuardRequest("conflicting_blocker_and_physical_route",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &stream);
  request.missing_physical_tree_blocker = true;
  request.table_scan_fallback = true;
  ExpectRefusal(request,
                "SB-IRC062-CONFLICTING-BLOCKER-AND-PHYSICAL-ROUTE",
                false);
}

void ExactBlockersAreNotReportedAsRegressions() {
  const Fixture fixture;
  auto missing_tree = BaseRequest(exec::IndexedPhysicalOperatorKind::point_lookup,
                                  nullptr);
  auto blocked_stream = exec::ExecuteIndexedPhysicalOperator(missing_tree);
  auto request = GuardRequest("missing_physical_tree",
                              exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                              &blocked_stream);
  request.table_scan_fallback = true;
  ExpectRefusal(request,
                "SB-IRC062-BLOCKED-MISSING-PHYSICAL-TREE",
                true);

  auto stale = BaseRequest(exec::IndexedPhysicalOperatorKind::point_lookup,
                           &fixture.tree);
  stale.plan_safe = false;
  blocked_stream = exec::ExecuteIndexedPhysicalOperator(stale);
  request = GuardRequest("stale_plan",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &blocked_stream);
  request.descriptor_scan_fallback = true;
  ExpectRefusal(request, "SB-IRC062-BLOCKED-STALE-PLAN", true);

  auto missing_mga = BaseRequest(exec::IndexedPhysicalOperatorKind::point_lookup,
                                 &fixture.tree);
  missing_mga.encoded_point_key = EncodedKey(fixture.index_uuid, "alpha");
  missing_mga.durable_mga_inventory_proof = false;
  blocked_stream = exec::ExecuteIndexedPhysicalOperator(missing_mga);
  request = GuardRequest("missing_mga_proof",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &blocked_stream);
  request.per_row_wrapper_execution = true;
  ExpectRefusal(request,
                "SB-IRC062-BLOCKED-MISSING-MGA-SECURITY-REDACTION-PROOF",
                true);

  auto missing_key = BaseRequest(exec::IndexedPhysicalOperatorKind::point_lookup,
                                 &fixture.tree);
  blocked_stream = exec::ExecuteIndexedPhysicalOperator(missing_key);
  request = GuardRequest("missing_encoded_key",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &blocked_stream);
  request.statistics_only_optimizer_route = true;
  ExpectRefusal(request,
                "SB-IRC062-BLOCKED-MISSING-ENCODED-KEY-OR-BOUNDS",
                true);

  const auto stream = RangeStream(fixture);
  exec::IndexRuntimeEngineRecheckProof proof;
  proof.redaction_checked_by_engine = false;
  const auto late_blocked =
      exec::ConsumeIndexedRowIdStreamForLateMaterialization(
          stream, proof,
          [](const exec::IndexedPhysicalOperatorLocator& locator) {
            exec::LateMaterializationIndexedProviderResult out;
            out.ok = true;
            out.row.row_uuid = locator.row_uuid;
            out.row.version_uuid = locator.version_uuid;
            return out;
          });
  request = GuardRequest("missing_redaction_proof",
                         exec::IndexPlanShapeRequiredPath::late_materialization_row_id_stream,
                         &stream);
  request.late_materialization_result = &late_blocked;
  request.map_scan_fallback = true;
  ExpectRefusal(request,
                "SB-IRC062-BLOCKED-MISSING-MGA-SECURITY-REDACTION-PROOF",
                true);

  exec::CoveringProjectionOnlyScanRequest covering_request;
  covering_request.physical_stream = &stream;
  const auto covering_blocked =
      exec::ExecuteCoveringProjectionOnlyScan(covering_request);
  request = GuardRequest("missing_covering_payload",
                         exec::IndexPlanShapeRequiredPath::covering_projection_only,
                         &stream);
  request.covering_projection_result = &covering_blocked;
  request.table_scan_fallback = true;
  ExpectRefusal(request,
                "SB-IRC062-BLOCKED-MISSING-COVERING-PAYLOAD",
                true);

  request = GuardRequest("unsupported_family",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &stream);
  request.physical_result = nullptr;
  request.unsupported_physical_family_blocker = true;
  request.unsupported_physical_family = "contract_only_hash";
  request.local_candidate_planning = true;
  ExpectRefusal(request,
                "SB-IRC062-BLOCKED-UNSUPPORTED-PHYSICAL-FAMILY",
                true);
}

}  // namespace

int main() {
  PositiveRoutesReportPhysicalConsumption();
  ScanAndWrapperRegressionsFailClosed();
  ExactBlockersAreNotReportedAsRegressions();
  std::cout << "index_plan_shape_regression_guard_gate=passed\n";
  return EXIT_SUCCESS;
}
