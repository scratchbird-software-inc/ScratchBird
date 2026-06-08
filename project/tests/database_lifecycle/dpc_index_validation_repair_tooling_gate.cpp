// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_api_sblr.hpp"
#include "index_backup_restore.hpp"
#include "index_management.hpp"
#include "index_validation_repair_tooling.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_INDEX_VALIDATION_REPAIR_TOOLING_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

struct UuidFactory {
  platform::u64 base_millis = NowMillis();

  platform::TypedUuid Typed(platform::UuidKind kind, platform::u64 salt) const {
    const auto generated =
        uuid::GenerateEngineIdentityV7(kind, base_millis + salt);
    Require(generated.ok(), "DPC-046 generated UUID creation failed");
    return generated.value;
  }

  std::string Text(platform::UuidKind kind, platform::u64 salt) const {
    return uuid::UuidToString(Typed(kind, salt).value);
  }
};

bool HasEvidence(const idx::IndexValidationRepairResult& result,
                 std::string_view key,
                 std::string_view value = {}) {
  for (const auto& evidence : result.support_evidence) {
    if (evidence.key == key && (value.empty() || evidence.value == value)) {
      return true;
    }
  }
  return false;
}

idx::IndexValidationRepairTarget Target(const UuidFactory& uuids,
                                        platform::u64 salt,
                                        idx::IndexFamily family =
                                            idx::IndexFamily::btree) {
  idx::IndexValidationRepairTarget target;
  target.database_uuid = uuids.Typed(platform::UuidKind::database, salt + 1);
  target.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  target.index_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  target.generation_uuid = uuids.Typed(platform::UuidKind::object, salt + 4);
  target.physical_family = family;
  return target;
}

idx::IndexCandidate Candidate(const UuidFactory& uuids,
                             const idx::IndexValidationRepairTarget& target,
                             platform::u64 salt,
                             std::string key) {
  idx::IndexCandidate candidate;
  candidate.key.encoded_key = std::move(key);
  candidate.locator.table_uuid = target.table_uuid;
  candidate.locator.row_uuid = uuids.Typed(platform::UuidKind::row, salt + 1);
  candidate.locator.version_uuid = uuids.Typed(platform::UuidKind::row, salt + 2);
  candidate.locator.local_transaction_id = salt + 10;
  candidate.mga_visible = true;
  candidate.predicate_exact = true;
  candidate.security_visible = true;
  return candidate;
}

idx::IndexValidationRepairRequest BaseRequest(
    const UuidFactory& uuids,
    platform::u64 salt,
    idx::IndexValidationRepairFamily family,
    idx::IndexValidationRepairOperation operation =
        idx::IndexValidationRepairOperation::validate) {
  idx::IndexValidationRepairRequest request;
  request.operation = operation;
  request.validation_family = family;
  request.target = Target(uuids, salt);
  request.policy_allows_mutation = operation ==
      idx::IndexValidationRepairOperation::validate ? false : true;
  return request;
}

idx::SecondaryIndexDeltaLedgerRecord DeltaRecord(
    const UuidFactory& uuids,
    const idx::IndexValidationRepairTarget& target,
    platform::u64 salt,
    idx::SecondaryIndexDeltaLedgerCommitState state) {
  idx::SecondaryIndexDeltaLedgerRecord record;
  record.delta.delta_id = uuids.Typed(platform::UuidKind::object, salt + 1);
  record.delta.index_uuid = target.index_uuid;
  record.delta.table_uuid = target.table_uuid;
  record.delta.row_uuid = uuids.Typed(platform::UuidKind::row, salt + 2);
  record.delta.version_uuid = uuids.Typed(platform::UuidKind::row, salt + 3);
  record.delta.transaction_uuid =
      uuids.Typed(platform::UuidKind::transaction, salt + 4);
  record.delta.local_transaction_id = salt + 20;
  record.delta.delta_kind = idx::SecondaryIndexDeltaKind::insert;
  record.delta.key_payload = "key:" + std::to_string(salt);
  record.delta.cleanup_horizon_token = "horizon:" + std::to_string(salt);
  record.delta.committed =
      state != idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted;
  record.commit_state = state;
  record.source_evidence_reference = "dpc046:delta:" + std::to_string(salt);
  return record;
}

