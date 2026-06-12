// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "indexed_physical_operator.hpp"
#include "index_key_encoding.hpp"
#include "index_route_capability.hpp"
#include "late_materialization_covering_scan_runtime.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << "ORH-286 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(std::string(message));
  }
}

platform::TypedUuid StableUuid(platform::UuidKind kind, platform::byte seed) {
  platform::TypedUuid out;
  out.kind = kind;
  for (std::size_t i = 0; i < out.value.bytes.size(); ++i) {
    out.value.bytes[i] =
        static_cast<platform::byte>(seed + static_cast<platform::byte>(i));
  }
  out.value.bytes[6] =
      static_cast<platform::byte>((out.value.bytes[6] & 0x0fu) | 0x70u);
  out.value.bytes[8] =
      static_cast<platform::byte>((out.value.bytes[8] & 0x3fu) | 0x80u);
  return out;
}

std::string StableHash(const std::vector<std::string>& fields) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& field : fields) {
    for (const unsigned char ch : field) {
      hash ^= ch;
      hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << "fnv64:" << std::hex << hash;
  return out.str();
}

bool HasEvidence(
    const std::vector<scratchbird::engine::internal_api::EngineEvidenceReference>&
        evidence,
    std::string_view kind,
    std::string_view id) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.evidence_kind == kind &&
           item.evidence_id.find(id) != std::string::npos;
  });
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

std::vector<platform::byte> Bytes(std::string_view text) {
  return std::vector<platform::byte>(text.begin(), text.end());
}

std::vector<platform::byte> EncodedKey(
    const platform::TypedUuid& index_uuid,
    std::string_view key) {
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = index_uuid;
  component.payload = Bytes(key);
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "index key encoding failed");
  return encoded.encoded;
}

page::IndexBtreePhysicalScanBound Bound(
    const platform::TypedUuid& index_uuid,
    std::string_view key,
    bool inclusive = true) {
  page::IndexBtreePhysicalScanBound bound;
  bound.unbounded = false;
  bound.inclusive = inclusive;
  bound.encoded_key = EncodedKey(index_uuid, key);
  return bound;
}

page::IndexBtreeCell Cell(const platform::TypedUuid& index_uuid,
                          std::string_view key,
                          platform::byte row_seed,
                          platform::byte version_seed) {
  page::IndexBtreeCell cell;
  cell.key_ordinal = 0;
  cell.encoded_key = EncodedKey(index_uuid, key);
  cell.row_uuid = StableUuid(platform::UuidKind::row, row_seed);
  cell.version_uuid = StableUuid(platform::UuidKind::row, version_seed);
  return cell;
}

struct Fixture {
  platform::TypedUuid index_uuid = StableUuid(platform::UuidKind::object, 0x20);
  page::IndexBtreePhysicalTree tree;

  Fixture() {
    auto initialized = page::InitializeIndexBtreePhysicalTree(index_uuid, 768);
    Require(initialized.ok(), "physical btree init failed");
    tree = std::move(initialized.tree);
    for (const auto& cell : {
             Cell(index_uuid, "alpha", 0x31, 0x71),
             Cell(index_uuid, "bravo", 0x32, 0x72),
             Cell(index_uuid, "charlie", 0x33, 0x73),
         }) {
      page::IndexBtreePhysicalInsertRequest insert;
      insert.cell = cell;
      const auto inserted = page::InsertIndexBtreeCell(&tree, insert);
      Require(inserted.ok(), "physical btree insert failed");
    }
  }
};

exec::IndexedPhysicalOperatorRequest BaseIndexedRequest(
    exec::IndexedPhysicalOperatorKind kind,
    const Fixture& fixture) {
  exec::IndexedPhysicalOperatorRequest request;
  request.kind = kind;
  request.physical_tree = &fixture.tree;
  request.plan_safe = true;
  request.physical_tree_available = true;
  request.encoded_key_proof = true;
  request.encoded_bounds_proof = true;
  request.durable_mga_inventory_proof = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  return request;
}

exec::IndexedPhysicalOperatorResult PointLookup(const Fixture& fixture) {
  auto request = BaseIndexedRequest(
      exec::IndexedPhysicalOperatorKind::point_lookup, fixture);
  request.encoded_point_key = EncodedKey(fixture.index_uuid, "bravo");
  return exec::ExecuteIndexedPhysicalOperator(request);
}

