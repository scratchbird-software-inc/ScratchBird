// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_index_write_path.hpp"
#include "dml/dml_row_locator_stream.hpp"
#include "index_key_encoding.hpp"
#include "index_route_capability.hpp"
#include "secondary_index_delta_overlay.hpp"
#include "uuid.hpp"

#include <algorithm>
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
  std::cerr << "ORH-230/231 gate failure: " << message << '\n';
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
  Require(parsed_row.ok() && parsed_version.ok(),
          "row/version uuid parse failed");
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
    const auto inserted = page::InsertUniqueIndexBtreeCell(tree, request);
    Require(inserted.ok() && !inserted.conflict,
            "unique physical insert failed");
    return;
  }
  page::IndexBtreePhysicalInsertRequest request;
  request.cell = cell;
  const auto inserted = page::InsertIndexBtreeCell(tree, request);
  Require(inserted.ok(), "physical insert failed");
}

std::size_t CountKey(const page::IndexBtreePhysicalTree& tree,
                     const std::string& index_uuid,
                     const std::string& key) {
  const auto scan =
      page::PointLookupIndexBtreePhysicalTree(tree, EncodedKey(index_uuid, key));
  Require(scan.ok(), "point lookup failed");
  return scan.locators.size();
}

api::CrudIndexRecord Index(std::string uuid,
                           std::string table_uuid,
                           std::string family,
                           std::string column,
                           bool unique = false) {
  api::CrudIndexRecord index;
  index.creator_tx = 230;
  index.index_uuid = std::move(uuid);
  index.table_uuid = std::move(table_uuid);
  index.family = std::move(family);
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.column_name = std::move(column);
  index.unique = unique;
  if (unique) {
    index.key_envelopes.push_back("unique");
  }
  return index;
}

api::DmlIndexWriteRowImage Row(std::string row_uuid,
                               std::string version_uuid,
                               std::string id,
                               std::string name,
                               std::string payload = "payload") {
  api::DmlIndexWriteRowImage row;
  row.row_uuid = std::move(row_uuid);
  row.version_uuid = std::move(version_uuid);
  row.values.push_back({"id", std::move(id)});
  row.values.push_back({"name", std::move(name)});
  row.values.push_back({"payload", std::move(payload)});
  return row;
}

api::DmlIndexWriteEvent BaseEvent(api::DmlIndexWriteOperation operation,
                                  const api::CrudIndexRecord& index,
                                  std::uint32_t ordinal) {
  api::DmlIndexWriteEvent event;
  event.operation = operation;
  event.index = index;
  event.table_uuid = index.table_uuid;
  event.transaction_uuid = UuidText(platform::UuidKind::transaction,
                                    1702300000000ull,
                                    static_cast<platform::byte>(0x20 + ordinal));
  event.local_transaction_id = 230 + ordinal;
  event.source_ordinal = ordinal;
  event.action_ordinal = ordinal;
  event.mga_transaction_identity_proof = true;
  event.mga_transaction_finality_authority_proof = true;
  event.rollback_evidence_token = "rollback-token-orh-230";
  event.index_descriptor_capability_proof = true;
  event.key_extraction_proof = true;
  event.partial_predicate_proof = true;
  event.covering_payload_proof = true;
  event.unique_preflight_proof = true;
  event.unique_reservation_preflight_proof = true;
  return event;
}

std::vector<std::string> DeferredOptions() {
  return {idx::kDeferredSecondaryIndexRuntimeOption,
          idx::kSecondaryIndexDeltaLedgerFeatureOption,
          idx::kDeltaLedgerReaderOverlayOption,
          idx::kDeltaLedgerCleanupHorizonBoundOption,
          idx::kDeltaLedgerRecoveryClassifiableOption};
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.evidence_kind == kind &&
           item.evidence_id.find(id) != std::string::npos;
  });
}