idx::PageExtentSummaryFormatCompatibility CurrentFormat() {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryFormatCompatibility format;
  format.observed = contract.current;
  format.open_class = idx::PageExtentSummaryFormatOpenClass::current;
  format.compatible = true;
  format.migration_required = false;
  format.diagnostic_code = "DPC-046.current_format";
  return format;
}

idx::PageExtentSummaryMetadata PageSummary(const UuidFactory& uuids,
                                           platform::u64 salt) {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryMetadata metadata;
  metadata.relation_uuid = uuids.Text(platform::UuidKind::object, salt + 1);
  metadata.summary_uuid = uuids.Text(platform::UuidKind::object, salt + 2);
  metadata.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  metadata.range.first_page_id = 10;
  metadata.range.page_count = 8;
  metadata.boundary.scalar_type_key = "int64_lex";
  metadata.boundary.encoded_min = "010";
  metadata.boundary.encoded_max = "030";
  metadata.boundary.min_present = true;
  metadata.boundary.max_present = true;
  metadata.row_count = 2;
  metadata.status = idx::PageExtentSummaryStatus::current;
  metadata.format_version = contract.current;
  metadata.generation = 2;
  metadata.persisted_record_present = true;
  metadata.checksum_valid = true;
  return metadata;
}

idx::PageExtentSummaryRowEvidence SummaryRow(platform::u64 page,
                                             std::string scalar) {
  idx::PageExtentSummaryRowEvidence row;
  row.page_id = page;
  row.extent_id = page / 16;
  row.scalar_type_key = "int64_lex";
  row.encoded_scalar = std::move(scalar);
  row.engine_mga_visible = true;
  return row;
}

idx::PageExtentSummaryMaintenanceEvent RebuildEvent(
    const idx::PageExtentSummaryMetadata& metadata) {
  idx::PageExtentSummaryMaintenanceEvent event;
  event.kind = idx::PageExtentSummaryMaintenanceEventKind::repair;
  event.relation_uuid = metadata.relation_uuid;
  event.summary_uuid = metadata.summary_uuid;
  event.base_page_rows.push_back(SummaryRow(11, "011"));
  event.base_page_rows.push_back(SummaryRow(12, "025"));
  event.caller_allows_transient_rebuild = true;
  return event;
}

idx::TimeRangeSummaryDescriptor TimeDescriptor(
    const UuidFactory& uuids,
    platform::u64 salt,
    idx::PageExtentSummaryStatus status =
        idx::PageExtentSummaryStatus::current) {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::TimeRangeSummaryDescriptor descriptor;
  descriptor.table_uuid = uuids.Text(platform::UuidKind::object, salt + 1);
  descriptor.index_uuid = uuids.Text(platform::UuidKind::object, salt + 2);
  descriptor.range_family_uuid = uuids.Text(platform::UuidKind::object, salt + 3);
  descriptor.summary_uuid = uuids.Text(platform::UuidKind::object, salt + 4);
  descriptor.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  descriptor.range.first_page_id = 20;
  descriptor.range.page_count = 3;
  descriptor.time_scalar_type_key = "timestamp_lex";
  descriptor.encoded_min_time = "2026-01-01T00";
  descriptor.encoded_max_time = "2026-01-01T10";
  descriptor.min_time_present = true;
  descriptor.max_time_present = true;
  descriptor.row_count = 4;
  descriptor.status = status;
  descriptor.format_version = contract.current;
  descriptor.generation = 7;
  descriptor.persisted_record_present = true;
  descriptor.checksum_valid = true;
  return descriptor;
}

idx::TimeRangeSummaryPruneRequest TimeRequest(
    const UuidFactory& uuids,
    platform::u64 salt,
    idx::PageExtentSummaryStatus status =
        idx::PageExtentSummaryStatus::current) {
  idx::TimeRangeSummaryPruneRequest request;
  request.format = CurrentFormat();
  request.summaries.push_back(TimeDescriptor(uuids, salt, status));
  request.predicate.time_scalar_type_key = "timestamp_lex";
  request.predicate.encoded_lower_time = "2026-01-01T20";
  request.predicate.lower_present = true;
  return request;
}

