// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_row_locator_stream.hpp"
#include "index_key_encoding.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "dml_conflict_merge_locator_stream_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::u64 NowMillis() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind) {
  static platform::u64 counter = 0;
  const auto generated = uuid::GenerateEngineIdentityV7(kind,
                                                        NowMillis() + (++counter * 17));
  Require(generated.ok(), "runtime uuidv7 generation failed");
  return generated.value;
}

std::string UuidText(platform::UuidKind kind) {
  return uuid::UuidToString(GeneratedUuid(kind).value);
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

page::IndexBtreePhysicalTree MakeTree(const std::string& index_uuid) {
  const auto parsed =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object,
                                           index_uuid);
  Require(parsed.ok(), "index uuid parse failed");
  auto initialized = page::InitializeIndexBtreePhysicalTree(parsed.value, 4096);
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
  const auto parsed_row =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::row,
                                           row_uuid);
  const auto parsed_version =
      uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::row,
                                           version_uuid);
  Require(parsed_row.ok() && parsed_version.ok(), "row/version uuid parse failed");
  cell.row_uuid = parsed_row.value;
  cell.version_uuid = parsed_version.value;
  return cell;
}

void InsertCell(page::IndexBtreePhysicalTree* tree,
                const page::IndexBtreeCell& cell,
                bool unique = false) {
  if (unique) {
    page::IndexBtreePhysicalUniqueInsertRequest request;
    request.cell = cell;
    request.active_duplicate_policy =
        page::IndexBtreePhysicalUniqueActiveDuplicatePolicy::refuse_candidate;
    auto inserted = page::InsertUniqueIndexBtreeCell(tree, request);
    Require(inserted.ok() && !inserted.conflict, "unique physical insert failed");
    return;
  }
  page::IndexBtreePhysicalInsertRequest request;
  request.cell = cell;
  auto inserted = page::InsertIndexBtreeCell(tree, request);
  Require(inserted.ok(), "physical insert failed");
}

api::DmlTargetAccessPlanRequest BasePlanRequest(const std::string& relation_uuid) {
  api::DmlTargetAccessPlanRequest request;
  request.mutation_kind = "irc_052_locator_stream";
  request.relation_uuid = relation_uuid;
  request.access_descriptor_present = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  request.grants_proven = true;
  request.security_context_present = true;
  request.observed_catalog_epoch = 7;
  request.current_catalog_epoch = 7;
  request.observed_security_epoch = 7;
  request.current_security_epoch = 7;
  request.observed_policy_epoch = 7;
  request.current_policy_epoch = 7;
  request.observed_stats_epoch = 7;
  request.current_stats_epoch = 7;
  request.local_transaction_id = 52;
  return request;
}

api::DmlRowLocatorStreamRequest BaseStreamRequest(
    api::DmlRowLocatorStreamConsumer consumer,
    api::DmlTargetAccessPlan plan) {
  api::DmlRowLocatorStreamRequest request;
  request.consumer = consumer;
  request.access_plan = std::move(plan);
  request.access_plan_engine_authority_proof = true;
  request.durable_mga_inventory_proof = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  request.parser_or_donor_authority = false;
  request.index_or_cache_finality_authority = false;
  return request;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const auto& item) {
                       return item.evidence_kind == kind &&
                              item.evidence_id.find(id) != std::string::npos;
                     });
}

api::DmlTargetAccessPlan RowUuidPlan(const std::string& relation_uuid,
                                     const std::string& row_uuid) {
  auto request = BasePlanRequest(relation_uuid);
  request.predicate_kind = "row_uuid_eq";
  request.row_uuid = row_uuid;
  auto plan = api::BuildDmlTargetAccessPlan(request);
  Require(plan.ok, "row uuid access plan failed");
  return plan;
}

api::DmlTargetAccessPlan RowUuidListPlan(
    const std::string& relation_uuid,
    const std::vector<std::string>& row_uuids) {
  auto request = BasePlanRequest(relation_uuid);
  request.predicate_kind = "row_uuid_in_list";
  request.row_uuids = row_uuids;
  auto plan = api::BuildDmlTargetAccessPlan(request);
  Require(plan.ok, "row uuid list access plan failed");
  return plan;
}

api::DmlTargetAccessPlan IndexPlan(const std::string& relation_uuid,
                                   const std::string& index_uuid,
                                   std::string predicate_kind,
                                   bool unique) {
  auto request = BasePlanRequest(relation_uuid);
  request.predicate_kind = std::move(predicate_kind);
  request.predicate_descriptor_digest = "encoded-key-digest";
  request.index_uuid = index_uuid;
  request.index_family = "btree";
  request.index_unique = unique;
  auto plan = api::BuildDmlTargetAccessPlan(request);
  Require(plan.ok, "index access plan failed");
  return plan;
}

