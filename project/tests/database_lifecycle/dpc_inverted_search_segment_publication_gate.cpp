// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "inverted_search_segment_publication.hpp"
#include "uuid.hpp"

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
    "DPC_INVERTED_SEARCH_SEGMENT_PUBLICATION_GATE";
constexpr std::string_view kImplementationSearchKey =
    "DPC_INVERTED_SEARCH_SEGMENT_PUBLICATION";

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
  Require(generated.ok(), "DPC-043 generated UUID creation failed");
  return generated.value;
}

bool SameUuid(const platform::TypedUuid& left,
              const platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

idx::InvertedSearchSegmentBuildRequest BuildRequest(platform::u64 seed) {
  idx::InvertedSearchSegmentBuildRequest request;
  request.index_uuid = NewUuid(platform::UuidKind::object, seed + 1);
  request.table_uuid = NewUuid(platform::UuidKind::object, seed + 2);
  request.generation = seed;
  request.engine_mga_inventory_evidence_ref =
      "engine_mga_inventory:search_segment:" + std::to_string(seed);
  request.engine_mga_horizon_evidence_ref =
      "engine_mga_horizon:search_segment:" + std::to_string(seed);
  return request;
}

idx::InvertedSearchSegmentDescriptor VisibleSegment(
    idx::InvertedSearchSegmentLedger* ledger,
    idx::InvertedSearchSegmentBuildRequest request,
    platform::u64 seed) {
  auto started = idx::StartInvertedSearchSegmentBuild(ledger, request);
  Require(started.ok(), "DPC-043 segment build request failed");
  auto segment = started.segment;
  Require(segment.segment_uuid.valid(),
          "DPC-043 build did not generate segment UUID");
  Require(!segment.visible, "DPC-043 building segment was visible");

  idx::InvertedSearchSegmentValidationRequest validation;
  validation.validation_succeeded = true;
  validation.validation_evidence_ref =
      "validation:search_segment:" + std::to_string(seed);
  validation.engine_mga_inventory_evidence_present = true;
  validation.complete = true;
  auto validated =
      idx::ValidateInvertedSearchSegmentBuild(ledger, &segment, validation);
  Require(validated.ok(), "DPC-043 segment validation failed");
  Require(!segment.visible, "DPC-043 validated segment was visible");

  idx::InvertedSearchSegmentPublishRequest publish;
  publish.publish_barrier_evidence_ref =
      "publish_barrier:engine_mga:search_segment:" + std::to_string(seed);
  publish.engine_owned_mga_publish_barrier = true;
  auto ready =
      idx::MarkInvertedSearchSegmentPublishReady(ledger, &segment, publish);
  Require(ready.ok(), "DPC-043 publish-ready transition failed");
  Require(!segment.visible, "DPC-043 publish-ready segment was visible");

  const auto generated_segment_uuid = segment.segment_uuid;
  auto published = idx::PublishInvertedSearchSegment(ledger, &segment);
  Require(published.ok(), "DPC-043 segment publish failed");
  Require(segment.visible, "DPC-043 published segment was not visible");
  Require(SameUuid(segment.segment_uuid, generated_segment_uuid),
          "DPC-043 publish did not preserve generated UUID identity");
  Require(segment.authority_source == idx::kInvertedSearchSegmentAuthoritySource,
          "DPC-043 publish did not record engine MGA authority source");
  return segment;
}

idx::InvertedSearchSegmentDescriptor VisibleSegment(
    idx::InvertedSearchSegmentLedger* ledger,
    platform::u64 seed) {
  return VisibleSegment(ledger, BuildRequest(seed), seed);
}

void RequireDiagnostic(const idx::InvertedSearchSegmentAccessPlan& plan,
                       std::string_view diagnostic_code,
                       std::string_view fallback_reason,
                       std::string_view message) {
  if (plan.diagnostic.diagnostic_code != diagnostic_code ||
      plan.fallback_reason != fallback_reason) {
    std::cerr << "diagnostic=" << plan.diagnostic.diagnostic_code
              << " fallback=" << plan.fallback_reason
              << " access=" << plan.selected_access << '\n';
  }
  Require(plan.diagnostic.diagnostic_code == diagnostic_code, message);
  Require(plan.fallback_reason == fallback_reason, message);
}

void ProveInvisibleUntilValidationAndMgaPublish() {
  idx::InvertedSearchSegmentLedger ledger;
  const auto segment = VisibleSegment(&ledger, 4301);

  bool saw_building = false;
  bool saw_validated = false;
  bool saw_ready = false;
  bool saw_visible = false;
  for (const auto& row : ledger.evidence) {
    Require(!row.diagnostic_code.empty(),
            "DPC-043 evidence diagnostic was empty");
    Require(!row.parser_finality_authority &&
                !row.client_state_authority &&
                !row.timestamp_ordering_authority &&
                !row.uuid_ordering_authority &&
                !row.event_stream_authority &&
                !row.reference_authority &&
                !row.write_ahead_authority,
            "DPC-043 evidence accepted external finality authority");
    if (row.state == idx::InvertedSearchSegmentState::building) {
      saw_building = true;
      Require(!row.visible, "DPC-043 building evidence was visible");
    }
    if (row.state == idx::InvertedSearchSegmentState::validated) {
      saw_validated = true;
      Require(row.validation_evidence_present,
              "DPC-043 validated evidence missing validation ref");
      Require(!row.visible, "DPC-043 validated evidence was visible");
    }
    if (row.state == idx::InvertedSearchSegmentState::publish_ready) {
      saw_ready = true;
      Require(row.validation_evidence_present &&
                  row.publish_barrier_evidence_present &&
                  row.publish_barrier_engine_owned_mga,
              "DPC-043 publish-ready evidence missing MGA barrier");
      Require(!row.visible, "DPC-043 publish-ready evidence was visible");
    }
    if (row.state == idx::InvertedSearchSegmentState::visible) {
      saw_visible = true;
      Require(row.visible, "DPC-043 visible evidence was hidden");
      Require(row.authority_source == idx::kInvertedSearchSegmentAuthoritySource,
              "DPC-043 visible evidence authority source changed");
    }
  }
  Require(saw_building && saw_validated && saw_ready && saw_visible,
          "DPC-043 lifecycle evidence did not cover publish states");

  idx::InvertedSearchSegmentAccessRequest access;
  access.segments = {segment};
  const auto plan = idx::PlanInvertedSearchSegmentAccess(access);
  Require(plan.ok(), "DPC-043 visible segment was not selected for search");
  Require(plan.selected_access == "inverted_search_segment_scan",
          "DPC-043 selected access changed");
  Require(plan.visible_segment_uuids.size() == 1 &&
              SameUuid(plan.visible_segment_uuids.front(), segment.segment_uuid),
          "DPC-043 selected segment UUID did not match published segment");
  Require(!plan.segment_metadata_visibility_authority &&
              !plan.segment_metadata_finality_authority,
          "DPC-043 segment metadata became visibility/finality authority");
}

void ProveExactFallbackForUnsafeSegmentStates() {
  idx::InvertedSearchSegmentLedger ledger;
  const auto segment = VisibleSegment(&ledger, 4401);

  idx::InvertedSearchSegmentAccessRequest request;
  request.segments = {segment};

  auto disabled = request;
  disabled.search_segments_enabled = false;
  RequireDiagnostic(idx::PlanInvertedSearchSegmentAccess(disabled),
                    "INDEX.SEARCH_SEGMENT.DISABLED_EXACT_FALLBACK",
                    "disabled_segment_exact_fallback",
                    "DPC-043 disabled segments did not select exact fallback");

  auto missing = request;
  missing.segments.clear();
  RequireDiagnostic(idx::PlanInvertedSearchSegmentAccess(missing),
                    "INDEX.SEARCH_SEGMENT.MISSING_EXACT_FALLBACK",
                    "missing_segment_exact_fallback",
                    "DPC-043 missing segments did not select exact fallback");

  auto stale = request;
  stale.segments.front().stale = true;
  RequireDiagnostic(idx::PlanInvertedSearchSegmentAccess(stale),
                    "INDEX.SEARCH_SEGMENT.STALE_EXACT_FALLBACK",
                    "stale_segment_exact_fallback",
                    "DPC-043 stale segment did not select exact fallback");

  auto corrupt = request;
  corrupt.segments.front().checksum_valid = false;
  RequireDiagnostic(idx::PlanInvertedSearchSegmentAccess(corrupt),
                    "INDEX.SEARCH_SEGMENT.CORRUPT_EXACT_FALLBACK",
                    "corrupt_segment_exact_fallback",
                    "DPC-043 corrupt segment did not select exact fallback");

  auto incomplete = request;
  incomplete.segments.front().state =
      idx::InvertedSearchSegmentState::publish_ready;
  incomplete.segments.front().visible = false;
  incomplete.segments.front().persisted_record_present = false;
  RequireDiagnostic(idx::PlanInvertedSearchSegmentAccess(incomplete),
                    "INDEX.SEARCH_SEGMENT.INCOMPLETE_EXACT_FALLBACK",
                    "incomplete_segment_exact_fallback",
                    "DPC-043 incomplete segment did not select exact fallback");

  auto non_authoritative = request;
  non_authoritative.segments.front().authority_source = "parser_timestamp";
  non_authoritative.segments.front().parser_finality_authority_claimed = true;
  RequireDiagnostic(
      idx::PlanInvertedSearchSegmentAccess(non_authoritative),
      "INDEX.SEARCH_SEGMENT.NON_AUTHORITATIVE_EXACT_FALLBACK",
      "non_authoritative_segment_exact_fallback",
      "DPC-043 non-authoritative segment did not select exact fallback");

  auto no_fallback = corrupt;
  no_fallback.exact_base_table_fallback_available = false;
  const auto refused = idx::PlanInvertedSearchSegmentAccess(no_fallback);
  Require(!refused.status.ok(),
          "DPC-043 unsafe segment without exact fallback did not fail closed");
  Require(refused.diagnostic.diagnostic_code ==
              "INDEX.SEARCH_SEGMENT.UNSAFE_NO_EXACT_FALLBACK",
          "DPC-043 unsafe no-fallback diagnostic changed");
  Require(refused.selected_access == "refused",
          "DPC-043 unsafe no-fallback access was not refused");
}

void ProveMergeRetainsOldSegmentsUntilCommitMarker() {
  idx::InvertedSearchSegmentLedger ledger;
  const auto left = VisibleSegment(&ledger, 4501);
  auto right_request = BuildRequest(4511);
  right_request.index_uuid = left.index_uuid;
  right_request.table_uuid = left.table_uuid;
  const auto right = VisibleSegment(&ledger, right_request, 4511);

  idx::InvertedSearchSegmentMergeRequest merge;
  merge.merge_id = NewUuid(platform::UuidKind::object, 4590);
  merge.merged_segment_uuid = NewUuid(platform::UuidKind::object, 4591);
  merge.index_uuid = left.index_uuid;
  merge.table_uuid = left.table_uuid;
  merge.input_segment_uuids = {left.segment_uuid, right.segment_uuid};
  merge.merged_generation = 99;
  merge.validation_succeeded = true;
  merge.validation_evidence_ref = "validation:search_segment_merge:4590";
  merge.publish_barrier_evidence_ref =
      "publish_barrier:engine_mga:search_segment_merge:4590";
  merge.engine_mga_inventory_evidence_ref =
      "engine_mga_inventory:search_segment_merge:4590";
  merge.engine_mga_horizon_evidence_ref =
      "engine_mga_horizon:search_segment_merge:4590";
  merge.engine_mga_inventory_evidence_present = true;
  merge.engine_owned_mga_publish_barrier = true;

  auto pending = idx::ApplyInvertedSearchSegmentMerge(&ledger, merge);
  Require(!pending.ok(), "DPC-043 pending merge unexpectedly committed");
  Require(!pending.fail_closed,
          "DPC-043 pending merge should retain old visible segments safely");
  Require(pending.old_visible_segments_retained,
          "DPC-043 pending merge did not retain old visible segments");
  Require(pending.evidence.old_visible_segment_count == 2,
          "DPC-043 pending merge did not count old visible segments");
  Require(pending.diagnostic.diagnostic_code ==
              "INDEX.SEARCH_SEGMENT.MERGE_COMMIT_MARKER_PENDING",
          "DPC-043 pending merge diagnostic changed");

  idx::InvertedSearchSegmentAccessRequest pending_access;
  pending_access.segments = {left, right};
  const auto pending_plan =
      idx::PlanInvertedSearchSegmentAccess(pending_access);
  Require(pending_plan.ok(),
          "DPC-043 old visible segments were not searchable before marker");
  Require(pending_plan.visible_segment_uuids.size() == 2,
          "DPC-043 pending merge did not keep both old segments searchable");

  merge.merge_commit_marker_complete = true;
  auto committed = idx::ApplyInvertedSearchSegmentMerge(&ledger, merge);
  Require(committed.ok(), "DPC-043 committed merge failed");
  Require(!committed.old_visible_segments_retained,
          "DPC-043 committed merge still retained old segments as visible");
  Require(committed.retired_input_segment_count == 2,
          "DPC-043 committed merge did not retire both input segments");
  Require(committed.merged_segment.visible,
          "DPC-043 committed merged segment was not visible");
  Require(SameUuid(committed.merged_segment.segment_uuid,
                   merge.merged_segment_uuid),
          "DPC-043 merge did not preserve generated merged segment UUID");
}

void ProveCrashReopenClassification() {
  idx::InvertedSearchSegmentLedger ledger;
  (void)VisibleSegment(&ledger, 4601);

  auto pending = idx::StartInvertedSearchSegmentBuild(&ledger,
                                                      BuildRequest(4611));
  Require(pending.ok(), "DPC-043 pending build setup failed");

  const auto fallback =
      idx::ClassifyInvertedSearchSegmentReopen(ledger, true);
  Require(fallback.ok(),
          "DPC-043 reopen with pending segment and fallback failed closed");
  Require(fallback.recovery_class ==
              idx::InvertedSearchSegmentRecoveryClass::
                  old_visible_segments_or_exact_fallback,
          "DPC-043 reopen fallback recovery class changed");
  Require(fallback.action ==
              idx::InvertedSearchSegmentRecoveryAction::
                  use_exact_base_table_fallback,
          "DPC-043 reopen fallback action changed");
  Require(fallback.diagnostic.diagnostic_code ==
              "INDEX.SEARCH_SEGMENT.RECOVERY_EXACT_FALLBACK",
          "DPC-043 reopen fallback diagnostic changed");

  auto unsafe_ledger = ledger;
  Require(!unsafe_ledger.segments.empty(),
          "DPC-043 unsafe reopen fixture missing segment");
  unsafe_ledger.segments.front().checksum_valid = false;
  unsafe_ledger.segments.front().visible = true;
  const auto refused =
      idx::ClassifyInvertedSearchSegmentReopen(unsafe_ledger, true);
  Require(!refused.ok(), "DPC-043 unsafe visible segment was not refused");
  Require(refused.diagnostic.diagnostic_code ==
              "INDEX.SEARCH_SEGMENT.RECOVERY_UNSAFE_REFUSED",
          "DPC-043 unsafe reopen diagnostic changed");
}

void ProveAuthorityClaimsRejected() {
  auto request = BuildRequest(4701);
  request.parser_finality_authority = true;
  request.client_state_authority = true;
  request.timestamp_ordering_authority = true;
  request.uuid_ordering_authority = true;
  request.event_stream_authority = true;
  request.reference_authority = true;
  request.write_ahead_authority = true;
  idx::InvertedSearchSegmentLedger ledger;
  const auto refused =
      idx::StartInvertedSearchSegmentBuild(&ledger, request);
  Require(!refused.ok(),
          "DPC-043 parser/client/timestamp/UUID/event/reference/write-ahead authority claim was accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "INDEX.SEARCH_SEGMENT.EXTERNAL_AUTHORITY_REJECTED",
          "DPC-043 external authority rejection diagnostic changed");
}

}  // namespace

int main() {
  Require(!kGateSearchKey.empty(), "DPC-043 gate search key missing");
  Require(!kImplementationSearchKey.empty(),
          "DPC-043 implementation search key missing");

  ProveInvisibleUntilValidationAndMgaPublish();
  ProveExactFallbackForUnsafeSegmentStates();
  ProveMergeRetainsOldSegmentsUntilCommitMarker();
  ProveCrashReopenClassification();
  ProveAuthorityClaimsRejected();

  std::cout
      << "DPC_INVERTED_SEARCH_SEGMENT_PUBLICATION_GATE passed\n";
  return EXIT_SUCCESS;
}