idx::ShadowIndexBuildRecord ShadowRecord(const UuidFactory& uuids,
                                         platform::u64 salt,
                                         bool published) {
  idx::ShadowIndexBuildRecord record;
  record.build_id = uuids.Typed(platform::UuidKind::object, salt + 1);
  record.shadow_index_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  record.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  record.validation_evidence_present = true;
  record.publish_barrier_evidence_present = published;
  record.publish_barrier_engine_owned_mga = published;
  record.validation_evidence_ref = "validation:shadow:" + std::to_string(salt);
  record.publish_barrier_evidence_ref = "publish:shadow:" + std::to_string(salt);
  record.engine_mga_inventory_evidence_ref =
      "mga_inventory:shadow:" + std::to_string(salt);
  record.engine_mga_horizon_evidence_ref =
      "mga_horizon:shadow:" + std::to_string(salt);
  record.state = published ? idx::ShadowIndexBuildState::published
                           : idx::ShadowIndexBuildState::validated;
  record.planner_visible = published;
  record.read_visible = published;
  record.published_index_uuid = published ? record.shadow_index_uuid
                                          : platform::TypedUuid{};
  return record;
}

idx::InvertedSearchSegmentDescriptor SearchSegment(
    const UuidFactory& uuids,
    platform::u64 salt,
    idx::InvertedSearchSegmentState state =
        idx::InvertedSearchSegmentState::visible) {
  idx::InvertedSearchSegmentDescriptor segment;
  segment.segment_uuid = uuids.Typed(platform::UuidKind::object, salt + 1);
  segment.index_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  segment.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  segment.generation = salt;
  segment.state = state;
  segment.visible = state == idx::InvertedSearchSegmentState::visible;
  segment.persisted_record_present = segment.visible;
  segment.checksum_valid = true;
  segment.complete = segment.visible;
  segment.validation_evidence_present = segment.visible;
  segment.publish_barrier_evidence_present = segment.visible;
  segment.publish_barrier_engine_owned_mga = segment.visible;
  segment.validation_evidence_ref = "validation:search:" + std::to_string(salt);
  segment.publish_barrier_evidence_ref = "publish:search:" + std::to_string(salt);
  segment.engine_mga_inventory_evidence_ref =
      "mga_inventory:search:" + std::to_string(salt);
  segment.engine_mga_horizon_evidence_ref =
      "mga_horizon:search:" + std::to_string(salt);
  return segment;
}

idx::VectorGenerationDescriptor VectorGeneration(
    const UuidFactory& uuids,
    platform::u64 salt,
    idx::VectorGenerationState state = idx::VectorGenerationState::published) {
  idx::VectorGenerationDescriptor generation;
  generation.generation_uuid = uuids.Typed(platform::UuidKind::object, salt + 1);
  generation.index_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  generation.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  generation.generation = salt;
  generation.state = state;
  generation.visible = state == idx::VectorGenerationState::published;
  generation.persisted_record_present = generation.visible;
  generation.checksum_valid = true;
  generation.complete = generation.visible;
  generation.training_evidence_present = true;
  generation.validation_evidence_present = generation.visible;
  generation.sealed_generation_evidence_present = generation.visible;
  generation.recall_contract_evidence_present = generation.visible;
  generation.publish_barrier_evidence_present = generation.visible;
  generation.publish_barrier_engine_owned_mga = generation.visible;
  generation.training_evidence_ref = "training:vector:" + std::to_string(salt);
  generation.validation_evidence_ref = "validation:vector:" + std::to_string(salt);
  generation.sealed_generation_evidence_ref =
      "sealed:vector:" + std::to_string(salt);
  generation.recall_contract_evidence_ref =
      "recall:vector:" + std::to_string(salt);
  generation.publish_barrier_evidence_ref = "publish:vector:" + std::to_string(salt);
  generation.engine_mga_inventory_evidence_ref =
      "mga_inventory:vector:" + std::to_string(salt);
  generation.engine_mga_horizon_evidence_ref =
      "mga_horizon:vector:" + std::to_string(salt);
  generation.resource_envelope.memory_limit_bytes = 1024;
  generation.resource_envelope.memory_observed_bytes = 512;
  generation.resource_envelope.temp_space_limit_bytes = 2048;
  generation.resource_envelope.temp_space_observed_bytes = 1000;
  generation.resource_envelope.worker_limit = 2;
  generation.resource_envelope.workers_used = 1;
  generation.resource_envelope.resource_governor_evidence_present = true;
  generation.resource_envelope.resource_governor_evidence_ref =
      "resource:vector:" + std::to_string(salt);
  generation.recall_contract.top_k = 4;
  generation.recall_contract.exact_sample_rows = 16;
  generation.recall_contract.required_recall = 0.9;
  generation.recall_contract.observed_recall = 0.95;
  generation.recall_contract.deterministic_sample = true;
  generation.recall_contract.evidence_present = true;
  generation.recall_contract.evidence_ref = generation.recall_contract_evidence_ref;
  return generation;
}