void TestExplicitRowUuidStreams() {
  const std::string relation_uuid = UuidText(platform::UuidKind::object);
  const std::string row1 = UuidText(platform::UuidKind::row);
  const std::string row2 = UuidText(platform::UuidKind::row);

  auto singleton = BaseStreamRequest(api::DmlRowLocatorStreamConsumer::update,
                                     RowUuidPlan(relation_uuid, row1));
  auto result = api::BuildDmlRowLocatorStream(singleton);
  Require(result.ok, "row uuid singleton stream refused");
  Require(result.source == api::DmlRowLocatorStreamSource::row_uuid_singleton,
          "row uuid singleton source mismatch");
  Require(result.locators.size() == 1 && result.locators.front().row_uuid == row1,
          "row uuid singleton locator mismatch");
  Require(HasEvidence(result.evidence,
                      "dml_row_locator_stream_no_table_scan",
                      "explicit_row_uuid_locator_stream_consumed"),
          "row uuid no-table-scan evidence missing");

  auto list = BaseStreamRequest(
      api::DmlRowLocatorStreamConsumer::delete_row,
      RowUuidListPlan(relation_uuid, {row1, row2}));
  result = api::BuildDmlRowLocatorStream(list);
  Require(result.ok, "row uuid list stream refused");
  Require(result.source == api::DmlRowLocatorStreamSource::row_uuid_list,
          "row uuid list source mismatch");
  Require(result.locators.size() == 2, "row uuid list locator count mismatch");
}

void TestOnConflictConsumesUniquePhysicalLocatorStream() {
  const std::string relation_uuid = UuidText(platform::UuidKind::object);
  const std::string index_uuid = UuidText(platform::UuidKind::object);
  const std::string row_uuid = UuidText(platform::UuidKind::row);
  auto tree = MakeTree(index_uuid);
  InsertCell(&tree,
             Cell(index_uuid,
                  "conflict-key",
                  row_uuid,
                  UuidText(platform::UuidKind::row)),
             true);

  auto request = BaseStreamRequest(
      api::DmlRowLocatorStreamConsumer::on_conflict,
      IndexPlan(relation_uuid, index_uuid, "unique_eq", true));
  request.index_unique = true;
  request.applicable_physical_index_exists = true;
  request.physical_tree = &tree;
  request.encoded_point_key = EncodedKey(index_uuid, "conflict-key");

  const auto result = api::BuildDmlRowLocatorStream(request);
  Require(result.ok, "ON CONFLICT unique locator stream refused");
  Require(result.source ==
              api::DmlRowLocatorStreamSource::physical_unique_btree_point,
          "ON CONFLICT did not use unique physical point stream");
  Require(result.locators.size() == 1 && result.locators.front().row_uuid == row_uuid,
          "ON CONFLICT locator mismatch");
  Require(HasEvidence(result.evidence,
                      "on_conflict_unique_locator_stream",
                      "consumed_no_table_scan"),
          "ON CONFLICT unique locator evidence missing");
  Require(HasEvidence(result.evidence,
                      "dml_row_locator_stream_no_table_scan",
                      "physical_index_locator_stream_consumed"),
          "ON CONFLICT no-table-scan evidence missing");
  Require(HasEvidence(result.evidence, "runtime_route_capability", "false"),
          "runtime route non-claim missing");
  Require(HasEvidence(result.evidence, "index_benchmark_clean", "false"),
          "benchmark-clean non-claim missing");
}