exec::IndexedPhysicalOperatorResult RangeScan(const Fixture& fixture) {
  auto request = BaseIndexedRequest(
      exec::IndexedPhysicalOperatorKind::range_scan, fixture);
  request.lower_bound = Bound(fixture.index_uuid, "alpha");
  request.upper_bound = Bound(fixture.index_uuid, "charlie");
  return exec::ExecuteIndexedPhysicalOperator(request);
}

exec::IndexedPhysicalOperatorResult OrderedLimit(const Fixture& fixture) {
  auto request = BaseIndexedRequest(
      exec::IndexedPhysicalOperatorKind::ordered_limit, fixture);
  request.limit = 2;
  return exec::ExecuteIndexedPhysicalOperator(request);
}

exec::LateMaterializationIndexedRuntimeResult LateMaterialize(
    const exec::IndexedPhysicalOperatorResult& stream) {
  return exec::ConsumeIndexedRowIdStreamForLateMaterialization(
      stream,
      {},
      [](const exec::IndexedPhysicalOperatorLocator& locator) {
        exec::LateMaterializationIndexedProviderResult provided;
        provided.ok = true;
        provided.row.row_uuid = locator.row_uuid;
        provided.row.version_uuid = locator.version_uuid;
        provided.row.projected_values = {"base:" + locator.row_uuid};
        return provided;
      });
}

exec::IndexPlanShapeRegressionGuardRequest GuardRequest(
    std::string route_label,
    exec::IndexPlanShapeRequiredPath path,
    const exec::IndexedPhysicalOperatorResult* physical) {
  exec::IndexPlanShapeRegressionGuardRequest request;
  request.route_name = std::move(route_label);
  request.required_path = path;
  request.physical_result = physical;
  return request;
}

std::string ResultHash(const exec::IndexedPhysicalOperatorResult& result) {
  std::vector<std::string> rows;
  for (const auto& locator : result.locators) {
    rows.push_back(locator.row_uuid + ":" + locator.version_uuid);
  }
  return StableHash(rows);
}

std::string ObservedShapeHash(
    std::string_view route_label,
    exec::IndexPlanShapeRequiredPath path,
    const exec::IndexedPhysicalOperatorResult& physical) {
  std::string operator_name = "missing";
  for (const auto& evidence : physical.evidence) {
    if (evidence.evidence_kind == "indexed_physical_operator") {
      operator_name = evidence.evidence_id;
      break;
    }
  }
  return StableHash({std::string(route_label),
                     exec::IndexPlanShapeRequiredPathName(path),
                     operator_name,
                     "physical_route_consumed=true"});
}

std::string ExpectedShapeHash(std::string_view route_label,
                              exec::IndexPlanShapeRequiredPath path,
                              std::string_view operator_name) {
  return StableHash({std::string(route_label),
                     exec::IndexPlanShapeRequiredPathName(path),
                     std::string(operator_name),
                     "physical_route_consumed=true"});
}

void RequireCapability(idx::IndexRouteKind route,
                       idx::IndexFamily family,
                       bool write_required = false) {
  const auto* capability = idx::FindBuiltinIndexRouteCapabilityState(route, family);
  Require(capability != nullptr, "route capability state missing");
  Require(capability->route_complete(), "route capability not complete");
  Require(!write_required || capability->supports_write,
          "write route capability missing");
  Require(capability->requires_mga_recheck,
          "route capability did not preserve MGA recheck");
  Require(capability->requires_security_recheck,
          "route capability did not preserve security recheck");
}