void ProveValidateSuccessAndFailure() {
  const UuidFactory uuids;
  auto request = BaseRequest(
      uuids, 1000, idx::IndexValidationRepairFamily::ordered_table_candidate_set);
  const auto candidate = Candidate(uuids, request.target, 1010, "alpha");
  request.state.ordered_candidate_set.expected_from_table = {candidate};
  request.state.ordered_candidate_set.observed_from_index = {candidate};
  auto valid = idx::ExecuteIndexValidationRepairOperation(request);
  Require(valid.ok(), "DPC-046 validate success was not admitted");
  Require(valid.validation_read_only && !valid.mutation_applied,
          "DPC-046 validation mutated state");
  Require(valid.classification == idx::IndexValidationRepairClass::clean,
          "DPC-046 validate success classification changed");
  Require(HasEvidence(valid, "message_vector_key",
                      "dpc.index_repair.ordered_valid"),
          "DPC-046 validate success lacked message-vector evidence");

  auto broken = request;
  broken.state.ordered_candidate_set.observed_from_index.clear();
  auto failed = idx::ExecuteIndexValidationRepairOperation(broken);
  Require(!failed.ok(), "DPC-046 validate failure did not close index use");
  Require(failed.classification == idx::IndexValidationRepairClass::rebuild_required,
          "DPC-046 validate failure did not require rebuild");
  Require(HasEvidence(failed, "ordered.missing_count", "1"),
          "DPC-046 validate failure lacked count evidence");
}

void ProveRepairRebuildDiscardAndPersistenceEvidence() {
  const UuidFactory uuids;
  auto delta = BaseRequest(
      uuids, 2000, idx::IndexValidationRepairFamily::secondary_delta_ledger,
      idx::IndexValidationRepairOperation::repair);
  delta.state.delta_ledger.records.push_back(DeltaRecord(
      uuids, delta.target, 2010,
      idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge));
  const auto repaired = idx::ExecuteIndexValidationRepairOperation(delta);
  Require(repaired.ok() && repaired.mutation_applied,
          "DPC-046 delta repair did not apply");
  Require(repaired.repaired_state.delta_ledger.records.front().commit_state ==
              idx::SecondaryIndexDeltaLedgerCommitState::merged_cleaned,
          "DPC-046 delta repair did not persist merged_cleaned state");

  const auto encoded = idx::EncodePersistentSecondaryIndexDeltaLedger(
      repaired.repaired_state.delta_ledger, idx::SecondaryIndexDeltaLedgerLimits{});
  Require(encoded.ok(), "DPC-046 repaired delta ledger did not encode");
  const auto decoded = idx::DecodePersistentSecondaryIndexDeltaLedger(
      encoded.bytes, idx::SecondaryIndexDeltaLedgerLimits{});
  Require(decoded.ok(), "DPC-046 repaired delta ledger did not decode");
  Require(idx::PersistentSecondaryIndexDeltaLedgerEquals(
              repaired.repaired_state.delta_ledger, decoded.ledger),
          "DPC-046 repaired delta ledger copy-equivalence failed");
  const auto reopened =
      idx::ClassifySecondaryIndexDeltaLedgerForRecovery(decoded.ledger);
  Require(reopened.ok(), "DPC-046 repaired delta ledger reopen refused");

  auto page = BaseRequest(
      uuids, 2100, idx::IndexValidationRepairFamily::page_extent_summary,
      idx::IndexValidationRepairOperation::rebuild);
  page.state.page_extent_summary = PageSummary(uuids, 2110);
  page.state.page_extent_summary.status = idx::PageExtentSummaryStatus::stale;
  page.state.page_extent_summary_format = CurrentFormat();
  page.state.page_extent_rebuild_event =
      RebuildEvent(page.state.page_extent_summary);
  const auto rebuilt = idx::ExecuteIndexValidationRepairOperation(page);
  Require(rebuilt.ok() && rebuilt.mutation_applied,
          "DPC-046 page summary rebuild did not apply");
  Require(rebuilt.repaired_state.page_extent_summary.status ==
              idx::PageExtentSummaryStatus::current,
          "DPC-046 page summary rebuild did not produce current metadata");

  auto shadow = BaseRequest(
      uuids, 2200, idx::IndexValidationRepairFamily::shadow_index_build_state,
      idx::IndexValidationRepairOperation::discard_unpublished);
  shadow.state.shadow_build = ShadowRecord(uuids, 2210, false);
  const auto discarded = idx::ExecuteIndexValidationRepairOperation(shadow);
  Require(discarded.ok() && discarded.mutation_applied,
          "DPC-046 shadow discard did not apply");
  Require(discarded.repaired_state.shadow_build.state ==
              idx::ShadowIndexBuildState::cancelled &&
              !discarded.repaired_state.shadow_build.planner_visible,
          "DPC-046 shadow discard left planner-visible state");
}