void TestMergeRowUuidAndIndexRangeOrdinalEvidence() {
  const std::string relation_uuid = UuidText(platform::UuidKind::object);
  const std::string row1 = UuidText(platform::UuidKind::row);
  const std::string row2 = UuidText(platform::UuidKind::row);

  auto row_request = BaseStreamRequest(
      api::DmlRowLocatorStreamConsumer::merge,
      RowUuidListPlan(relation_uuid, {row1, row2}));
  row_request.merge_ordinals.push_back({0, 0, true});
  row_request.merge_ordinals.push_back({1, 1, false});
  auto result = api::BuildDmlRowLocatorStream(row_request);
  Require(result.ok, "MERGE row uuid locator stream refused");
  Require(HasEvidence(result.evidence,
                      "merge_locator_stream_source_action_order",
                      "0:0:matched"),
          "MERGE matched row uuid ordinal evidence missing");
  Require(HasEvidence(result.evidence,
                      "merge_locator_stream_source_action_order",
                      "1:1:unmatched"),
          "MERGE unmatched row uuid ordinal evidence missing");

  const std::string index_uuid = UuidText(platform::UuidKind::object);
  auto tree = MakeTree(index_uuid);
  const std::string range_row1 = UuidText(platform::UuidKind::row);
  const std::string range_row2 = UuidText(platform::UuidKind::row);
  InsertCell(&tree,
             Cell(index_uuid, "bravo", range_row1, UuidText(platform::UuidKind::row)));
  InsertCell(&tree,
             Cell(index_uuid, "charlie", range_row2, UuidText(platform::UuidKind::row)));

  auto range_request = BaseStreamRequest(
      api::DmlRowLocatorStreamConsumer::merge,
      IndexPlan(relation_uuid, index_uuid, "index_range", false));
  range_request.applicable_physical_index_exists = true;
  range_request.physical_tree = &tree;
  range_request.lower_bound.unbounded = false;
  range_request.lower_bound.inclusive = true;
  range_request.lower_bound.encoded_key = EncodedKey(index_uuid, "bravo");
  range_request.upper_bound.unbounded = false;
  range_request.upper_bound.inclusive = true;
  range_request.upper_bound.encoded_key = EncodedKey(index_uuid, "charlie");
  range_request.merge_ordinals.push_back({2, 0, true});
  result = api::BuildDmlRowLocatorStream(range_request);
  Require(result.ok, "MERGE index range locator stream refused");
  Require(result.source == api::DmlRowLocatorStreamSource::physical_btree_range,
          "MERGE did not use physical range stream");
  Require(result.locators.size() == 2, "MERGE range locator count mismatch");
  Require(HasEvidence(result.evidence,
                      "merge_locator_stream_source_action_order",
                      "2:0:matched"),
          "MERGE index range ordinal evidence missing");
}

void TestUpdateDeleteFailClosedAndExactFallback() {
  const std::string relation_uuid = UuidText(platform::UuidKind::object);
  const std::string index_uuid = UuidText(platform::UuidKind::object);
  auto tree = MakeTree(index_uuid);
  const std::string update_row = UuidText(platform::UuidKind::row);
  const std::string delete_row = UuidText(platform::UuidKind::row);
  InsertCell(&tree,
             Cell(index_uuid,
                  "update-key",
                  update_row,
                  UuidText(platform::UuidKind::row)));
  InsertCell(&tree,
             Cell(index_uuid,
                  "delete-key",
                  delete_row,
                  UuidText(platform::UuidKind::row)));

  auto indexed_update = BaseStreamRequest(
      api::DmlRowLocatorStreamConsumer::update,
      IndexPlan(relation_uuid, index_uuid, "scalar_eq", false));
  indexed_update.applicable_physical_index_exists = true;
  indexed_update.physical_tree = &tree;
  indexed_update.encoded_point_key = EncodedKey(index_uuid, "update-key");
  auto result = api::BuildDmlRowLocatorStream(indexed_update);
  Require(result.ok &&
              result.source == api::DmlRowLocatorStreamSource::physical_btree_point,
          "UPDATE did not consume index-backed point locator stream");
  Require(result.locators.size() == 1 &&
              result.locators.front().row_uuid == update_row,
          "UPDATE index locator mismatch");

  auto indexed_delete = BaseStreamRequest(
      api::DmlRowLocatorStreamConsumer::delete_row,
      IndexPlan(relation_uuid, index_uuid, "scalar_eq", false));
  indexed_delete.applicable_physical_index_exists = true;
  indexed_delete.physical_tree = &tree;
  indexed_delete.encoded_point_key = EncodedKey(index_uuid, "delete-key");
  result = api::BuildDmlRowLocatorStream(indexed_delete);
  Require(result.ok &&
              result.source == api::DmlRowLocatorStreamSource::physical_btree_point,
          "DELETE did not consume index-backed point locator stream");
  Require(result.locators.size() == 1 &&
              result.locators.front().row_uuid == delete_row,
          "DELETE index locator mismatch");

  auto stale_request = BasePlanRequest(relation_uuid);
  stale_request.predicate_kind = "unique_eq";
  stale_request.index_uuid = index_uuid;
  stale_request.index_unique = true;
  stale_request.observed_catalog_epoch = 1;
  stale_request.current_catalog_epoch = 2;
  auto stale_plan = api::BuildDmlTargetAccessPlan(stale_request);
  Require(!stale_plan.ok, "stale access plan was not refused");

  auto stream = BaseStreamRequest(api::DmlRowLocatorStreamConsumer::update,
                                  stale_plan);
  stream.index_unique = true;
  stream.applicable_physical_index_exists = true;
  result = api::BuildDmlRowLocatorStream(stream);
  Require(!result.ok, "stale update access plan did not fail closed");
  Require(HasEvidence(result.evidence,
                      "dml_row_locator_stream_refusal",
                      "access_plan_not_safe"),
          "stale access plan refusal evidence missing");

  auto safe_plan = IndexPlan(relation_uuid, index_uuid, "unique_eq", true);
  stream = BaseStreamRequest(api::DmlRowLocatorStreamConsumer::delete_row,
                             safe_plan);
  stream.durable_mga_inventory_proof = false;
  result = api::BuildDmlRowLocatorStream(stream);
  Require(!result.ok, "delete without durable MGA proof did not fail closed");
  Require(HasEvidence(result.evidence,
                      "dml_row_locator_stream_refusal",
                      "durable_mga_inventory_proof_required"),
          "durable MGA proof refusal evidence missing");

  auto fallback_plan_request = BasePlanRequest(relation_uuid);
  fallback_plan_request.explicit_table_scan_fallback = true;
  fallback_plan_request.predicate_kind = "residual_filter";
  auto fallback_plan = api::BuildDmlTargetAccessPlan(fallback_plan_request);
  Require(fallback_plan.ok &&
              fallback_plan.access_kind == api::DmlTargetAccessKind::table_scan,
          "table scan fallback access plan failed");
  stream = BaseStreamRequest(api::DmlRowLocatorStreamConsumer::delete_row,
                             fallback_plan);
  stream.table_scan_fallback_allowed = true;
  stream.applicable_physical_index_exists = false;
  result = api::BuildDmlRowLocatorStream(stream);
  Require(result.ok && result.table_scan_fallback,
          "exact no-index table scan fallback was refused");
  Require(HasEvidence(result.evidence,
                      "dml_row_locator_stream_table_scan_fallback",
                      "allowed_no_applicable_row_uuid_or_physical_index_locator"),
          "exact table scan fallback evidence missing");

  stream.applicable_physical_index_exists = true;
  result = api::BuildDmlRowLocatorStream(stream);
  Require(!result.ok, "table scan fallback was allowed despite applicable index");
  Require(HasEvidence(result.evidence,
                      "dml_row_locator_stream_refusal",
                      "table_scan_fallback_refused_applicable_index_exists"),
          "applicable-index table scan refusal evidence missing");
}