void VerifyAcceptedRoute(
    std::string route_label,
    idx::IndexRouteKind route_kind,
    idx::IndexFamily family,
    exec::IndexPlanShapeRequiredPath required_path,
    std::string_view expected_operator,
    const exec::IndexedPhysicalOperatorResult& physical,
    const exec::LateMaterializationIndexedRuntimeResult* late = nullptr) {
  RequireCapability(route_kind,
                    family,
                    route_kind == idx::IndexRouteKind::dml_update);
  Require(physical.ok, "physical operator did not run");
  Require(!physical.table_scan_consumed, "physical operator used table scan");
  Require(HasEvidence(physical.evidence,
                      "indexed_physical_operator",
                      expected_operator),
          "physical operator evidence missing");
  Require(HasEvidence(physical.evidence, "mga_visibility_recheck", "required"),
          "MGA recheck evidence missing");
  Require(HasEvidence(physical.evidence, "security_recheck", "required"),
          "security recheck evidence missing");
  Require(HasEvidence(physical.evidence, "parser_or_reference_authority", "false"),
          "parser/reference non-authority evidence missing");

  auto request = GuardRequest(route_label, required_path, &physical);
  request.late_materialization_result = late;
  const auto guard = exec::EvaluateIndexPlanShapeRegressionGuard(request);
  if (!guard.ok) {
    Fail("accepted plan-shape route rejected: " + guard.diagnostic_code);
  }
  Require(guard.physical_route_consumed, "guard missed runtime consumption");
  Require(!guard.benchmark_clean, "guard claimed standalone benchmark-clean");
  Require(HasEvidence(guard.evidence, "irc062.route=" + route_label),
          "route label evidence missing");
  Require(HasEvidence(guard.evidence, "irc062.physical_route_consumed=true"),
          "physical route consumption evidence missing");
  Require(HasEvidence(guard.evidence,
                      "irc062.finality_authority=engine_transaction_inventory"),
          "MGA finality authority evidence missing");
  const std::string expected_hash =
      ExpectedShapeHash(route_label, required_path, expected_operator);
  const std::string observed_hash =
      ObservedShapeHash(route_label, required_path, physical);
  Require(expected_hash == observed_hash,
          "expected/observed plan-shape hash mismatch");
  Require(!ResultHash(physical).empty(), "result hash missing");
}

void PositiveRoutes() {
  const Fixture fixture;
  const auto point = PointLookup(fixture);
  VerifyAcceptedRoute("orh286.sql_select.point_lookup",
                      idx::IndexRouteKind::sql_select,
                      idx::IndexFamily::btree,
                      exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                      "point_lookup",
                      point);

  const auto range = RangeScan(fixture);
  VerifyAcceptedRoute("orh286.sql_select.range_scan",
                      idx::IndexRouteKind::sql_select,
                      idx::IndexFamily::btree,
                      exec::IndexPlanShapeRequiredPath::indexed_range_scan,
                      "range_scan",
                      range);

  const auto ordered = OrderedLimit(fixture);
  VerifyAcceptedRoute("orh286.sql_select.ordered_limit",
                      idx::IndexRouteKind::sql_select,
                      idx::IndexFamily::btree,
                      exec::IndexPlanShapeRequiredPath::indexed_ordered_limit,
                      "ordered_limit",
                      ordered);

  const auto late = LateMaterialize(range);
  VerifyAcceptedRoute(
      "orh286.row_locator.late_materialization",
      idx::IndexRouteKind::dml_update,
      idx::IndexFamily::btree,
      exec::IndexPlanShapeRequiredPath::late_materialization_row_id_stream,
      "range_scan",
      range,
      &late);
}

void ExpectGuardRefusal(
    exec::IndexPlanShapeRegressionGuardRequest request,
    std::string_view expected_code) {
  const auto result = exec::EvaluateIndexPlanShapeRegressionGuard(request);
  Require(!result.ok, "negative plan-shape case was accepted");
  Require(!result.benchmark_clean,
          "negative plan-shape case claimed benchmark-clean");
  if (result.diagnostic_code != expected_code) {
    Fail("diagnostic mismatch expected " + std::string(expected_code) +
         " got " + result.diagnostic_code);
  }
  Require(HasEvidence(result.evidence, "irc062.fail_closed=true"),
          "fail-closed evidence missing");
  Require(HasEvidence(result.evidence, "irc062.diagnostic="),
          "exact diagnostic evidence missing");
}