void ProveRouteAdmissionAndRefusal() {
  const UuidFactory uuids;
  auto request = BaseRequest(
      uuids, 3000, idx::IndexValidationRepairFamily::ordered_table_candidate_set,
      idx::IndexValidationRepairOperation::rebuild);
  const auto candidate = Candidate(uuids, request.target, 3010, "route");
  request.state.ordered_candidate_set.expected_from_table = {candidate};
  request.state.ordered_candidate_set.observed_from_index.clear();

  idx::IndexSblrOperationEnvelope envelope;
  envelope.operation = idx::IndexCanonicalOperation::rebuild_index_family;
  envelope.index_uuid = request.target.index_uuid;
  envelope.family = request.target.physical_family;
  envelope.validation_family = request.validation_family;
  auto sblr = idx::BindIndexValidationRepairSblrOperation(envelope, request);
  Require(sblr.ok() && sblr.mutation_applied,
          "DPC-046 SBLR rebuild route did not apply");
  Require(HasEvidence(sblr, "route_surface", "sblr"),
          "DPC-046 SBLR route evidence missing");

  auto refused_envelope = envelope;
  refused_envelope.names_resolved_to_uuids = false;
  auto refused_sblr =
      idx::BindIndexValidationRepairSblrOperation(refused_envelope, request);
  Require(!refused_sblr.ok() &&
              refused_sblr.diagnostic.diagnostic_code ==
                  "SB-INDEX-SBLR-AUTHORITY-VIOLATION",
          "DPC-046 SBLR authority refusal changed");

  auto management = idx::PlanIndexManagementValidationRepairOperation(request);
  Require(management.ok() && HasEvidence(management, "route_surface",
                                         "management_api"),
          "DPC-046 management route did not admit repair");

  auto read_only = request;
  read_only.read_only_database = true;
  auto read_only_refused =
      idx::PlanIndexManagementValidationRepairOperation(read_only);
  Require(!read_only_refused.ok() &&
              read_only_refused.diagnostic.diagnostic_code ==
                  "DPC.INDEX_REPAIR.READ_ONLY_REFUSED",
          "DPC-046 read-only refusal diagnostic changed");

  auto policy_refused = request;
  policy_refused.policy_allows_mutation = false;
  auto missing_policy =
      idx::PlanIndexManagementValidationRepairOperation(policy_refused);
  Require(!missing_policy.ok() &&
              missing_policy.diagnostic.diagnostic_code ==
                  "DPC.INDEX_REPAIR.MUTATION_POLICY_REQUIRED",
          "DPC-046 mutation-policy refusal diagnostic changed");
}

