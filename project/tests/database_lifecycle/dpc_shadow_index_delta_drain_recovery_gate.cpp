// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "shadow_index_build_lifecycle.hpp"
#include "shadow_index_delta_drain_recovery.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_SHADOW_INDEX_DELTA_DRAIN_RECOVERY_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, seed);
  Require(generated.ok(), "DPC-041 generated UUID creation failed");
  return generated.value;
}

bool SameUuid(const platform::TypedUuid& left,
              const platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

void RequireDiagnostic(const idx::ShadowIndexDeltaDrainResult& result,
                       std::string_view code,
                       std::string_view message) {
  Require(result.diagnostic.diagnostic_code == code, message);
  Require(result.evidence.diagnostic_code == code,
          "DPC-041 drain evidence diagnostic did not match result");
}

void RequireDiagnostic(const idx::ShadowIndexDeltaDrainRecoveryResult& result,
                       std::string_view code,
                       std::string_view message) {
  Require(result.diagnostic.diagnostic_code == code, message);
  Require(result.evidence.diagnostic_code == code,
          "DPC-041 recovery evidence diagnostic did not match result");
}

void RequireDiagnostic(const idx::ShadowIndexDeltaPublishEligibilityResult& result,
                       std::string_view code,
                       std::string_view message) {
  Require(result.diagnostic.diagnostic_code == code, message);
}

idx::ShadowIndexDeltaDrainRequest DrainRequest(platform::u64 seed) {
  idx::ShadowIndexDeltaDrainRequest request;
  request.build_id = NewUuid(platform::UuidKind::object, seed + 1);
  request.shadow_index_uuid = NewUuid(platform::UuidKind::object, seed + 2);
  request.table_uuid = NewUuid(platform::UuidKind::object, seed + 3);
  request.authoritative_visible_through_local_transaction_id = 1000;
  request.durable_transaction_inventory_authoritative = true;
  request.durable_transaction_horizon_authoritative = true;
  request.engine_mga_inventory_evidence_ref =
      "engine_mga_inventory:shadow_delta:" + std::to_string(seed);
  request.engine_mga_horizon_evidence_ref =
      "engine_mga_horizon:shadow_delta:" + std::to_string(seed);
  request.max_records_to_scan = 32;
  request.max_records_to_apply = 32;
  return request;
}

idx::SecondaryIndexDeltaEntry Delta(const idx::ShadowIndexDeltaDrainRequest& request,
                                    platform::u64 seed,
                                    idx::SecondaryIndexDeltaKind kind,
                                    std::string key,
                                    bool committed,
                                    platform::u64 local_transaction_id) {
  idx::SecondaryIndexDeltaEntry delta;
  delta.delta_id = NewUuid(platform::UuidKind::object, seed + 1);
  delta.index_uuid = request.shadow_index_uuid;
  delta.table_uuid = request.table_uuid;
  delta.row_uuid = NewUuid(platform::UuidKind::row, seed + 2);
  delta.version_uuid = NewUuid(platform::UuidKind::row, seed + 3);
  delta.transaction_uuid = NewUuid(platform::UuidKind::transaction, seed + 4);
  delta.local_transaction_id = local_transaction_id;
  delta.delta_kind = kind;
  delta.key_payload = std::move(key);
  delta.cleanup_horizon_token = "mga_cleanup_horizon:" + std::to_string(seed);
  delta.committed = committed;
  return delta;
}

idx::SecondaryIndexDeltaEntry WrongIndexDelta(
    const idx::ShadowIndexDeltaDrainRequest& request,
    platform::u64 seed) {
  auto delta = Delta(request,
                    seed,
                    idx::SecondaryIndexDeltaKind::insert,
                    "wrong-index",
                    true,
                    80);
  delta.index_uuid = NewUuid(platform::UuidKind::object, seed + 99);
  return delta;
}

idx::SecondaryIndexDeltaEntry WrongTableDelta(
    const idx::ShadowIndexDeltaDrainRequest& request,
    platform::u64 seed) {
  auto delta = Delta(request,
                    seed,
                    idx::SecondaryIndexDeltaKind::insert,
                    "wrong-table",
                    true,
                    81);
  delta.table_uuid = NewUuid(platform::UuidKind::object, seed + 98);
  return delta;
}

idx::SecondaryIndexBaseEntry BaseEntryFromDelta(
    const idx::SecondaryIndexDeltaEntry& delta) {
  idx::SecondaryIndexBaseEntry entry;
  entry.index_uuid = delta.index_uuid;
  entry.table_uuid = delta.table_uuid;
  entry.row_uuid = delta.row_uuid;
  entry.version_uuid = delta.version_uuid;
  entry.key_payload = delta.key_payload;
  entry.committed_local_transaction_id = delta.local_transaction_id;
  entry.deleted = false;
  return entry;
}

idx::ShadowIndexBuildRecord PublishReadyRecord(
    idx::ShadowIndexBuildLedger* lifecycle_ledger,
    const idx::ShadowIndexDeltaDrainRequest& request) {
  idx::ShadowIndexBuildRequest build_request;
  build_request.build_id = request.build_id;
  build_request.shadow_index_uuid = request.shadow_index_uuid;
  build_request.table_uuid = request.table_uuid;
  build_request.index_kind = idx::SecondaryIndexKind::non_unique;
  build_request.engine_mga_inventory_evidence_ref =
      request.engine_mga_inventory_evidence_ref;
  build_request.engine_mga_horizon_evidence_ref =
      request.engine_mga_horizon_evidence_ref;

  auto requested =
      idx::RequestShadowIndexBuild(lifecycle_ledger, build_request);
  Require(requested.ok(), "DPC-041 lifecycle request failed");
  auto record = requested.record;
  Require(idx::StartShadowIndexBuild(lifecycle_ledger, &record).ok(),
          "DPC-041 lifecycle start failed");
  Require(idx::CompleteShadowIndexBuild(lifecycle_ledger, &record).ok(),
          "DPC-041 lifecycle complete failed");

  idx::ShadowIndexValidationRequest validation;
  validation.validation_succeeded = true;
  validation.validation_evidence_ref =
      "validation_evidence:shadow_delta:" +
      std::to_string(request.authoritative_visible_through_local_transaction_id);
  validation.engine_mga_inventory_evidence_present = true;
  Require(idx::ValidateShadowIndexBuild(lifecycle_ledger, &record, validation).ok(),
          "DPC-041 lifecycle validation failed");

  idx::ShadowIndexPublishBarrierRequest barrier;
  barrier.publish_barrier_evidence_ref =
      "publish_barrier:engine_mga:shadow_delta";
  barrier.engine_owned_mga_publish_barrier = true;
  Require(idx::MarkShadowIndexPublishReady(lifecycle_ledger, &record, barrier).ok(),
          "DPC-041 lifecycle publish-ready failed");
  return record;
}

idx::SecondaryIndexDeltaLedger SourceLedger(
    const idx::ShadowIndexDeltaDrainRequest& request) {
  idx::SecondaryIndexDeltaLedger source;
  source.deltas.push_back(Delta(request,
                                4300100,
                                idx::SecondaryIndexDeltaKind::insert,
                                "alpha",
                                true,
                                90));
  source.deltas.push_back(Delta(request,
                                4300200,
                                idx::SecondaryIndexDeltaKind::update_after,
                                "beta",
                                true,
                                91));
  source.deltas.push_back(Delta(request,
                                4300250,
                                idx::SecondaryIndexDeltaKind::delete_row,
                                "delete-old",
                                true,
                                92));
  source.deltas.push_back(Delta(request,
                                4300260,
                                idx::SecondaryIndexDeltaKind::update_before,
                                "update-old",
                                true,
                                93));
  source.deltas.push_back(Delta(request,
                                4300300,
                                idx::SecondaryIndexDeltaKind::insert,
                                "precommit",
                                false,
                                94));
  source.deltas.push_back(Delta(request,
                                4300400,
                                idx::SecondaryIndexDeltaKind::insert,
                                "above-horizon",
                                true,
                                5000));
  source.deltas.push_back(WrongIndexDelta(request, 4300500));
  source.deltas.push_back(WrongTableDelta(request, 4300600));
  return source;
}

void RequireNoExternalAuthority(const idx::ShadowIndexDeltaDrainLedger& ledger) {
  for (const auto& row : ledger.evidence) {
    Require(!row.parser_finality_authority,
            "DPC-041 parser became finality authority");
    Require(!row.client_state_authority,
            "DPC-041 client state became finality authority");
    Require(!row.timestamp_ordering_authority,
            "DPC-041 timestamp ordering became finality authority");
    Require(!row.uuid_ordering_authority,
            "DPC-041 UUID ordering became finality authority");
    Require(!row.event_stream_authority,
            "DPC-041 event stream became finality authority");
  }
}

void ProveCommittedTargetDeltasDrainIdempotently() {
  const auto request = DrainRequest(4300000);
  const auto source = SourceLedger(request);
  idx::ShadowIndexDeltaDrainLedger drain_ledger;
  drain_ledger.shadow_entries.push_back(BaseEntryFromDelta(source.deltas[2]));
  drain_ledger.shadow_entries.push_back(BaseEntryFromDelta(source.deltas[3]));

  auto drained =
      idx::DrainShadowIndexCommittedDeltas(&drain_ledger, source, request);
  Require(drained.ok(), "DPC-041 initial drain refused valid ledger");
  RequireDiagnostic(drained,
                    "shadow_delta_drain_complete",
                    "DPC-041 drain diagnostic changed");
  Require(drained.scanned_delta_count == 6,
          "DPC-041 target scan count changed");
  Require(drained.eligible_committed_delta_count == 4,
          "DPC-041 committed eligible count changed");
  Require(drained.newly_applied_delta_count == 4,
          "DPC-041 newly applied count changed");
  Require(drained.skipped_uncommitted_delta_count == 2,
          "DPC-041 uncommitted or above-horizon skip count changed");
  Require(drained.skipped_wrong_target_delta_count == 2,
          "DPC-041 wrong target skip count changed");
  Require(drain_ledger.applied_deltas.size() == 4,
          "DPC-041 applied delta evidence count changed");
  Require(drain_ledger.shadow_entries.size() == 2,
          "DPC-041 shadow entries did not reflect insert update delete DML");
  Require(drain_ledger.drain_complete,
          "DPC-041 drain did not mark complete evidence");

  auto repeated =
      idx::DrainShadowIndexCommittedDeltas(&drain_ledger, source, request);
  Require(repeated.ok(), "DPC-041 repeated drain refused");
  Require(repeated.newly_applied_delta_count == 0,
          "DPC-041 repeated drain duplicated applied deltas");
  Require(repeated.idempotent_replayed_delta_count == 4,
          "DPC-041 repeated drain did not prove idempotent delta identity");
  Require(drain_ledger.applied_deltas.size() == 4,
          "DPC-041 applied delta evidence duplicated");
  Require(drain_ledger.shadow_entries.size() == 2,
          "DPC-041 shadow entries duplicated");
  RequireNoExternalAuthority(drain_ledger);

  auto non_authoritative = request;
  non_authoritative.durable_transaction_inventory_authoritative = false;
  idx::ShadowIndexDeltaDrainLedger refused_ledger;
  auto refused =
      idx::DrainShadowIndexCommittedDeltas(&refused_ledger,
                                           source,
                                           non_authoritative);
  Require(!refused.ok(), "DPC-041 non-authoritative drain succeeded");
  RequireDiagnostic(refused,
                    "shadow_delta_drain_non_authoritative_mga",
                    "DPC-041 non-authoritative drain diagnostic changed");

  auto external_authority = request;
  external_authority.parser_finality_authority = true;
  idx::ShadowIndexDeltaDrainLedger parser_refused_ledger;
  auto parser_refused =
      idx::DrainShadowIndexCommittedDeltas(&parser_refused_ledger,
                                           source,
                                           external_authority);
  Require(!parser_refused.ok(), "DPC-041 parser authority drain succeeded");
  RequireDiagnostic(parser_refused,
                    "shadow_delta_drain_external_authority_refused",
                    "DPC-041 external authority diagnostic changed");
}

void ProvePublishEligibilityRequiresAuthoritativeDrainEvidence() {
  const auto request = DrainRequest(4310000);
  const auto source = SourceLedger(request);
  idx::ShadowIndexBuildLedger lifecycle_ledger;
  auto record = PublishReadyRecord(&lifecycle_ledger, request);

  auto missing =
      idx::EvaluateShadowIndexDeltaDrainPublishEligibility(record, nullptr);
  Require(!missing.ok(), "DPC-041 missing drain evidence was publish-eligible");
  RequireDiagnostic(missing,
                    "shadow_delta_publish_drain_evidence_missing",
                    "DPC-041 missing drain publish diagnostic changed");

  idx::ShadowIndexDeltaDrainLedger incomplete;
  incomplete.build_id = request.build_id;
  incomplete.shadow_index_uuid = request.shadow_index_uuid;
  incomplete.table_uuid = request.table_uuid;
  incomplete.drain_complete = true;
  auto incomplete_route =
      idx::EvaluateShadowIndexDeltaDrainPublishEligibility(record, &incomplete);
  Require(!incomplete_route.ok(),
          "DPC-041 incomplete drain evidence was publish-eligible");
  RequireDiagnostic(incomplete_route,
                    "shadow_delta_publish_drain_evidence_incomplete",
                    "DPC-041 incomplete drain publish diagnostic changed");

  idx::ShadowIndexDeltaDrainLedger lifecycle_guard_ledger;
  Require(idx::DrainShadowIndexCommittedDeltas(&lifecycle_guard_ledger,
                                               source,
                                               request).ok(),
          "DPC-041 lifecycle guard drain setup failed");
  auto malformed_lifecycle = record;
  malformed_lifecycle.validation_evidence_present = false;
  malformed_lifecycle.validation_evidence_ref.clear();
  auto malformed_lifecycle_route =
      idx::EvaluateShadowIndexDeltaDrainPublishEligibility(
          malformed_lifecycle,
          &lifecycle_guard_ledger);
  Require(!malformed_lifecycle_route.ok(),
          "DPC-041 malformed lifecycle evidence was publish-eligible");
  RequireDiagnostic(malformed_lifecycle_route,
                    "shadow_delta_publish_lifecycle_evidence_incomplete",
                    "DPC-041 malformed lifecycle diagnostic changed");

  auto corrupt_drain_ledger = lifecycle_guard_ledger;
  corrupt_drain_ledger.applied_deltas.front().delta.committed = false;
  auto corrupt_drain_route =
      idx::EvaluateShadowIndexDeltaDrainPublishEligibility(record,
                                                           &corrupt_drain_ledger);
  Require(!corrupt_drain_route.ok(),
          "DPC-041 corrupt applied drain evidence was publish-eligible");
  RequireDiagnostic(corrupt_drain_route,
                    "shadow_delta_publish_drain_evidence_corrupt",
                    "DPC-041 corrupt drain publish diagnostic changed");

  auto direct_publish_record = record;
  Require(idx::PublishShadowIndexBuild(&lifecycle_ledger,
                                       &direct_publish_record).ok(),
          "DPC-040 standalone publish unexpectedly failed");
  auto dpc041_route_without_drain =
      idx::EvaluateShadowIndexDeltaDrainPublishEligibility(direct_publish_record,
                                                           nullptr);
  Require(!dpc041_route_without_drain.ok(),
          "DPC-041 route allowed visible index without drain evidence");

  idx::ShadowIndexDeltaDrainLedger drain_ledger;
  Require(idx::DrainShadowIndexCommittedDeltas(&drain_ledger,
                                               source,
                                               request).ok(),
          "DPC-041 publish drain setup failed");
  auto eligible =
      idx::EvaluateShadowIndexDeltaDrainPublishEligibility(record,
                                                           &drain_ledger);
  Require(eligible.ok(), "DPC-041 valid drain evidence was not publish-eligible");
  RequireDiagnostic(eligible,
                    "shadow_delta_publish_eligible",
                    "DPC-041 publish-eligible diagnostic changed");

  auto published = idx::PublishShadowIndexBuildWithDeltaDrainEvidence(
      &lifecycle_ledger,
      &record,
      &drain_ledger);
  Require(published.ok(), "DPC-041 publish wrapper refused valid drain evidence");
  Require(published.diagnostic.diagnostic_code == "shadow_index_publish_success",
          "DPC-041 lifecycle publish diagnostic changed");
  auto visible =
      idx::EvaluateShadowIndexDeltaDrainPublishEligibility(record,
                                                           &drain_ledger);
  Require(visible.ok(), "DPC-041 published route was not eligible");
  Require(visible.planner_visible && visible.read_visible,
          "DPC-041 published route was not planner/read visible");
  Require(SameUuid(visible.visible_index_uuid, request.shadow_index_uuid),
          "DPC-041 visible index UUID changed");
}

void ProveCrashReopenRecoveryReplaysOrRefuses() {
  const auto request = DrainRequest(4320000);
  const auto source = SourceLedger(request);
  idx::ShadowIndexDeltaDrainLedger drain_ledger;
  Require(idx::DrainShadowIndexCommittedDeltas(&drain_ledger,
                                               source,
                                               request).ok(),
          "DPC-041 recovery drain setup failed");

  idx::ShadowIndexDeltaDrainLedger reopened = drain_ledger;
  reopened.shadow_entries.clear();
  auto classified =
      idx::ClassifyShadowIndexDeltaDrainForRecovery(reopened, request);
  Require(classified.ok(), "DPC-041 recoverable replay classification failed");
  Require(classified.recovery_class ==
              idx::ShadowIndexDeltaDrainRecoveryClass::recoverable_replay,
          "DPC-041 recovery class did not require replay");
  RequireDiagnostic(classified,
                    "shadow_delta_recovery_replay_required",
                    "DPC-041 replay-required diagnostic changed");

  auto recovered = idx::RecoverShadowIndexDeltaDrain(&reopened, request);
  Require(recovered.ok(), "DPC-041 recoverable replay refused");
  RequireDiagnostic(recovered,
                    "shadow_delta_recovery_replayed",
                    "DPC-041 recovery replay diagnostic changed");
  Require(recovered.replayed_delta_count == 2,
          "DPC-041 recovery replay count changed");
  Require(reopened.shadow_entries.size() == 2,
          "DPC-041 recovery did not rebuild shadow entries");

  auto second_recovery = idx::RecoverShadowIndexDeltaDrain(&reopened, request);
  Require(second_recovery.ok(), "DPC-041 second recovery refused clean state");
  RequireDiagnostic(second_recovery,
                    "shadow_delta_recovery_clean_complete",
                    "DPC-041 second recovery diagnostic changed");
  Require(reopened.shadow_entries.size() == 2,
          "DPC-041 second recovery duplicated shadow entries");

  auto corrupt = drain_ledger;
  auto conflicting = corrupt.applied_deltas.front();
  conflicting.delta.key_payload = "conflicting-key";
  corrupt.applied_deltas.push_back(conflicting);
  auto corrupt_result = idx::RecoverShadowIndexDeltaDrain(&corrupt, request);
  Require(!corrupt_result.ok(), "DPC-041 corrupt recovery succeeded");
  RequireDiagnostic(corrupt_result,
                    "shadow_delta_recovery_corrupt_delta_identity",
                    "DPC-041 corrupt recovery diagnostic changed");

  auto incomplete = drain_ledger;
  incomplete.drain_complete = false;
  incomplete.evidence.clear();
  auto incomplete_result =
      idx::RecoverShadowIndexDeltaDrain(&incomplete, request);
  Require(!incomplete_result.ok(), "DPC-041 incomplete recovery succeeded");
  RequireDiagnostic(incomplete_result,
                    "shadow_delta_recovery_incomplete_drain_evidence",
                    "DPC-041 incomplete recovery diagnostic changed");

  auto non_authoritative = request;
  non_authoritative.durable_transaction_horizon_authoritative = false;
  auto non_authoritative_result =
      idx::RecoverShadowIndexDeltaDrain(&drain_ledger, non_authoritative);
  Require(!non_authoritative_result.ok(),
          "DPC-041 non-authoritative recovery succeeded");
  RequireDiagnostic(non_authoritative_result,
                    "shadow_delta_recovery_non_authoritative_mga",
                    "DPC-041 non-authoritative recovery diagnostic changed");
}

}  // namespace

int main() {
  Require(kGateSearchKey == "DPC_SHADOW_INDEX_DELTA_DRAIN_RECOVERY_GATE",
          "DPC-041 gate search key changed");
  Require(idx::kShadowIndexDeltaDrainRecoverySearchKey ==
              std::string_view("DPC_SHADOW_INDEX_DELTA_DRAIN_RECOVERY"),
          "DPC-041 production search key changed");
  ProveCommittedTargetDeltasDrainIdempotently();
  ProvePublishEligibilityRequiresAuthoritativeDrainEvidence();
  ProveCrashReopenRecoveryReplaysOrRefuses();
  return EXIT_SUCCESS;
}