api::DmlTargetAccessPlanRequest BasePlanRequest(
    const std::string& relation_uuid) {
  api::DmlTargetAccessPlanRequest request;
  request.mutation_kind = "orh_230_locator_batch";
  request.relation_uuid = relation_uuid;
  request.access_descriptor_present = true;
  request.mga_visibility_recheck_planned = true;
  request.security_recheck_planned = true;
  request.grants_proven = true;
  request.security_context_present = true;
  request.observed_catalog_epoch = 230;
  request.current_catalog_epoch = 230;
  request.observed_security_epoch = 230;
  request.current_security_epoch = 230;
  request.observed_policy_epoch = 230;
  request.current_policy_epoch = 230;
  request.observed_stats_epoch = 230;
  request.current_stats_epoch = 230;
  request.local_transaction_id = 230;
  return request;
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
  request.predicate_descriptor_digest = "orh-230-encoded-key-digest";
  request.index_uuid = index_uuid;
  request.index_family = unique ? "unique_btree" : api::kCrudIndexFamilyBtree;
  request.index_unique = unique;
  auto plan = api::BuildDmlTargetAccessPlan(request);
  Require(plan.ok, "index access plan failed");
  return plan;
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

api::DmlIndexWritePathResult ApplyWriteBatch(
    std::vector<api::DmlIndexWriteEvent> events,
    page::IndexBtreePhysicalTree* tree,
    idx::PersistentSecondaryIndexDeltaLedger* ledger = nullptr,
    bool deferred = false) {
  Require(!events.empty(), "write batch requires events");
  api::DmlIndexWritePathRequest request;
  request.events = std::move(events);
  if (tree != nullptr) {
    request.physical_trees.push_back(
        {request.events.front().index.index_uuid, tree});
  }
  if (ledger != nullptr) {
    request.secondary_delta_ledgers.push_back(
        {request.events.front().index.index_uuid, ledger, {}});
  }
  if (deferred) {
    request.deferred_secondary_index_options = DeferredOptions();
    request.cleanup_horizon_token = "cleanup-horizon-orh-231";
    request.durable_mga_inventory_proof = true;
    request.delta_overlay_read_proof = true;
    request.recovery_classification_proof = true;
    request.unique_reservation_protocol_proof = true;
    request.unique_deferred_route_closure_proof = true;
  }
  return api::ApplyDmlIndexWritePath(request);
}

void RequireLocatorAuthorityEvidence(
    const api::DmlRowLocatorStreamResult& result,
    std::string_view context) {
  Require(result.ok, std::string(context) + " locator stream refused");
  Require(HasEvidence(result.evidence, "mga_visibility_recheck", "required"),
          std::string(context) + " missing MGA visibility recheck evidence");
  Require(HasEvidence(result.evidence, "security_recheck", "required"),
          std::string(context) + " missing security recheck evidence");
  Require(HasEvidence(result.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          std::string(context) + " missing engine finality authority");
  Require(HasEvidence(result.evidence, "parser_or_donor_authority", "false"),
          std::string(context) + " allowed parser/donor authority");
  Require(HasEvidence(result.evidence,
                      "index_or_cache_finality_authority",
                      "false"),
          std::string(context) + " allowed index/cache finality authority");
  Require(!result.locators.empty(),
          std::string(context) + " produced no live locators");
}

void RequireWriteAuthorityEvidence(
    const api::DmlIndexWritePathResult& result,
    std::string_view context) {
  Require(result.ok, std::string(context) + " write path refused");
  Require(HasEvidence(result.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          std::string(context) + " missing MGA finality authority");
  Require(HasEvidence(result.evidence, "parser_or_donor_authority", "false"),
          std::string(context) + " allowed parser/donor authority");
  Require(HasEvidence(result.evidence,
                      "tree_mutation_finality_authority",
                      "false"),
          std::string(context) + " allowed tree finality authority");
  Require(HasEvidence(result.evidence,
                      "dml_index_write_rollback_safe",
                      "rollback-token-orh-230"),
          std::string(context) + " missing rollback-safe write evidence");
}

void TestUpdateDeleteMergeAndConflictConsumeLocatorBatches() {
  const std::string table_uuid =
      UuidText(platform::UuidKind::object, 1702300100000ull, 0x31);
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1702300101000ull, 0x32);
  auto tree = MakeTree(index_uuid);
  const auto index =
      Index(index_uuid, table_uuid, api::kCrudIndexFamilyBtree, "name");

  const std::string update_row =
      UuidText(platform::UuidKind::row, 1702300102000ull, 0x33);
  const std::string delete_row =
      UuidText(platform::UuidKind::row, 1702300103000ull, 0x34);
  const std::string merge_row =
      UuidText(platform::UuidKind::row, 1702300104000ull, 0x35);
  const std::string update_v1 =
      UuidText(platform::UuidKind::row, 1702300105000ull, 0x36);
  const std::string delete_v1 =
      UuidText(platform::UuidKind::row, 1702300106000ull, 0x37);
  const std::string merge_v1 =
      UuidText(platform::UuidKind::row, 1702300107000ull, 0x38);

  InsertCell(&tree, Cell(index_uuid, "alpha", update_row, update_v1));
  InsertCell(&tree, Cell(index_uuid, "bravo", delete_row, delete_v1));
  InsertCell(&tree, Cell(index_uuid, "charlie", merge_row, merge_v1));

  auto update_stream = BaseStreamRequest(
      api::DmlRowLocatorStreamConsumer::update,
      RowUuidListPlan(table_uuid, {update_row, merge_row}));
  auto stream_result = api::BuildDmlRowLocatorStream(update_stream);
  RequireLocatorAuthorityEvidence(stream_result, "UPDATE row UUID list");
  Require(stream_result.source == api::DmlRowLocatorStreamSource::row_uuid_list,
          "UPDATE did not consume row UUID locator batch");
  Require(stream_result.locators.size() == 2,
          "UPDATE row UUID locator batch count mismatch");

  auto indexed_delete = BaseStreamRequest(
      api::DmlRowLocatorStreamConsumer::delete_row,
      IndexPlan(table_uuid, index_uuid, "index_range", false));
  indexed_delete.applicable_physical_index_exists = true;
  indexed_delete.physical_tree = &tree;
  indexed_delete.lower_bound.unbounded = false;
  indexed_delete.lower_bound.inclusive = true;
  indexed_delete.lower_bound.encoded_key = EncodedKey(index_uuid, "bravo");
  indexed_delete.upper_bound.unbounded = false;
  indexed_delete.upper_bound.inclusive = true;
  indexed_delete.upper_bound.encoded_key = EncodedKey(index_uuid, "charlie");
  stream_result = api::BuildDmlRowLocatorStream(indexed_delete);
  RequireLocatorAuthorityEvidence(stream_result, "DELETE physical range");
  Require(stream_result.source ==
              api::DmlRowLocatorStreamSource::physical_btree_range,
          "DELETE did not consume index-backed range locator stream");
  Require(stream_result.locators.size() == 2,
          "DELETE physical range locator count mismatch");
  Require(HasEvidence(stream_result.evidence,
                      "dml_row_locator_stream_no_table_scan",
                      "physical_index_locator_stream_consumed"),
          "DELETE range lost no-table-scan physical evidence");

  auto merge_stream = BaseStreamRequest(
      api::DmlRowLocatorStreamConsumer::merge,
      IndexPlan(table_uuid, index_uuid, "scalar_eq", false));
  merge_stream.applicable_physical_index_exists = true;
  merge_stream.physical_tree = &tree;
  merge_stream.encoded_point_key = EncodedKey(index_uuid, "alpha");
  merge_stream.merge_ordinals.push_back({0, 0, true});
  stream_result = api::BuildDmlRowLocatorStream(merge_stream);
  RequireLocatorAuthorityEvidence(stream_result, "MERGE physical point");
  Require(stream_result.source ==
              api::DmlRowLocatorStreamSource::physical_btree_point,
          "MERGE did not consume index-backed point locator stream");
  Require(HasEvidence(stream_result.evidence,
                      "merge_locator_stream_source_action_order",
                      "0:0:matched"),
          "MERGE locator source/action evidence missing");

  const std::string unique_index_uuid =
      UuidText(platform::UuidKind::object, 1702300108000ull, 0x39);
  auto unique_tree = MakeTree(unique_index_uuid);
  const auto unique_index =
      Index(unique_index_uuid, table_uuid, "unique_btree", "id", true);
  const std::string conflict_row =
      UuidText(platform::UuidKind::row, 1702300109000ull, 0x3a);
  const std::string conflict_v1 =
      UuidText(platform::UuidKind::row, 1702300110000ull, 0x3b);
  InsertCell(&unique_tree,
             Cell(unique_index_uuid, "42", conflict_row, conflict_v1),
             true);
  auto conflict_stream = BaseStreamRequest(
      api::DmlRowLocatorStreamConsumer::on_conflict,
      IndexPlan(table_uuid, unique_index_uuid, "unique_eq", true));
  conflict_stream.index_unique = true;
  conflict_stream.applicable_physical_index_exists = true;
  conflict_stream.physical_tree = &unique_tree;
  conflict_stream.encoded_point_key = EncodedKey(unique_index_uuid, "42");
  stream_result = api::BuildDmlRowLocatorStream(conflict_stream);
  RequireLocatorAuthorityEvidence(stream_result, "ON CONFLICT unique point");
  Require(stream_result.source ==
              api::DmlRowLocatorStreamSource::physical_unique_btree_point,
          "ON CONFLICT did not consume unique index-backed locator");
  Require(HasEvidence(stream_result.evidence,
                      "on_conflict_unique_locator_stream",
                      "consumed_no_table_scan"),
          "ON CONFLICT unique locator evidence missing");

  std::vector<api::DmlIndexWriteEvent> events;
  auto update = BaseEvent(api::DmlIndexWriteOperation::update, index, 0);
  update.has_old_row = true;
  update.old_row = Row(update_row, update_v1, "1", "alpha");
  update.has_new_row = true;
  update.new_row =
      Row(update_row,
          UuidText(platform::UuidKind::row, 1702300111000ull, 0x3c),
          "1",
          "delta");
  events.push_back(update);
  auto delete_event =
      BaseEvent(api::DmlIndexWriteOperation::delete_row, index, 1);
  delete_event.has_old_row = true;
  delete_event.old_row = Row(delete_row, delete_v1, "2", "bravo");
  events.push_back(delete_event);
  auto merge_update =
      BaseEvent(api::DmlIndexWriteOperation::merge_update, index, 2);
  merge_update.has_old_row = true;
  merge_update.old_row = Row(merge_row, merge_v1, "3", "charlie");
  merge_update.has_new_row = true;
  merge_update.new_row =
      Row(merge_row,
          UuidText(platform::UuidKind::row, 1702300112000ull, 0x3d),
          "3",
          "echo");
  events.push_back(merge_update);

  const auto write_result = ApplyWriteBatch(std::move(events), &tree);
  RequireWriteAuthorityEvidence(write_result, "locator-backed DML batch");
  Require(write_result.physical_deletes == 3 && write_result.physical_inserts == 2,
          "locator-backed DML batch did not mutate expected index entries");
  Require(write_result.merge_events == 1, "MERGE write evidence missing");
  Require(CountKey(tree, index_uuid, "alpha") == 0 &&
              CountKey(tree, index_uuid, "bravo") == 0 &&
              CountKey(tree, index_uuid, "charlie") == 0 &&
              CountKey(tree, index_uuid, "delta") == 1 &&
              CountKey(tree, index_uuid, "echo") == 1,
          "locator-backed DML batch left incorrect physical index state");
}

void TestFailClosedLocatorAndRouteLimits() {
  const std::string table_uuid =
      UuidText(platform::UuidKind::object, 1702300200000ull, 0x41);
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1702300201000ull, 0x42);

  auto empty = BasePlanRequest(table_uuid);
  empty.predicate_kind = "row_uuid_in_list";
  const auto empty_plan = api::BuildDmlTargetAccessPlan(empty);
  Require(!empty_plan.ok, "empty row-locator list produced accepted plan");

  auto stale = BasePlanRequest(table_uuid);
  stale.predicate_kind = "scalar_eq";
  stale.index_uuid = index_uuid;
  stale.observed_security_epoch = 1;
  stale.current_security_epoch = 2;
  auto stale_plan = api::BuildDmlTargetAccessPlan(stale);
  Require(!stale_plan.ok, "stale security epoch access plan was admitted");
  auto stream = BaseStreamRequest(api::DmlRowLocatorStreamConsumer::update,
                                  stale_plan);
  auto stream_result = api::BuildDmlRowLocatorStream(stream);
  Require(!stream_result.ok, "stale locator stream was admitted");
  Require(HasEvidence(stream_result.evidence,
                      "dml_row_locator_stream_refusal",
                      "access_plan_not_safe"),
          "stale locator stream diagnostic missing");

  auto safe_plan = IndexPlan(table_uuid, index_uuid, "scalar_eq", false);
  stream = BaseStreamRequest(api::DmlRowLocatorStreamConsumer::delete_row,
                             safe_plan);
  stream.parser_or_donor_authority = true;
  stream_result = api::BuildDmlRowLocatorStream(stream);
  Require(!stream_result.ok,
          "parser/donor-authoritative locator stream was admitted");
  Require(HasEvidence(stream_result.evidence,
                      "dml_row_locator_stream_refusal",
                      "parser_or_donor_authority_forbidden"),
          "parser/donor authority refusal evidence missing");

  const auto* hash_select = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::sql_select, idx::IndexFamily::hash);
  Require(hash_select != nullptr, "hash select route capability missing");
  Require(hash_select->supports_equality_lookup,
          "hash route lost equality lookup capability");
  Require(!hash_select->supports_ordered_range,
          "hash route was incorrectly treated as ordered range");

  const auto* donor_update = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::dml_update, idx::IndexFamily::donor_emulated);
  Require(donor_update != nullptr && !donor_update->route_complete(),
          "donor-emulated DML update route was benchmark-clean");
  const auto* policy_update = idx::FindBuiltinIndexRouteCapabilityState(
      idx::IndexRouteKind::dml_update, idx::IndexFamily::policy_blocked);
  Require(policy_update != nullptr && !policy_update->route_complete(),
          "policy-blocked DML update route was benchmark-clean");

  const auto hash_index =
      Index(index_uuid, table_uuid, api::kCrudIndexFamilyHash, "name");
  idx::PersistentSecondaryIndexDeltaLedger hash_ledger;
  auto hash_update =
      BaseEvent(api::DmlIndexWriteOperation::update, hash_index, 3);
  const std::string row_uuid =
      UuidText(platform::UuidKind::row, 1702300202000ull, 0x43);
  hash_update.has_old_row = true;
  hash_update.old_row =
      Row(row_uuid,
          UuidText(platform::UuidKind::row, 1702300203000ull, 0x44),
          "1",
          "hash-old");
  hash_update.has_new_row = true;
  hash_update.new_row =
      Row(row_uuid,
          UuidText(platform::UuidKind::row, 1702300204000ull, 0x45),
          "1",
          "hash-new");
  auto write_result = ApplyWriteBatch({hash_update}, nullptr, &hash_ledger, true);
  Require(write_result.ok && write_result.secondary_delta_ledger_appends == 2,
          "hash family was not admitted to deferred DML update ledger route");

  auto donor = Index(index_uuid,
                     table_uuid,
                     api::kCrudIndexFamilyDonorEmulated,
                     "name");
  auto donor_update_event =
      BaseEvent(api::DmlIndexWriteOperation::update, donor, 4);
  donor_update_event.has_old_row = true;
  donor_update_event.old_row =
      Row(row_uuid,
          UuidText(platform::UuidKind::row, 1702300205000ull, 0x46),
          "1",
          "donor-old");
  donor_update_event.has_new_row = true;
  donor_update_event.new_row =
      Row(row_uuid,
          UuidText(platform::UuidKind::row, 1702300206000ull, 0x47),
          "1",
          "donor-new");
  auto donor_tree = MakeTree(index_uuid);
  write_result = ApplyWriteBatch({donor_update_event}, &donor_tree);
  Require(!write_result.ok, "donor-emulated DML update was admitted");
  Require(HasEvidence(write_result.evidence,
                      "dml_index_family_fail_closed",
                      "donor_emulated"),
          "donor-emulated fail-closed evidence missing");

  auto candidate = Index(index_uuid,
                         table_uuid,
                         api::kCrudIndexFamilyVectorHnsw,
                         "name");
  auto candidate_update =
      BaseEvent(api::DmlIndexWriteOperation::update, candidate, 5);
  candidate_update.has_old_row = true;
  candidate_update.old_row =
      Row(row_uuid,
          UuidText(platform::UuidKind::row, 1702300207000ull, 0x48),
          "1",
          "candidate-old");
  candidate_update.has_new_row = true;
  candidate_update.new_row =
      Row(row_uuid,
          UuidText(platform::UuidKind::row, 1702300208000ull, 0x49),
          "1",
          "candidate-new");
  auto candidate_tree = MakeTree(index_uuid);
  write_result = ApplyWriteBatch({candidate_update}, &candidate_tree);
  Require(!write_result.ok,
          "approximate/candidate vector DML admitted without ledger proof");
  Require(write_result.diagnostic.code ==
              "SB-DML-HOT-DELTA-REQUIRED-FOR-FAMILY",
          "candidate-route missing-ledger diagnostic mismatch");
}