void TestIndexedPlanWithoutPhysicalTreeRefusesUnlessExplicitFallback() {
  const std::string relation_uuid = UuidText(platform::UuidKind::object);
  const std::string index_uuid = UuidText(platform::UuidKind::object);

  auto indexed = BaseStreamRequest(
      api::DmlRowLocatorStreamConsumer::update,
      IndexPlan(relation_uuid, index_uuid, "scalar_eq", false));
  indexed.applicable_physical_index_exists = true;
  indexed.physical_tree = nullptr;
  auto result = api::BuildDmlRowLocatorStream(indexed);
  Require(!result.ok, "indexed locator stream without physical tree was admitted");
  Require(HasEvidence(result.evidence,
                      "dml_row_locator_stream_refusal",
                      "physical_index_tree_required"),
          "missing physical tree refusal evidence missing");

  auto fallback_plan_request = BasePlanRequest(relation_uuid);
  fallback_plan_request.explicit_table_scan_fallback = true;
  fallback_plan_request.predicate_kind = "residual_filter";
  auto fallback_plan = api::BuildDmlTargetAccessPlan(fallback_plan_request);
  Require(fallback_plan.ok &&
              fallback_plan.access_kind == api::DmlTargetAccessKind::table_scan,
          "fallback plan for explicit no-index route failed");
  auto fallback = BaseStreamRequest(api::DmlRowLocatorStreamConsumer::update,
                                    fallback_plan);
  fallback.table_scan_fallback_allowed = true;
  fallback.applicable_physical_index_exists = false;
  result = api::BuildDmlRowLocatorStream(fallback);
  Require(result.ok && result.table_scan_fallback,
          "explicit table scan fallback without applicable locator refused");
  Require(HasEvidence(result.evidence,
                      "dml_row_locator_stream_table_scan_fallback",
                      "allowed_no_applicable_row_uuid_or_physical_index_locator"),
          "explicit no-applicable-locator fallback evidence missing");
}

}  // namespace

int main() {
  TestExplicitRowUuidStreams();
  TestOnConflictConsumesUniquePhysicalLocatorStream();
  TestMergeRowUuidAndIndexRangeOrdinalEvidence();
  TestUpdateDeleteFailClosedAndExactFallback();
  TestIndexedPlanWithoutPhysicalTreeRefusesUnlessExplicitFallback();
  return EXIT_SUCCESS;
}