void NegativeRegressions() {
  const Fixture fixture;
  const auto point = PointLookup(fixture);
  const auto range = RangeScan(fixture);

  auto request = GuardRequest("orh286.scan_only",
                              exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                              &point);
  request.table_scan_fallback = true;
  ExpectGuardRefusal(request, "SB-IRC062-TABLE-SCAN-FALLBACK-REGRESSION");

  request = GuardRequest("orh286.descriptor_scan",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &point);
  request.descriptor_scan_fallback = true;
  ExpectGuardRefusal(request,
                     "SB-IRC062-DESCRIPTOR-MAP-SCAN-FALLBACK-REGRESSION");

  request = GuardRequest("orh286.per_row_wrapper",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &point);
  request.per_row_wrapper_execution = true;
  ExpectGuardRefusal(request, "SB-IRC062-PER-ROW-WRAPPER-REGRESSION");

  request = GuardRequest("orh286.text_result",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &point);
  request.text_result_materialization = true;
  ExpectGuardRefusal(request, "SB-IRC062-TEXT-RESULT-REGRESSION");

  request = GuardRequest("orh286.statistics_only",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &point);
  request.statistics_only_optimizer_route = true;
  ExpectGuardRefusal(request,
                     "SB-IRC062-STATISTICS-ONLY-LOCAL-CANDIDATE-REGRESSION");

  request = GuardRequest("orh286.contract_only",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &point);
  request.contract_only_evidence = true;
  ExpectGuardRefusal(request,
                     "SB-IRC062-CONTRACT-ONLY-NO-RUNTIME-EVIDENCE");

  auto missing_mga = BaseIndexedRequest(
      exec::IndexedPhysicalOperatorKind::point_lookup, fixture);
  missing_mga.encoded_point_key = EncodedKey(fixture.index_uuid, "alpha");
  missing_mga.durable_mga_inventory_proof = false;
  auto blocked = exec::ExecuteIndexedPhysicalOperator(missing_mga);
  request = GuardRequest("orh286.missing_mga_security",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &blocked);
  ExpectGuardRefusal(
      request,
      "SB-IRC062-BLOCKED-MISSING-MGA-SECURITY-REDACTION-PROOF");

  auto reference_authority = BaseIndexedRequest(
      exec::IndexedPhysicalOperatorKind::point_lookup, fixture);
  reference_authority.encoded_point_key = EncodedKey(fixture.index_uuid, "alpha");
  reference_authority.parser_or_reference_authority = true;
  blocked = exec::ExecuteIndexedPhysicalOperator(reference_authority);
  Require(!blocked.ok, "parser/reference authority was accepted");
  Require(blocked.diagnostic_code ==
              "SB-IRC060-PARSER-REFERENCE-AUTHORITY-FORBIDDEN",
          "parser/reference authority diagnostic mismatch");

  request = GuardRequest("orh286.missing_exact_blocker",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         nullptr);
  ExpectGuardRefusal(request,
                     "SB-IRC062-PHYSICAL-ROUTE-MISSING-EXACT-BLOCKER");

  request = GuardRequest("orh286.benchmark_clean_overclaim",
                         exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                         &point);
  request.benchmark_clean_claim = true;
  ExpectGuardRefusal(request, "SB-IRC062-BENCHMARK-CLEAN-CLAIM-FORBIDDEN");

  request = GuardRequest("orh286.reference_dominance_overclaim",
                         exec::IndexPlanShapeRequiredPath::indexed_range_scan,
                         &range);
  request.reference_dominance_claim = true;
  ExpectGuardRefusal(request, "SB-IRC062-REFERENCE-DOMINANCE-CLAIM-FORBIDDEN");
}

void InvalidRouteFamilyAndStaleCapability() {
  const Fixture fixture;
  const auto point = PointLookup(fixture);

  const auto* hash_for_dml = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::dml_insert, idx::IndexFamily::hash);
  Require(hash_for_dml != nullptr, "hash DML route capability missing");
  Require(hash_for_dml->route_complete() && hash_for_dml->supports_write &&
              hash_for_dml->supports_mutation &&
              hash_for_dml->requires_exact_recheck,
          "hash family was not admitted for DML ledger mutation route");

  const auto* reference = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::sql_select, idx::IndexFamily::reference_emulated);
  Require(reference != nullptr && !reference->route_complete(),
          "reference emulated family became route authority");

  auto stale = GuardRequest("orh286.stale_route_capability",
                            exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                            &point);
  stale.expected_route_capability_generation = 286;
  stale.observed_route_capability_generation = 285;
  ExpectGuardRefusal(stale, "SB-IRC062-STALE-ROUTE-CAPABILITY");

  auto invalid = GuardRequest("orh286.invalid_hash_dml_route",
                              exec::IndexPlanShapeRequiredPath::indexed_point_lookup,
                              &point);
  invalid.invalid_route_family_use = true;
  invalid.invalid_route_family_detail =
      hash_for_dml->route_diagnostic_code + ":" + hash_for_dml->route_detail;
  ExpectGuardRefusal(invalid, "SB-IRC062-INVALID-ROUTE-FAMILY-USE");
}

}  // namespace

int main() {
  PositiveRoutes();
  NegativeRegressions();
  InvalidRouteFamilyAndStaleCapability();
  std::cout << "ORH-286 plan-shape regression gate passed\n";
  return EXIT_SUCCESS;
}