void TestHotLikeAndDeferredDeltaUpdateFastPaths() {
  const std::string table_uuid =
      UuidText(platform::UuidKind::object, 1702310100000ull, 0x51);
  const std::string index_uuid =
      UuidText(platform::UuidKind::object, 1702310101000ull, 0x52);
  const auto index =
      Index(index_uuid, table_uuid, api::kCrudIndexFamilyBtree, "name");
  auto tree = MakeTree(index_uuid);
  const std::string row_uuid =
      UuidText(platform::UuidKind::row, 1702310102000ull, 0x53);
  const std::string v1 =
      UuidText(platform::UuidKind::row, 1702310103000ull, 0x54);
  const std::string v2 =
      UuidText(platform::UuidKind::row, 1702310104000ull, 0x55);
  InsertCell(&tree, Cell(index_uuid, "alpha", row_uuid, v1));

  auto unchanged = BaseEvent(api::DmlIndexWriteOperation::update, index, 6);
  unchanged.has_old_row = true;
  unchanged.old_row = Row(row_uuid, v1, "1", "alpha");
  unchanged.has_new_row = true;
  unchanged.new_row = Row(row_uuid, v2, "1", "alpha", "payload-changed");
  auto write_result = ApplyWriteBatch({unchanged}, &tree);
  RequireWriteAuthorityEvidence(write_result, "HOT-like unchanged update");
  Require(write_result.hot_like_version_appends == 1,
          "HOT-like unchanged update did not append row version");
  Require(write_result.physical_inserts == 0 &&
              write_result.physical_deletes == 0,
          "HOT-like update churned physical index entries");
  Require(CountKey(tree, index_uuid, "alpha") == 1,
          "HOT-like update lost old index locator");
  Require(HasEvidence(write_result.evidence,
                      "old_index_locator_remains_valid_through_mga_chain",
                      index_uuid),
          "HOT-like MGA chain recheck evidence missing");

  const std::string hash_index_uuid =
      UuidText(platform::UuidKind::object, 1702310109000ull, 0x5a);
  const auto hash_index =
      Index(hash_index_uuid, table_uuid, api::kCrudIndexFamilyHash, "name");
  idx::PersistentSecondaryIndexDeltaLedger ledger;
  auto changed = BaseEvent(api::DmlIndexWriteOperation::update, hash_index, 7);
  changed.has_old_row = true;
  changed.old_row = Row(row_uuid, v2, "1", "alpha");
  changed.has_new_row = true;
  changed.new_row =
      Row(row_uuid,
          UuidText(platform::UuidKind::row, 1702310105000ull, 0x56),
          "1",
          "bravo");
  write_result = ApplyWriteBatch({changed}, nullptr, &ledger, true);
  RequireWriteAuthorityEvidence(write_result,
                                "non-ordered changed-index deferred delta update");
  Require(write_result.physical_inserts == 0 &&
              write_result.physical_deletes == 0,
          "non-ordered deferred delta update performed physical churn");
  Require(write_result.secondary_delta_ledger_appends == 2,
          "non-ordered deferred delta update did not append before/after records");
  Require(write_result.secondary_delta_overlay_reads == 1,
          "non-ordered deferred delta update did not exercise overlay read");
  Require(ledger.records.size() == 2,
          "non-ordered deferred delta ledger count mismatch");
  Require(HasEvidence(write_result.evidence,
                      "secondary_delta_ledger_appended",
                      "true"),
          "secondary delta ledger append evidence missing");
  Require(HasEvidence(write_result.evidence, "delta_overlay_read_safe", "true"),
          "delta overlay safe evidence missing");
  Require(HasEvidence(write_result.evidence,
                      "durable_mga_inventory_remains_authority",
                      "true"),
          "deferred delta MGA authority evidence missing");

  const std::string unique_uuid =
      UuidText(platform::UuidKind::object, 1702310106000ull, 0x57);
  const auto unique =
      Index(unique_uuid, table_uuid, "unique_btree", "id", true);
  idx::PersistentSecondaryIndexDeltaLedger unique_ledger;
  auto unique_update =
      BaseEvent(api::DmlIndexWriteOperation::update, unique, 8);
  unique_update.has_old_row = true;
  unique_update.old_row =
      Row(row_uuid,
          UuidText(platform::UuidKind::row, 1702310107000ull, 0x58),
          "1",
          "alpha");
  unique_update.has_new_row = true;
  unique_update.new_row =
      Row(row_uuid,
          UuidText(platform::UuidKind::row, 1702310108000ull, 0x59),
          "2",
          "alpha");
  auto unique_tree = MakeTree(unique_uuid);
  InsertCell(&unique_tree,
             Cell(unique_uuid,
                  "1",
                  row_uuid,
                  unique_update.old_row.version_uuid),
             true);
  write_result = ApplyWriteBatch({unique_update}, &unique_tree, &unique_ledger, true);
  Require(write_result.ok,
          "ordered unique update did not fall back to synchronous rewrite");
  Require(write_result.physical_inserts == 1 && write_result.physical_deletes == 1,
          "ordered unique fallback did not rewrite physical entries");
  Require(unique_ledger.records.empty(),
          "ordered unique synchronous fallback appended delta records");
}

}  // namespace

int main() {
  TestUpdateDeleteMergeAndConflictConsumeLocatorBatches();
  TestFailClosedLocatorAndRouteLimits();
  TestHotLikeAndDeferredDeltaUpdateFastPaths();
  return EXIT_SUCCESS;
}