void ProveSpecializedFamiliesAndMovementCopyEquivalence() {
  const UuidFactory uuids;

  auto time = BaseRequest(
      uuids, 4000, idx::IndexValidationRepairFamily::time_range_summary);
  time.state.time_range_summary = TimeRequest(uuids, 4010);
  auto time_valid = idx::ExecuteIndexValidationRepairOperation(time);
  Require(time_valid.ok(), "DPC-046 time range validation did not succeed");
  auto time_stale = time;
  time_stale.state.time_range_summary = TimeRequest(
      uuids, 4020, idx::PageExtentSummaryStatus::stale);
  auto time_fallback = idx::ExecuteIndexValidationRepairOperation(time_stale);
  Require(time_fallback.classification ==
              idx::IndexValidationRepairClass::safe_fallback &&
              !time_fallback.planner_visible,
          "DPC-046 stale time range did not select safe fallback");

  auto search = BaseRequest(
      uuids, 4100,
      idx::IndexValidationRepairFamily::inverted_search_segment_state);
  search.state.inverted_segments.segments.push_back(SearchSegment(uuids, 4110));
  auto search_valid = idx::ExecuteIndexValidationRepairOperation(search);
  Require(search_valid.ok() && search_valid.planner_visible,
          "DPC-046 search segment validation did not admit visible segment");
  auto search_building = search;
  search_building.operation = idx::IndexValidationRepairOperation::discard_unpublished;
  search_building.policy_allows_mutation = true;
  search_building.state.inverted_segments.segments = {
      SearchSegment(uuids, 4120, idx::InvertedSearchSegmentState::building)};
  auto search_discarded =
      idx::ExecuteIndexValidationRepairOperation(search_building);
  Require(search_discarded.ok() && search_discarded.mutation_applied,
          "DPC-046 search segment discard did not apply");

  auto vector = BaseRequest(
      uuids, 4200,
      idx::IndexValidationRepairFamily::vector_generation_state);
  vector.state.vector_generations.generations.push_back(
      VectorGeneration(uuids, 4210));
  auto vector_valid = idx::ExecuteIndexValidationRepairOperation(vector);
  Require(vector_valid.ok() && vector_valid.planner_visible,
          "DPC-046 vector generation validation did not admit published generation");
  auto vector_pending = vector;
  vector_pending.operation = idx::IndexValidationRepairOperation::discard_unpublished;
  vector_pending.policy_allows_mutation = true;
  vector_pending.state.vector_generations.generations = {
      VectorGeneration(uuids, 4220, idx::VectorGenerationState::building)};
  auto vector_discarded =
      idx::ExecuteIndexValidationRepairOperation(vector_pending);
  Require(vector_discarded.ok() && vector_discarded.mutation_applied,
          "DPC-046 vector generation discard did not apply");

  idx::IndexMovementValidationRequest movement;
  movement.operation = idx::IndexMovementOperation::restore;
  movement.family = idx::IndexFamily::btree;
  movement.resource_available = true;
  movement.transaction_finality_proven = true;
  movement.destination_supports_family = true;
  movement.page_authority.expected_index_uuid =
      uuids.Typed(platform::UuidKind::object, 4301);
  movement.page_authority.observed_index_uuid =
      movement.page_authority.expected_index_uuid;
  movement.page_authority.expected_family = idx::IndexFamily::btree;
  movement.page_authority.observed_family = idx::IndexFamily::btree;
  movement.page_authority.expected_resource_epoch = 1;
  movement.page_authority.observed_resource_epoch = 1;
  movement.page_authority.checksum_valid = true;
  movement.page_authority.page_type_supported = true;
  const auto moved = idx::ValidateIndexMovement(movement);
  Require(moved.ok() && (moved.verify_after_move || moved.rebuild_after_move),
          "DPC-046 restore movement verification route did not admit copy equivalence");
}

}  // namespace

int main() {
  Require(!kGateSearchKey.empty(), "DPC-046 gate search key missing");
  ProveValidateSuccessAndFailure();
  ProveRepairRebuildDiscardAndPersistenceEvidence();
  ProveRouteAdmissionAndRefusal();
  ProveSpecializedFamiliesAndMovementCopyEquivalence();
  return EXIT_SUCCESS;
}
