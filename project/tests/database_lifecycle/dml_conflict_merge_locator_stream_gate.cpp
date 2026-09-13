// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_row_locator_stream.hpp"
#include "index_key_encoding.hpp"
#include "hot_point_lookup_cache.hpp"
#include "uuid.hpp"
#include "../common/single_tu_allocation_fault.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
unsigned checks = 0;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "dml_conflict_merge_locator_stream_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  ++checks;
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

api::EngineUuid BinaryId(platform::UuidKind kind) {
  return GeneratedUuid(kind).value;
}

std::vector<platform::byte> EncodedKey(const api::EngineUuid& index_uuid,
                                       const std::string& key) {
  const platform::TypedUuid descriptor_uuid{platform::UuidKind::object, index_uuid};
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = descriptor_uuid;
  component.payload.assign(key.begin(), key.end());
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "test key encoding failed");
  return encoded.encoded;
}

page::IndexBtreePhysicalTree MakeTree(const api::EngineUuid& index_uuid) {
  auto initialized = page::InitializeIndexBtreePhysicalTree(
      {platform::UuidKind::object, index_uuid}, 4096);
  Require(initialized.ok(), "physical btree init failed");
  return std::move(initialized.tree);
}

page::IndexBtreeCell Cell(const api::EngineUuid& index_uuid,
                          const std::string& key,
                          const api::EngineUuid& row_uuid,
                          const api::EngineUuid& version_uuid) {
  page::IndexBtreeCell cell;
  cell.key_ordinal = 0;
  cell.encoded_key = EncodedKey(index_uuid, key);
  cell.row_uuid = {platform::UuidKind::row, row_uuid};
  cell.version_uuid = {platform::UuidKind::row, version_uuid};
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

api::DmlTargetAccessPlanRequest BasePlanRequest(const api::EngineUuid& relation_uuid) {
  api::DmlTargetAccessPlanRequest request;
  request.mutation_kind = "irc_052_locator_stream";
  static const auto node = BinaryId(platform::UuidKind::database);
  request.database_uuid = node;
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
  request.parser_or_reference_authority = false;
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

api::DmlTargetAccessPlan RowUuidPlan(const api::EngineUuid& relation_uuid,
                                     const api::EngineUuid& row_uuid) {
  auto request = BasePlanRequest(relation_uuid);
  request.predicate_kind = "row_uuid_eq";
  request.row_uuid = row_uuid;
  auto plan = api::BuildDmlTargetAccessPlan(request);
  Require(plan.ok, "row uuid access plan failed");
  return plan;
}

api::DmlTargetAccessPlan RowUuidListPlan(
    const api::EngineUuid& relation_uuid,
    const std::vector<api::EngineUuid>& row_uuids) {
  auto request = BasePlanRequest(relation_uuid);
  request.predicate_kind = "row_uuid_in_list";
  request.row_uuids = row_uuids;
  auto plan = api::BuildDmlTargetAccessPlan(request);
  Require(plan.ok, "row uuid list access plan failed");
  return plan;
}

api::DmlTargetAccessPlan IndexPlan(const api::EngineUuid& relation_uuid,
                                   const api::EngineUuid& index_uuid,
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
  const auto relation_uuid = BinaryId(platform::UuidKind::object);
  const auto row1 = BinaryId(platform::UuidKind::row);
  const auto row2 = BinaryId(platform::UuidKind::row);

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
  const auto relation_uuid = BinaryId(platform::UuidKind::object);
  const auto index_uuid = BinaryId(platform::UuidKind::object);
  const auto row_uuid = BinaryId(platform::UuidKind::row);
  auto tree = MakeTree(index_uuid);
  InsertCell(&tree,
             Cell(index_uuid,
                  "conflict-key",
                  row_uuid,
                  BinaryId(platform::UuidKind::row)),
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
  const auto relation_uuid = BinaryId(platform::UuidKind::object);
  const auto row1 = BinaryId(platform::UuidKind::row);
  const auto row2 = BinaryId(platform::UuidKind::row);

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

  const auto index_uuid = BinaryId(platform::UuidKind::object);
  auto tree = MakeTree(index_uuid);
  const auto range_row1 = BinaryId(platform::UuidKind::row);
  const auto range_row2 = BinaryId(platform::UuidKind::row);
  InsertCell(&tree,
             Cell(index_uuid, "bravo", range_row1, BinaryId(platform::UuidKind::row)));
  InsertCell(&tree,
             Cell(index_uuid, "charlie", range_row2, BinaryId(platform::UuidKind::row)));

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
  const auto relation_uuid = BinaryId(platform::UuidKind::object);
  const auto index_uuid = BinaryId(platform::UuidKind::object);
  auto tree = MakeTree(index_uuid);
  const auto update_row = BinaryId(platform::UuidKind::row);
  const auto delete_row = BinaryId(platform::UuidKind::row);
  InsertCell(&tree,
             Cell(index_uuid,
                  "update-key",
                  update_row,
                  BinaryId(platform::UuidKind::row)));
  InsertCell(&tree,
             Cell(index_uuid,
                  "delete-key",
                  delete_row,
                  BinaryId(platform::UuidKind::row)));

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
  const auto relation_uuid = BinaryId(platform::UuidKind::object);
  const auto index_uuid = BinaryId(platform::UuidKind::object);

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

void TestBinaryIdentityAdmissionAndActualCacheLocator() {
  auto request = BasePlanRequest(BinaryId(platform::UuidKind::object));
  request.predicate_kind = "row_uuid_eq";
  request.row_uuid = BinaryId(platform::UuidKind::row);
  const auto refused = [](const api::DmlTargetAccessPlanRequest& bad) {
    const auto plan = api::BuildDmlTargetAccessPlan(bad);
    Require(!plan.ok && plan.database_uuid.is_nil() && plan.relation_uuid.is_nil() &&
        plan.row_uuid.is_nil() && plan.index_uuid.is_nil() && plan.row_uuids.empty(),
        "invalid identity published an executable target plan");
  };
  for (auto member : {&api::DmlTargetAccessPlanRequest::database_uuid,
      &api::DmlTargetAccessPlanRequest::relation_uuid,
      &api::DmlTargetAccessPlanRequest::row_uuid,
      &api::DmlTargetAccessPlanRequest::index_uuid}) {
    auto valid = request;
    if (member == &api::DmlTargetAccessPlanRequest::index_uuid) {
      valid.row_uuid = {}; valid.index_uuid = BinaryId(platform::UuidKind::object);
      valid.predicate_kind = "unique_eq"; valid.index_unique = true;
    }
    Require(api::BuildDmlTargetAccessPlan(valid).ok, "identity matrix baseline refused");
    for (unsigned version = 0; version < 16; ++version) if (version != 7) {
      auto bad = valid; (bad.*member).bytes[6] = static_cast<platform::byte>(version << 4); refused(bad);
    }
    auto bad = valid; (bad.*member).bytes[8] = 0xc0; refused(bad);
  }
  auto bad = request; bad.row_uuid = {}; refused(bad);
  bad = request; bad.row_uuids = {request.row_uuid}; refused(bad);
  auto list = request; list.row_uuid = {}; list.predicate_kind = "row_uuid_in_list";
  auto plan = api::BuildDmlTargetAccessPlan(list);
  Require(plan.ok && plan.access_kind == api::DmlTargetAccessKind::row_uuid_list && plan.estimated_rows == 0,
          "empty target list became a scan or fabricated singleton estimate");
  auto stream = BaseStreamRequest(api::DmlRowLocatorStreamConsumer::delete_row, plan);
  auto result = api::BuildDmlRowLocatorStream(stream);
  Require(result.ok && result.locators.empty() && !result.table_scan_fallback,
          "empty list stream performed a table scan");
  auto pruned_empty = list;
  pruned_empty.summary_prune.requested = true;
  pruned_empty.summary_prune.summary_present = pruned_empty.summary_prune.predicate_supported = true;
  pruned_empty.summary_prune.summary_generation = pruned_empty.summary_prune.relation_generation = 1;
  const auto empty_summary_plan = api::BuildDmlTargetAccessPlan(pruned_empty);
  Require(empty_summary_plan.ok && empty_summary_plan.access_kind == api::DmlTargetAccessKind::row_uuid_list &&
          empty_summary_plan.estimated_rows == 0, "summary pruning overrode an empty target list");
  list.row_uuids = {request.row_uuid, request.row_uuid}; refused(list);
  list.row_uuids = {request.row_uuid, {}}; refused(list);
  // The stream independently validates caller-supplied plan objects.
  stream.access_plan.row_uuids = {request.row_uuid, {}};
  result = api::BuildDmlRowLocatorStream(stream);
  Require(!result.ok && result.locators.empty() && result.diagnostic.code == "CATALOG.INVALID_INPUT",
          "forged plan published a partial list or unknown diagnostic");

  // A requested row is not a successfully observed row.
  request.row_uuid = BinaryId(platform::UuidKind::row);
  for (unsigned repeat = 0; repeat < 2; ++repeat) {
    plan = api::BuildDmlTargetAccessPlan(request);
    Require(plan.ok && std::find(plan.evidence.begin(), plan.evidence.end(),
        "hot_point_lookup_cache_lookup=miss") != plan.evidence.end(),
        "planning admitted an unobserved row into the cache");
  }
  std::vector<std::string> evidence;
  api::AdmitDmlHotPointLookupCacheSuccessfulRowLocator(request, BinaryId(platform::UuidKind::row), &evidence);
  plan = api::BuildDmlTargetAccessPlan(request);
  Require(std::find(plan.evidence.begin(), plan.evidence.end(), "hot_point_lookup_cache_lookup=miss") != plan.evidence.end(),
          "different actual row poisoned singleton cache");
  api::AdmitDmlHotPointLookupCacheSuccessfulRowLocator(request, request.row_uuid, &evidence);
  plan = api::BuildDmlTargetAccessPlan(request);
  Require(std::find(plan.evidence.begin(), plan.evidence.end(), "hot_point_lookup_cache_lookup=hit") != plan.evidence.end(),
          "actual successful binary locator was not admitted");
  const auto display = api::SerializeDmlTargetAccessPlanEvidence(plan);
  Require(display.find(uuid::UuidToString(request.row_uuid)) == std::string::npos &&
          display.find(std::string(reinterpret_cast<const char*>(request.row_uuid.bytes.data()), 16)) == std::string::npos,
          "diagnostic labels exposed a UUID text or raw-key identity");

  const auto index = BinaryId(platform::UuidKind::object);
  const auto row = BinaryId(platform::UuidKind::row), version = BinaryId(platform::UuidKind::row);
  auto tree = MakeTree(index); InsertCell(&tree, Cell(index, "key", row, version));
  stream = BaseStreamRequest(api::DmlRowLocatorStreamConsumer::update,
      IndexPlan(request.relation_uuid, index, "scalar_eq", false));
  stream.physical_tree = &tree; stream.encoded_point_key = EncodedKey(index, "key");
  result = api::BuildDmlRowLocatorStream(stream);
  Require(result.ok && result.locators.size() == 1 && result.locators[0].row_uuid == row &&
      result.locators[0].version_uuid == version && result.locators[0].index_uuid == index,
      "physical index locator did not preserve exact raw identities");
  stream.access_plan.index_uuid = BinaryId(platform::UuidKind::object);
  result = api::BuildDmlRowLocatorStream(stream);
  Require(!result.ok && result.locators.empty(), "physical tree from another index was admitted");
  stream.access_plan.index_uuid = index;
  Require(!tree.pages.empty() && !tree.pages.front().serialized.empty(), "physical corruption fixture missing page bytes");
  tree.pages.front().serialized[0] ^= 1;
  result = api::BuildDmlRowLocatorStream(stream);
  Require(!result.ok && result.locators.empty() && result.diagnostic.native_source.has_value(),
          "physical index corruption lost its native diagnostic or published partial locators");
}

void TestBinaryCacheKeyFramingAndDependencies() {
  idx::HotPointLookupCacheKey key;
  key.database_uuid = GeneratedUuid(platform::UuidKind::database);
  key.object_uuid = GeneratedUuid(platform::UuidKind::object);
  key.encoded_probe_key = std::string("key\0bytes", 9);
  key.statistics_snapshot_id = "stats"; key.descriptor_set_digest = "descriptor";
  key.index_definition_digest = "index"; key.security_policy_digest = "security";
  key.redaction_policy_digest = "redaction"; key.access_policy_digest = "access";
  key.collation_profile_digest = "collation";
  key.catalog_epoch = key.index_epoch = key.statistics_epoch = key.security_epoch =
      key.policy_epoch = key.object_epoch = key.compatibility_epoch = 1;
  const auto bytes = idx::BuildHotPointLookupStableProbeKey(key);
  Require(bytes.size() == 8 + 3 * 17 + 8 + 9 &&
          std::equal(key.database_uuid.value.bytes.begin(), key.database_uuid.value.bytes.end(),
                     reinterpret_cast<const platform::byte*>(bytes.data()) + 9),
          "cache stable key does not carry framed binary identity");
  std::string idx::HotPointLookupCacheKey::* const members[] = {
    &idx::HotPointLookupCacheKey::statistics_snapshot_id, &idx::HotPointLookupCacheKey::descriptor_set_digest,
    &idx::HotPointLookupCacheKey::index_definition_digest, &idx::HotPointLookupCacheKey::security_policy_digest,
    &idx::HotPointLookupCacheKey::redaction_policy_digest, &idx::HotPointLookupCacheKey::access_policy_digest,
    &idx::HotPointLookupCacheKey::collation_profile_digest};
  const char* labels[] = {"stats_snapshot", "descriptor_set", "index_definition", "security_policy",
                         "redaction_policy", "access_policy", "collation_profile"};
  for (unsigned field = 0; field < 6; ++field) {
    auto a = key, b = key;
    a.*members[field] = std::string("x|") + labels[field + 1] + "=y";
    a.*members[field + 1] = "z";
    b.*members[field] = "x";
    b.*members[field + 1] = std::string("y|") + labels[field + 1] + "=z";
    Require(idx::BuildHotPointLookupCacheKey(a) != idx::BuildHotPointLookupCacheKey(b),
            "cache key fields alias through embedded delimiters");
  }
  auto shortened = key; shortened.encoded_probe_key = "key";
  Require(idx::BuildHotPointLookupCacheKey(shortened) != idx::BuildHotPointLookupCacheKey(key),
          "embedded NUL truncated a cache key");
  idx::AdaptiveHotPointLookupCache cache;
  idx::HotPointLookupCacheEntry entry; entry.key = key;
  idx::HotPointLookupCandidate candidate;
  candidate.locator.table_uuid = key.object_uuid;
  candidate.locator.row_uuid = GeneratedUuid(platform::UuidKind::row);
  entry.candidates.push_back(candidate); entry.dependency_uuids.push_back(key.object_uuid);
  Require(cache.Put(entry).admitted && cache.Lookup(key).cache_hit, "binary cache admission/reuse failed");
  auto mismatched = entry; mismatched.candidates[0].locator.table_uuid = GeneratedUuid(platform::UuidKind::object);
  Require(!cache.Put(mismatched).admitted, "foreign relation candidate entered cache");
  mismatched = entry; mismatched.dependency_uuids[0].value.bytes[6] = 0x40;
  Require(!cache.Put(mismatched).admitted, "non-v7 dependency entered cache");
  mismatched = entry; mismatched.candidates[0].locator.version_uuid = GeneratedUuid(platform::UuidKind::row);
  mismatched.candidates[0].locator.version_uuid.value.bytes[6] = 0x40;
  Require(!cache.Put(mismatched).admitted, "non-v7 version candidate entered cache");
  auto invalid = key; invalid.database_uuid.value.bytes[6] = 0x40;
  Require(!cache.Lookup(invalid).cache_hit, "non-v7 cache lookup was admitted");
  idx::HotPointLookupInvalidationEvent event;
  event.event_kind = "catalog_alter"; event.dependency_uuid = key.object_uuid;
  Require(cache.Invalidate(event).invalidated_count == 1, "binary dependency invalidation missed entry");
  const auto invalidated = cache.Lookup(key);
  Require(!invalidated.cache_hit && invalidated.entry &&
      invalidated.entry->invalidation_dependency_uuid.value == key.object_uuid.value &&
      invalidated.entry->invalidation_dependency_uuid.kind == key.object_uuid.kind,
      "invalidation lost binary dependency identity");

  for (const bool replace : {false, true}) {
    auto next = entry;
    next.candidates[0].locator.row_uuid = GeneratedUuid(platform::UuidKind::row);
    if (!replace) next.key.encoded_probe_key = "different cache key at capacity";
    std::size_t points = 0;
    {
      idx::AdaptiveHotPointLookupCache measured({1, 64, 64, 1});
      Require(measured.Put(entry).admitted, "seed allocation-count cache");
      allocation_attempts = 0;
      allocations_before_failure = std::numeric_limits<std::ptrdiff_t>::max();
      const auto inserted = measured.Put(next);
      allocations_before_failure = -1;
      points = allocation_attempts;
      Require(inserted.admitted && points != 0, "measure actual cache publication allocations");
    }
    for (std::size_t point = 0; point < points; ++point) {
      idx::AdaptiveHotPointLookupCache trial({1, 64, 64, 1});
      Require(trial.Put(entry).admitted, "seed failure-atomic cache");
      const auto before = trial.PartitionCounters(0);
      bool threw = false;
      allocation_attempts = 0; allocations_before_failure = static_cast<std::ptrdiff_t>(point);
      try { (void)trial.Put(next); } catch (const std::bad_alloc&) { threw = true; }
      allocations_before_failure = -1;
      const auto after = trial.PartitionCounters(0);
      Require(threw && after.puts == before.puts && after.entry_count == before.entry_count,
              "allocation failure changed cache publication counters");
      const auto retained = trial.Lookup(entry.key);
      Require(retained.cache_hit && retained.entry &&
          retained.entry->candidates[0].locator.row_uuid.value == candidate.locator.row_uuid.value,
          "failed cache publication evicted or changed the previous locator");
      if (!replace) Require(!trial.Lookup(next.key).cache_hit, "failed cache insertion published a locator");
    }
    std::cout << "cache_put replace=" << replace << " allocation_points=" << points << '\n';
  }
  idx::AdaptiveHotPointLookupCache at_capacity({1, 64, 64, 2});
  auto other = entry; other.key.encoded_probe_key = "other at-capacity entry";
  Require(at_capacity.Put(entry).admitted && at_capacity.Put(other).admitted, "seed full cache");
  entry.candidates[0].locator.row_uuid = GeneratedUuid(platform::UuidKind::row);
  Require(at_capacity.Put(entry).admitted && at_capacity.Lookup(other.key).cache_hit &&
      at_capacity.PartitionCounters(0).entry_count == 2,
      "replacement at capacity evicted an unrelated entry");
}

}  // namespace

int main() {
  TestExplicitRowUuidStreams();
  TestOnConflictConsumesUniquePhysicalLocatorStream();
  TestMergeRowUuidAndIndexRangeOrdinalEvidence();
  TestUpdateDeleteFailClosedAndExactFallback();
  TestIndexedPlanWithoutPhysicalTreeRefusesUnlessExplicitFallback();
  TestBinaryIdentityAdmissionAndActualCacheLocator();
  TestBinaryCacheKeyFramingAndDependencies();
  std::cout << "dml_binary_locator checks=" << checks << " failures=0\n";
  return EXIT_SUCCESS;
}
