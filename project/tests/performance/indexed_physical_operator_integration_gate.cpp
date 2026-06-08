// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "indexed_physical_operator.hpp"
#include "index_key_encoding.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "indexed_physical_operator_integration_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
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

std::string UuidText(platform::UuidKind kind,
                     platform::u64 millis,
                     platform::byte suffix) {
  return uuid::UuidToString(GeneratedUuid(kind, millis, suffix).value);
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
  const auto parsed =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           index_uuid);
  Require(parsed.ok(), "index uuid parse failed");
  auto initialized = page::InitializeIndexBtreePhysicalTree(parsed.value, 768);
  Require(initialized.ok(), "physical btree init failed");
  return std::move(initialized.tree);
}

page::IndexBtreeCell Cell(const std::string& index_uuid,
                          const std::string& key,
                          const std::string& row_uuid,
                          platform::byte version_suffix) {
  page::IndexBtreeCell cell;
  cell.key_ordinal = 0;
  cell.encoded_key = EncodedKey(index_uuid, key);
  const auto parsed_row =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::row,
                                           row_uuid);
  Require(parsed_row.ok(), "row uuid parse failed");
  cell.row_uuid = parsed_row.value;
  cell.version_uuid = GeneratedUuid(platform::UuidKind::row,
                                    1700100000000ull + version_suffix,
                                    version_suffix);
  return cell;
}

void InsertCell(page::IndexBtreePhysicalTree* tree,
                const page::IndexBtreeCell& cell) {
  page::IndexBtreePhysicalInsertRequest request;
  request.cell = cell;
  auto inserted = page::InsertIndexBtreeCell(tree, request);
  Require(inserted.ok(), "physical insert failed");
}

bool HasEvidence(
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

void RequireCommonPhysicalEvidence(const exec::IndexedPhysicalOperatorResult& result,
                                   std::string_view operator_name) {
  Require(result.ok, "operator unexpectedly refused");
  Require(result.runtime_route_capability, "runtime route capability was not true");
  Require(!result.benchmark_clean, "family benchmark-clean was claimed");
  Require(!result.table_scan_consumed, "table scan was consumed");
  Require(HasEvidence(result.evidence,
                      "indexed_physical_operator",
                      operator_name),
          "operator name evidence missing");
  Require(HasEvidence(result.evidence,
                      "indexed_physical_operator_physical_scan_consumed",
                      "index_btree_physical_tree"),
          "physical scan consumption evidence missing");
  Require(HasEvidence(result.evidence,
                      "indexed_physical_operator_no_table_scan",
                      "physical_index_scan_output_consumed"),
          "no table scan evidence missing");
  Require(HasEvidence(result.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "MGA finality authority evidence missing");
  Require(HasEvidence(result.evidence, "parser_or_donor_authority", "false"),
          "parser/donor non-authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "index_or_cache_finality_authority",
                      "false"),
          "index/cache non-authority evidence missing");
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
      UuidText(platform::UuidKind::object, 1700101000000ull, 0x41);
  std::string row_alpha =
      UuidText(platform::UuidKind::row, 1700102000000ull, 0x51);
  std::string row_bravo =
      UuidText(platform::UuidKind::row, 1700102001000ull, 0x52);
  std::string row_charlie =
      UuidText(platform::UuidKind::row, 1700102002000ull, 0x53);
  std::string row_delta =
      UuidText(platform::UuidKind::row, 1700102003000ull, 0x54);
  page::IndexBtreePhysicalTree tree = MakeTree(index_uuid);

  Fixture() {
    InsertCell(&tree, Cell(index_uuid, "alpha", row_alpha, 0x61));
    InsertCell(&tree, Cell(index_uuid, "bravo", row_bravo, 0x62));
    InsertCell(&tree, Cell(index_uuid, "charlie", row_charlie, 0x63));
    InsertCell(&tree, Cell(index_uuid, "delta", row_delta, 0x64));
  }
};

void TestPointRangeAndOrderedLimit() {
  Fixture fixture;

  auto point = BaseRequest(exec::IndexedPhysicalOperatorKind::point_lookup,
                           &fixture.tree);
  point.encoded_point_key = EncodedKey(fixture.index_uuid, "bravo");
  auto result = exec::ExecuteIndexedPhysicalOperator(point);
  RequireCommonPhysicalEvidence(result, "point_lookup");
  Require(result.locators.size() == 1 &&
              result.locators.front().row_uuid == fixture.row_bravo,
          "point lookup did not return physical locator");

  auto range = BaseRequest(exec::IndexedPhysicalOperatorKind::range_scan,
                           &fixture.tree);
  range.lower_bound = Bound(fixture.index_uuid, "bravo");
  range.upper_bound = Bound(fixture.index_uuid, "delta", false);
  result = exec::ExecuteIndexedPhysicalOperator(range);
  RequireCommonPhysicalEvidence(result, "range_scan");
  Require(result.locators.size() == 2, "range scan locator count mismatch");
  Require(result.locators[0].row_uuid == fixture.row_bravo &&
              result.locators[1].row_uuid == fixture.row_charlie,
          "range scan did not preserve index order");

  auto limit = BaseRequest(exec::IndexedPhysicalOperatorKind::ordered_limit,
                           &fixture.tree);
  limit.limit = 2;
  result = exec::ExecuteIndexedPhysicalOperator(limit);
  RequireCommonPhysicalEvidence(result, "ordered_limit");
  Require(result.locators.size() == 2, "ordered LIMIT did not stop at bound");
  Require(result.locators[0].row_uuid == fixture.row_alpha &&
              result.locators[1].row_uuid == fixture.row_bravo,
          "ordered LIMIT did not consume ordered index output");
  Require(HasEvidence(result.evidence, "ordered_limit_stopped_at_bound", "true"),
          "ordered LIMIT bound evidence missing");
}

void TestIndexedNestedLoopAndRuntimeFilter() {
  Fixture fixture;

  auto nested = BaseRequest(
      exec::IndexedPhysicalOperatorKind::indexed_nested_loop,
      &fixture.tree);
  exec::IndexedPhysicalOuterProbe probe1;
  probe1.outer_ordinal = 7;
  probe1.encoded_key = EncodedKey(fixture.index_uuid, "alpha");
  nested.outer_probes.push_back(probe1);
  exec::IndexedPhysicalOuterProbe probe2;
  probe2.outer_ordinal = 8;
  probe2.range_probe = true;
  probe2.lower_bound = Bound(fixture.index_uuid, "charlie");
  probe2.upper_bound = Bound(fixture.index_uuid, "delta");
  nested.outer_probes.push_back(probe2);
  auto result = exec::ExecuteIndexedPhysicalOperator(nested);
  RequireCommonPhysicalEvidence(result, "indexed_nested_loop");
  Require(result.locators.size() == 3, "nested-loop locator count mismatch");
  Require(result.locators[0].outer_ordinal == 7 &&
              result.locators[1].outer_ordinal == 8,
          "nested-loop outer ordinal evidence not preserved on locators");
  Require(HasEvidence(result.evidence,
                      "indexed_nested_loop_outer_ordinal",
                      "7"),
          "nested-loop outer ordinal evidence missing");

  auto filter = BaseRequest(exec::IndexedPhysicalOperatorKind::runtime_filter,
                            &fixture.tree);
  filter.runtime_filter_keys.push_back(EncodedKey(fixture.index_uuid, "bravo"));
  filter.runtime_filter_keys.push_back(EncodedKey(fixture.index_uuid, "delta"));
  result = exec::ExecuteIndexedPhysicalOperator(filter);
  RequireCommonPhysicalEvidence(result, "runtime_filter");
  Require(result.locators.size() == 2, "runtime filter locator count mismatch");
  Require(HasEvidence(result.evidence,
                      "runtime_filter_exact_recheck",
                      "mga_visibility_and_security_required_per_candidate"),
          "runtime filter exact recheck evidence missing");
}

void TestMergeOrderedInput() {
  Fixture left;
  const std::string right_index_uuid =
      UuidText(platform::UuidKind::object, 1700103000000ull, 0x71);
  auto right = MakeTree(right_index_uuid);
  InsertCell(&right, Cell(right_index_uuid, "alpha",
                         UuidText(platform::UuidKind::row,
                                  1700104000000ull,
                                  0x81),
                         0x91));
  InsertCell(&right, Cell(right_index_uuid, "charlie",
                         UuidText(platform::UuidKind::row,
                                  1700104001000ull,
                                  0x82),
                         0x92));
  InsertCell(&right, Cell(right_index_uuid, "echo",
                         UuidText(platform::UuidKind::row,
                                  1700104002000ull,
                                  0x83),
                         0x93));

  auto merge = BaseRequest(exec::IndexedPhysicalOperatorKind::merge_ordered_input,
                           &left.tree);
  merge.right_physical_tree = &right;
  const auto result = exec::ExecuteIndexedPhysicalOperator(merge);
  RequireCommonPhysicalEvidence(result, "merge_ordered_input");
  Require(result.merge_pairs.size() == 2, "merge ordered pair count mismatch");
  Require(HasEvidence(result.evidence, "merge_ordered_input_pair", "0:0"),
          "merge ordered first pair evidence missing");
  Require(HasEvidence(result.evidence, "merge_ordered_input_advance", "left:1"),
          "merge ordered advance evidence missing");
  Require(HasEvidence(result.evidence,
                      "merge_ordered_left_stream_evidence",
                      "indexed_physical_operator_scan_kind=merge_ordered_left"),
          "merge ordered left stream evidence missing");
  Require(HasEvidence(result.evidence,
                      "merge_ordered_right_stream_evidence",
                      "indexed_physical_operator_scan_kind=merge_ordered_right"),
          "merge ordered right stream evidence missing");
}

void TestFailClosedDiagnostics() {
  Fixture fixture;

  auto missing_tree = BaseRequest(exec::IndexedPhysicalOperatorKind::point_lookup,
                                 nullptr);
  auto result = exec::ExecuteIndexedPhysicalOperator(missing_tree);
  Require(!result.ok &&
              result.diagnostic_detail == "physical_index_tree_required",
          "missing physical tree did not fail closed");

  auto stale = BaseRequest(exec::IndexedPhysicalOperatorKind::point_lookup,
                           &fixture.tree);
  stale.plan_safe = false;
  result = exec::ExecuteIndexedPhysicalOperator(stale);
  Require(!result.ok && result.diagnostic_detail == "stale_or_unsafe_plan",
          "stale plan did not fail closed");

  auto missing_key = BaseRequest(exec::IndexedPhysicalOperatorKind::point_lookup,
                                &fixture.tree);
  result = exec::ExecuteIndexedPhysicalOperator(missing_key);
  Require(!result.ok && result.diagnostic_detail == "encoded_point_key_required",
          "missing encoded key did not fail closed");

  auto nested_missing_key_proof = BaseRequest(
      exec::IndexedPhysicalOperatorKind::indexed_nested_loop,
      &fixture.tree);
  exec::IndexedPhysicalOuterProbe point_probe;
  point_probe.outer_ordinal = 1;
  point_probe.encoded_key = EncodedKey(fixture.index_uuid, "alpha");
  nested_missing_key_proof.outer_probes.push_back(point_probe);
  nested_missing_key_proof.encoded_key_proof = false;
  result = exec::ExecuteIndexedPhysicalOperator(nested_missing_key_proof);
  Require(!result.ok &&
              result.diagnostic_detail == "nested_loop_encoded_point_key_required",
          "nested-loop missing point key proof did not fail closed");

  auto missing_bounds = BaseRequest(exec::IndexedPhysicalOperatorKind::range_scan,
                                   &fixture.tree);
  missing_bounds.lower_bound.unbounded = false;
  result = exec::ExecuteIndexedPhysicalOperator(missing_bounds);
  Require(!result.ok &&
              result.diagnostic_detail == "encoded_range_bounds_required",
          "missing encoded bounds did not fail closed");

  auto nested_missing_bounds_proof = BaseRequest(
      exec::IndexedPhysicalOperatorKind::indexed_nested_loop,
      &fixture.tree);
  exec::IndexedPhysicalOuterProbe range_probe;
  range_probe.outer_ordinal = 2;
  range_probe.range_probe = true;
  range_probe.lower_bound = Bound(fixture.index_uuid, "alpha");
  range_probe.upper_bound = Bound(fixture.index_uuid, "bravo");
  nested_missing_bounds_proof.outer_probes.push_back(range_probe);
  nested_missing_bounds_proof.encoded_bounds_proof = false;
  result = exec::ExecuteIndexedPhysicalOperator(nested_missing_bounds_proof);
  Require(!result.ok &&
              result.diagnostic_detail == "nested_loop_encoded_range_bounds_required",
          "nested-loop missing range bound proof did not fail closed");

  auto missing_mga = BaseRequest(exec::IndexedPhysicalOperatorKind::ordered_limit,
                                &fixture.tree);
  missing_mga.limit = 1;
  missing_mga.durable_mga_inventory_proof = false;
  result = exec::ExecuteIndexedPhysicalOperator(missing_mga);
  Require(!result.ok &&
              result.diagnostic_detail == "durable_mga_inventory_proof_required",
          "missing durable MGA proof did not fail closed");

  auto missing_security = BaseRequest(
      exec::IndexedPhysicalOperatorKind::runtime_filter,
      &fixture.tree);
  missing_security.security_recheck_planned = false;
  result = exec::ExecuteIndexedPhysicalOperator(missing_security);
  Require(!result.ok && result.diagnostic_detail == "security_recheck_required",
          "missing security proof did not fail closed");

  auto runtime_filter_missing_key_proof = BaseRequest(
      exec::IndexedPhysicalOperatorKind::runtime_filter,
      &fixture.tree);
  runtime_filter_missing_key_proof.runtime_filter_keys.push_back(
      EncodedKey(fixture.index_uuid, "alpha"));
  runtime_filter_missing_key_proof.encoded_key_proof = false;
  result = exec::ExecuteIndexedPhysicalOperator(runtime_filter_missing_key_proof);
  Require(!result.ok &&
              result.diagnostic_detail == "runtime_filter_encoded_key_proof_required",
          "runtime filter missing encoded key proof did not fail closed");

  auto unsafe_tree = fixture.tree;
  unsafe_tree.root_page_number = 999999;
  auto unsafe = BaseRequest(exec::IndexedPhysicalOperatorKind::ordered_limit,
                           &unsafe_tree);
  unsafe.limit = 1;
  result = exec::ExecuteIndexedPhysicalOperator(unsafe);
  Require(!result.ok &&
              result.diagnostic_detail == "unsafe_physical_index_tree",
          "unsafe physical tree did not fail closed");
}

}  // namespace

int main() {
  TestPointRangeAndOrderedLimit();
  TestIndexedNestedLoopAndRuntimeFilter();
  TestMergeOrderedInput();
  TestFailClosedDiagnostics();
  std::cout << "indexed_physical_operator_integration_gate=passed\n";
  return EXIT_SUCCESS;
}
