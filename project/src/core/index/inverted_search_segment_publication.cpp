// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// DPC_INVERTED_SEARCH_SEGMENT_PUBLICATION
#include "inverted_search_segment_publication.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status WarnStatus() {
  return {StatusCode::ok, Severity::warning, Subsystem::engine};
}

Status RefuseStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

TypedUuid GeneratedId(UuidKind kind, u64 seed) {
  const auto generated =
      scratchbird::core::uuid::GenerateEngineIdentityV7(kind, seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool GeneratedDurableUuid(const TypedUuid& value, UuidKind expected_kind) {
  return value.kind == expected_kind && value.valid() &&
         scratchbird::core::uuid::IsDurableEngineIdentityKind(value.kind) &&
         scratchbird::core::uuid::IsEngineIdentityUuid(value.value);
}

bool ExternalAuthorityRequested(const InvertedSearchSegmentBuildRequest& request) {
  return request.parser_finality_authority ||
         request.client_state_authority ||
         request.timestamp_ordering_authority ||
         request.uuid_ordering_authority ||
         request.event_stream_authority ||
         request.reference_authority ||
         request.write_ahead_authority;
}

bool SameTarget(const InvertedSearchSegmentDescriptor& segment,
                const TypedUuid& index_uuid,
                const TypedUuid& table_uuid) {
  return SameUuid(segment.index_uuid, index_uuid) &&
         SameUuid(segment.table_uuid, table_uuid);
}

bool HasMgaEvidence(const InvertedSearchSegmentDescriptor& segment) {
  return !segment.engine_mga_inventory_evidence_ref.empty() &&
         !segment.engine_mga_horizon_evidence_ref.empty();
}

bool PublishEvidenceComplete(const InvertedSearchSegmentDescriptor& segment) {
  return segment.validation_evidence_present &&
         !segment.validation_evidence_ref.empty() &&
         segment.publish_barrier_evidence_present &&
         !segment.publish_barrier_evidence_ref.empty() &&
         segment.publish_barrier_engine_owned_mga &&
         HasMgaEvidence(segment);
}

void ApplyVisibility(InvertedSearchSegmentDescriptor* segment) {
  segment->visible =
      segment->state == InvertedSearchSegmentState::visible &&
      segment->persisted_record_present &&
      segment->checksum_valid &&
      segment->complete &&
      !segment->stale &&
      InvertedSearchSegmentDescriptorIdentityValid(*segment) &&
      InvertedSearchSegmentDescriptorAuthorityClean(*segment) &&
      PublishEvidenceComplete(*segment);
}

u64 VisibleSegmentCount(const InvertedSearchSegmentLedger* ledger,
                        const TypedUuid& index_uuid,
                        const TypedUuid& table_uuid) {
  if (ledger == nullptr) {
    return 0;
  }
  u64 count = 0;
  for (const auto& segment : ledger->segments) {
    if (segment.visible && SameTarget(segment, index_uuid, table_uuid)) {
      ++count;
    }
  }
  return count;
}

void UpsertSegment(InvertedSearchSegmentLedger* ledger,
                   const InvertedSearchSegmentDescriptor& segment) {
  if (ledger == nullptr) {
    return;
  }
  for (auto& existing : ledger->segments) {
    if (SameUuid(existing.segment_uuid, segment.segment_uuid)) {
      existing = segment;
      return;
    }
  }
  ledger->segments.push_back(segment);
}

InvertedSearchSegmentEvidenceRow BuildEvidence(
    InvertedSearchSegmentLedger* ledger,
    const InvertedSearchSegmentDescriptor& segment,
    const TypedUuid& merge_id,
    bool old_visible_segments_retained,
    u64 old_visible_segment_count,
    u64 new_visible_segment_count,
    std::string diagnostic_code,
    std::string detail) {
  InvertedSearchSegmentEvidenceRow evidence;
  evidence.sequence = ledger == nullptr ? 0 : ledger->next_evidence_sequence++;
  evidence.evidence_id = GeneratedId(UuidKind::object,
                                     4300000 + evidence.sequence);
  evidence.segment_uuid = segment.segment_uuid;
  evidence.index_uuid = segment.index_uuid;
  evidence.table_uuid = segment.table_uuid;
  evidence.merge_id = merge_id;
  evidence.state = segment.state;
  evidence.generation = segment.generation;
  evidence.visible = segment.visible;
  evidence.validation_evidence_present = segment.validation_evidence_present;
  evidence.publish_barrier_evidence_present =
      segment.publish_barrier_evidence_present;
  evidence.publish_barrier_engine_owned_mga =
      segment.publish_barrier_engine_owned_mga;
  evidence.old_visible_segments_retained = old_visible_segments_retained;
  evidence.old_visible_segment_count = old_visible_segment_count;
  evidence.new_visible_segment_count = new_visible_segment_count;
  evidence.authority_source = segment.authority_source;
  evidence.parser_finality_authority = false;
  evidence.client_state_authority = false;
  evidence.timestamp_ordering_authority = false;
  evidence.uuid_ordering_authority = false;
  evidence.event_stream_authority = false;
  evidence.reference_authority = false;
  evidence.write_ahead_authority = false;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.diagnostic_detail = std::move(detail);
  return evidence;
}

InvertedSearchSegmentLifecycleResult FinishLifecycle(
    InvertedSearchSegmentLedger* ledger,
    const InvertedSearchSegmentDescriptor& segment,
    Status status,
    bool accepted,
    bool fail_closed,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  InvertedSearchSegmentLifecycleResult result;
  result.status = status;
  result.accepted = accepted;
  result.fail_closed = fail_closed;
  result.segment = segment;
  result.evidence = BuildEvidence(ledger,
                                  segment,
                                  TypedUuid{},
                                  true,
                                  VisibleSegmentCount(ledger,
                                                      segment.index_uuid,
                                                      segment.table_uuid),
                                  segment.visible ? 1 : 0,
                                  diagnostic_code,
                                  detail);
  result.diagnostic = MakeInvertedSearchSegmentDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  if (ledger != nullptr) {
    ledger->evidence.push_back(result.evidence);
    if (!fail_closed) {
      UpsertSegment(ledger, segment);
      ++ledger->ledger_generation;
    }
  }
  return result;
}

InvertedSearchSegmentLifecycleResult RefuseLifecycle(
    InvertedSearchSegmentLedger* ledger,
    InvertedSearchSegmentDescriptor segment,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  segment.state = InvertedSearchSegmentState::refused;
  segment.visible = false;
  return FinishLifecycle(ledger,
                         segment,
                         RefuseStatus(),
                         false,
                         true,
                         std::move(diagnostic_code),
                         std::move(message_key),
                         std::move(detail));
}

InvertedSearchSegmentAccessPlan Fallback(
    InvertedSearchSegmentFallbackReason reason,
    bool fallback_available,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  InvertedSearchSegmentAccessPlan plan;
  plan.status = fallback_available ? WarnStatus() : RefuseStatus();
  plan.selected_category = IndexPlanCategory::fallback_full_scan;
  plan.selected_access =
      fallback_available ? "exact_base_table_scan" : "refused";
  plan.fallback_reason = InvertedSearchSegmentFallbackReasonName(reason);
  plan.segment_search_selected = false;
  plan.exact_base_table_fallback_selected = fallback_available;
  plan.exact_base_table_fallback_available = fallback_available;
  plan.base_row_mga_recheck_required = true;
  plan.base_row_security_recheck_required = true;
  plan.segment_metadata_visibility_authority = false;
  plan.segment_metadata_finality_authority = false;
  plan.diagnostic = MakeInvertedSearchSegmentDiagnostic(
      plan.status,
      fallback_available ? std::move(diagnostic_code)
                         : "INDEX.SEARCH_SEGMENT.UNSAFE_NO_EXACT_FALLBACK",
      fallback_available ? std::move(message_key)
                         : "index.search_segment.unsafe_no_exact_fallback",
      fallback_available ? std::move(detail) : std::move(diagnostic_code));
  plan.actions.push_back(fallback_available
                             ? "select_exact_base_table_search_fallback"
                             : "refuse_unsafe_search_segment_without_exact_fallback");
  plan.actions.push_back(
      "do_not_use_search_segment_as_visibility_or_finality_authority");
  return plan;
}

bool SegmentUuidListed(const std::vector<TypedUuid>& uuids,
                       const TypedUuid& segment_uuid) {
  return std::any_of(uuids.begin(), uuids.end(), [&](const TypedUuid& value) {
    return SameUuid(value, segment_uuid);
  });
}

}  // namespace

const char* InvertedSearchSegmentStateName(InvertedSearchSegmentState state) {
  switch (state) {
    case InvertedSearchSegmentState::building: return "building";
    case InvertedSearchSegmentState::validated: return "validated";
    case InvertedSearchSegmentState::publish_ready: return "publish_ready";
    case InvertedSearchSegmentState::visible: return "visible";
    case InvertedSearchSegmentState::retired: return "retired";
    case InvertedSearchSegmentState::refused: return "refused";
  }
  return "refused";
}

const char* InvertedSearchSegmentFallbackReasonName(
    InvertedSearchSegmentFallbackReason reason) {
  switch (reason) {
    case InvertedSearchSegmentFallbackReason::none: return "none";
    case InvertedSearchSegmentFallbackReason::disabled_segment_exact_fallback:
      return "disabled_segment_exact_fallback";
    case InvertedSearchSegmentFallbackReason::missing_segment_exact_fallback:
      return "missing_segment_exact_fallback";
    case InvertedSearchSegmentFallbackReason::stale_segment_exact_fallback:
      return "stale_segment_exact_fallback";
    case InvertedSearchSegmentFallbackReason::corrupt_segment_exact_fallback:
      return "corrupt_segment_exact_fallback";
    case InvertedSearchSegmentFallbackReason::incomplete_segment_exact_fallback:
      return "incomplete_segment_exact_fallback";
    case InvertedSearchSegmentFallbackReason::
        non_authoritative_segment_exact_fallback:
      return "non_authoritative_segment_exact_fallback";
    case InvertedSearchSegmentFallbackReason::invalid_identity_exact_fallback:
      return "invalid_identity_exact_fallback";
  }
  return "none";
}

const char* InvertedSearchSegmentRecoveryClassName(
    InvertedSearchSegmentRecoveryClass recovery_class) {
  switch (recovery_class) {
    case InvertedSearchSegmentRecoveryClass::clean_visible_segments:
      return "clean_visible_segments";
    case InvertedSearchSegmentRecoveryClass::
        old_visible_segments_or_exact_fallback:
      return "old_visible_segments_or_exact_fallback";
    case InvertedSearchSegmentRecoveryClass::unsafe_segment_refused:
      return "unsafe_segment_refused";
  }
  return "unsafe_segment_refused";
}

const char* InvertedSearchSegmentRecoveryActionName(
    InvertedSearchSegmentRecoveryAction action) {
  switch (action) {
    case InvertedSearchSegmentRecoveryAction::keep_visible_segments:
      return "keep_visible_segments";
    case InvertedSearchSegmentRecoveryAction::use_exact_base_table_fallback:
      return "use_exact_base_table_fallback";
    case InvertedSearchSegmentRecoveryAction::refuse_unsafe_segment_use:
      return "refuse_unsafe_segment_use";
  }
  return "refuse_unsafe_segment_use";
}

bool InvertedSearchSegmentDescriptorIdentityValid(
    const InvertedSearchSegmentDescriptor& segment) {
  return GeneratedDurableUuid(segment.segment_uuid, UuidKind::object) &&
         GeneratedDurableUuid(segment.index_uuid, UuidKind::object) &&
         GeneratedDurableUuid(segment.table_uuid, UuidKind::object);
}

bool InvertedSearchSegmentDescriptorAuthorityClean(
    const InvertedSearchSegmentDescriptor& segment) {
  return segment.authority_source == kInvertedSearchSegmentAuthoritySource &&
         !segment.parser_finality_authority_claimed &&
         !segment.client_finality_authority_claimed &&
         !segment.timestamp_finality_authority_claimed &&
         !segment.uuid_ordering_finality_authority_claimed &&
         !segment.event_stream_finality_authority_claimed &&
         !segment.reference_finality_authority_claimed &&
         !segment.write_ahead_log_finality_authority_claimed;
}

bool InvertedSearchSegmentDescriptorUsable(
    const InvertedSearchSegmentDescriptor& segment) {
  return segment.visible &&
         segment.state == InvertedSearchSegmentState::visible &&
         segment.persisted_record_present &&
         segment.checksum_valid &&
         segment.complete &&
         !segment.stale &&
         segment.generation != 0 &&
         InvertedSearchSegmentDescriptorIdentityValid(segment) &&
         InvertedSearchSegmentDescriptorAuthorityClean(segment) &&
         PublishEvidenceComplete(segment);
}

InvertedSearchSegmentLifecycleResult StartInvertedSearchSegmentBuild(
    InvertedSearchSegmentLedger* ledger,
    const InvertedSearchSegmentBuildRequest& request) {
  InvertedSearchSegmentDescriptor segment;
  segment.segment_uuid = request.segment_uuid.valid()
                             ? request.segment_uuid
                             : GeneratedId(UuidKind::object,
                                           4310000 +
                                               (ledger == nullptr
                                                    ? 1
                                                    : ledger->next_evidence_sequence));
  segment.index_uuid = request.index_uuid;
  segment.table_uuid = request.table_uuid;
  segment.generation = request.generation;
  segment.engine_mga_inventory_evidence_ref =
      request.engine_mga_inventory_evidence_ref;
  segment.engine_mga_horizon_evidence_ref =
      request.engine_mga_horizon_evidence_ref;
  segment.state = InvertedSearchSegmentState::building;
  segment.visible = false;

  if (!InvertedSearchSegmentDescriptorIdentityValid(segment)) {
    return RefuseLifecycle(
        ledger,
        segment,
        "INDEX.SEARCH_SEGMENT.INVALID_IDENTITY",
        "index.search_segment.invalid_identity",
        "segment index and table UUIDs must be generated durable object UUIDs");
  }
  if (segment.generation == 0) {
    return RefuseLifecycle(ledger,
                           segment,
                           "INDEX.SEARCH_SEGMENT.INVALID_GENERATION",
                           "index.search_segment.invalid_generation",
                           "segment generation must be nonzero");
  }
  if (!HasMgaEvidence(segment)) {
    return RefuseLifecycle(
        ledger,
        segment,
        "INDEX.SEARCH_SEGMENT.MGA_EVIDENCE_MISSING",
        "index.search_segment.mga_evidence_missing",
        "segment build requires engine MGA inventory and horizon evidence");
  }
  if (ExternalAuthorityRequested(request)) {
    return RefuseLifecycle(
        ledger,
        segment,
        "INDEX.SEARCH_SEGMENT.EXTERNAL_AUTHORITY_REJECTED",
        "index.search_segment.external_authority_rejected",
        "parser client timestamp UUID ordering event stream reference and write-ahead authority claims are forbidden");
  }

  return FinishLifecycle(ledger,
                         segment,
                         OkStatus(),
                         true,
                         false,
                         "INDEX.SEARCH_SEGMENT.BUILDING_UNPUBLISHED",
                         "index.search_segment.building_unpublished",
                         {});
}

InvertedSearchSegmentLifecycleResult ValidateInvertedSearchSegmentBuild(
    InvertedSearchSegmentLedger* ledger,
    InvertedSearchSegmentDescriptor* segment,
    const InvertedSearchSegmentValidationRequest& request) {
  if (segment == nullptr) {
    return RefuseLifecycle(ledger,
                           InvertedSearchSegmentDescriptor{},
                           "INDEX.SEARCH_SEGMENT.MISSING_RECORD",
                           "index.search_segment.missing_record",
                           "segment record is required");
  }
  if (segment->state != InvertedSearchSegmentState::building) {
    return RefuseLifecycle(ledger,
                           *segment,
                           "INDEX.SEARCH_SEGMENT.INVALID_TRANSITION",
                           "index.search_segment.invalid_transition",
                           InvertedSearchSegmentStateName(segment->state));
  }
  if (!request.validation_succeeded ||
      request.validation_evidence_ref.empty() ||
      !request.engine_mga_inventory_evidence_present ||
      !request.complete) {
    return RefuseLifecycle(
        ledger,
        *segment,
        "INDEX.SEARCH_SEGMENT.VALIDATION_REFUSED",
        "index.search_segment.validation_refused",
        "validation requires success evidence complete postings and engine MGA inventory evidence");
  }

  segment->state = InvertedSearchSegmentState::validated;
  segment->validation_evidence_present = true;
  segment->validation_evidence_ref = request.validation_evidence_ref;
  segment->complete = true;
  ApplyVisibility(segment);
  return FinishLifecycle(ledger,
                         *segment,
                         OkStatus(),
                         true,
                         false,
                         "INDEX.SEARCH_SEGMENT.VALIDATED_UNPUBLISHED",
                         "index.search_segment.validated_unpublished",
                         {});
}

InvertedSearchSegmentLifecycleResult MarkInvertedSearchSegmentPublishReady(
    InvertedSearchSegmentLedger* ledger,
    InvertedSearchSegmentDescriptor* segment,
    const InvertedSearchSegmentPublishRequest& request) {
  if (segment == nullptr) {
    return RefuseLifecycle(ledger,
                           InvertedSearchSegmentDescriptor{},
                           "INDEX.SEARCH_SEGMENT.MISSING_RECORD",
                           "index.search_segment.missing_record",
                           "segment record is required");
  }
  if (segment->state != InvertedSearchSegmentState::validated) {
    return RefuseLifecycle(ledger,
                           *segment,
                           "INDEX.SEARCH_SEGMENT.INVALID_TRANSITION",
                           "index.search_segment.invalid_transition",
                           InvertedSearchSegmentStateName(segment->state));
  }
  if (!segment->validation_evidence_present ||
      segment->validation_evidence_ref.empty()) {
    return RefuseLifecycle(
        ledger,
        *segment,
        "INDEX.SEARCH_SEGMENT.PUBLISH_VALIDATION_MISSING",
        "index.search_segment.publish_validation_missing",
        "publish-ready transition requires segment validation evidence");
  }
  if (request.publish_barrier_evidence_ref.empty() ||
      !request.engine_owned_mga_publish_barrier ||
      request.authority_source != kInvertedSearchSegmentAuthoritySource) {
    return RefuseLifecycle(
        ledger,
        *segment,
        "INDEX.SEARCH_SEGMENT.PUBLISH_BARRIER_REFUSED",
        "index.search_segment.publish_barrier_refused",
        "publish-ready transition requires engine-owned MGA barrier evidence");
  }

  segment->state = InvertedSearchSegmentState::publish_ready;
  segment->publish_barrier_evidence_present = true;
  segment->publish_barrier_engine_owned_mga = true;
  segment->publish_barrier_evidence_ref = request.publish_barrier_evidence_ref;
  segment->authority_source = request.authority_source;
  ApplyVisibility(segment);
  return FinishLifecycle(ledger,
                         *segment,
                         OkStatus(),
                         true,
                         false,
                         "INDEX.SEARCH_SEGMENT.PUBLISH_READY_UNPUBLISHED",
                         "index.search_segment.publish_ready_unpublished",
                         {});
}

InvertedSearchSegmentLifecycleResult PublishInvertedSearchSegment(
    InvertedSearchSegmentLedger* ledger,
    InvertedSearchSegmentDescriptor* segment) {
  if (segment == nullptr) {
    return RefuseLifecycle(ledger,
                           InvertedSearchSegmentDescriptor{},
                           "INDEX.SEARCH_SEGMENT.MISSING_RECORD",
                           "index.search_segment.missing_record",
                           "segment record is required");
  }
  if (segment->state != InvertedSearchSegmentState::publish_ready) {
    return RefuseLifecycle(ledger,
                           *segment,
                           "INDEX.SEARCH_SEGMENT.PUBLISH_NOT_READY",
                           "index.search_segment.publish_not_ready",
                           InvertedSearchSegmentStateName(segment->state));
  }
  if (!PublishEvidenceComplete(*segment)) {
    return RefuseLifecycle(
        ledger,
        *segment,
        "INDEX.SEARCH_SEGMENT.PUBLISH_EVIDENCE_MISSING",
        "index.search_segment.publish_evidence_missing",
        "publish requires validation evidence and engine-owned MGA barrier evidence");
  }
  segment->state = InvertedSearchSegmentState::visible;
  segment->persisted_record_present = true;
  ApplyVisibility(segment);
  return FinishLifecycle(ledger,
                         *segment,
                         OkStatus(),
                         true,
                         false,
                         "INDEX.SEARCH_SEGMENT.PUBLISH_SUCCESS",
                         "index.search_segment.publish_success",
                         {});
}

InvertedSearchSegmentMergeResult ApplyInvertedSearchSegmentMerge(
    InvertedSearchSegmentLedger* ledger,
    const InvertedSearchSegmentMergeRequest& request) {
  InvertedSearchSegmentMergeResult result;
  InvertedSearchSegmentDescriptor merged;
  merged.segment_uuid = request.merged_segment_uuid;
  merged.index_uuid = request.index_uuid;
  merged.table_uuid = request.table_uuid;
  merged.generation = request.merged_generation;
  merged.validation_evidence_present = request.validation_succeeded &&
                                       !request.validation_evidence_ref.empty();
  merged.validation_evidence_ref = request.validation_evidence_ref;
  merged.publish_barrier_evidence_present =
      request.engine_owned_mga_publish_barrier &&
      !request.publish_barrier_evidence_ref.empty();
  merged.publish_barrier_engine_owned_mga =
      request.engine_owned_mga_publish_barrier;
  merged.publish_barrier_evidence_ref = request.publish_barrier_evidence_ref;
  merged.engine_mga_inventory_evidence_ref =
      request.engine_mga_inventory_evidence_ref;
  merged.engine_mga_horizon_evidence_ref =
      request.engine_mga_horizon_evidence_ref;
  merged.authority_source = request.authority_source;
  merged.complete = request.validation_succeeded;
  merged.checksum_valid = true;

  const auto old_visible_count =
      VisibleSegmentCount(ledger, request.index_uuid, request.table_uuid);
  const auto finish = [&](Status status,
                          bool committed,
                          bool fail_closed,
                          bool old_retained,
                          std::string diagnostic_code,
                          std::string message_key,
                          std::string detail) {
    result.status = status;
    result.merge_committed = committed;
    result.old_visible_segments_retained = old_retained;
    result.fail_closed = fail_closed;
    result.merged_segment = merged;
    result.evidence = BuildEvidence(ledger,
                                    merged,
                                    request.merge_id,
                                    old_retained,
                                    old_visible_count,
                                    committed ? 1 : 0,
                                    diagnostic_code,
                                    detail);
    result.diagnostic = MakeInvertedSearchSegmentDiagnostic(
        result.status,
        std::move(diagnostic_code),
        std::move(message_key),
        std::move(detail));
    if (ledger != nullptr) {
      ledger->evidence.push_back(result.evidence);
      if (!fail_closed) {
        UpsertSegment(ledger, merged);
        ++ledger->ledger_generation;
      }
    }
    return result;
  };

  if (ledger == nullptr) {
    return finish(RefuseStatus(),
                  false,
                  true,
                  true,
                  "INDEX.SEARCH_SEGMENT.MERGE_LEDGER_MISSING",
                  "index.search_segment.merge_ledger_missing",
                  "merge requires a segment ledger");
  }
  if (!GeneratedDurableUuid(request.merge_id, UuidKind::object) ||
      !InvertedSearchSegmentDescriptorIdentityValid(merged) ||
      request.input_segment_uuids.empty() ||
      request.merged_generation == 0) {
    return finish(RefuseStatus(),
                  false,
                  true,
                  true,
                  "INDEX.SEARCH_SEGMENT.MERGE_INVALID_IDENTITY",
                  "index.search_segment.merge_invalid_identity",
                  "merge and segment identities must be generated durable UUIDs");
  }

  for (const auto& input_uuid : request.input_segment_uuids) {
    const auto found = std::find_if(
        ledger->segments.begin(), ledger->segments.end(),
        [&](const InvertedSearchSegmentDescriptor& segment) {
          return SameUuid(segment.segment_uuid, input_uuid) &&
                 SameTarget(segment, request.index_uuid, request.table_uuid);
        });
    if (found == ledger->segments.end() ||
        !InvertedSearchSegmentDescriptorUsable(*found)) {
      return finish(RefuseStatus(),
                    false,
                    true,
                    true,
                    "INDEX.SEARCH_SEGMENT.MERGE_INPUT_UNSAFE",
                    "index.search_segment.merge_input_unsafe",
                    "merge inputs must be visible authoritative segments");
    }
  }

  if (!request.validation_succeeded ||
      request.validation_evidence_ref.empty() ||
      !request.engine_mga_inventory_evidence_present ||
      !request.engine_owned_mga_publish_barrier ||
      request.publish_barrier_evidence_ref.empty() ||
      request.engine_mga_inventory_evidence_ref.empty() ||
      request.engine_mga_horizon_evidence_ref.empty() ||
      request.authority_source != kInvertedSearchSegmentAuthoritySource) {
    return finish(RefuseStatus(),
                  false,
                  true,
                  true,
                  "INDEX.SEARCH_SEGMENT.MERGE_VALIDATION_REFUSED",
                  "index.search_segment.merge_validation_refused",
                  "merge requires validation evidence and engine MGA barrier evidence");
  }

  if (!request.merge_commit_marker_complete) {
    merged.state = InvertedSearchSegmentState::publish_ready;
    merged.persisted_record_present = true;
    ApplyVisibility(&merged);
    return finish(WarnStatus(),
                  false,
                  false,
                  true,
                  "INDEX.SEARCH_SEGMENT.MERGE_COMMIT_MARKER_PENDING",
                  "index.search_segment.merge_commit_marker_pending",
                  "old visible segments remain searchable until merge commit marker is complete");
  }

  for (auto& segment : ledger->segments) {
    if (SameTarget(segment, request.index_uuid, request.table_uuid) &&
        SegmentUuidListed(request.input_segment_uuids, segment.segment_uuid)) {
      segment.state = InvertedSearchSegmentState::retired;
      segment.visible = false;
      ++result.retired_input_segment_count;
    }
  }
  merged.state = InvertedSearchSegmentState::visible;
  merged.persisted_record_present = true;
  ApplyVisibility(&merged);
  return finish(OkStatus(),
                true,
                false,
                false,
                "INDEX.SEARCH_SEGMENT.MERGE_PUBLISH_SUCCESS",
                "index.search_segment.merge_publish_success",
                {});
}

InvertedSearchSegmentAccessPlan PlanInvertedSearchSegmentAccess(
    const InvertedSearchSegmentAccessRequest& request) {
  if (!request.search_segments_enabled) {
    return Fallback(
        InvertedSearchSegmentFallbackReason::disabled_segment_exact_fallback,
        request.exact_base_table_fallback_available,
        "INDEX.SEARCH_SEGMENT.DISABLED_EXACT_FALLBACK",
        "index.search_segment.disabled_exact_fallback");
  }
  if (!request.base_row_mga_recheck_required ||
      !request.base_row_security_recheck_required) {
    return Fallback(
        InvertedSearchSegmentFallbackReason::
            non_authoritative_segment_exact_fallback,
        request.exact_base_table_fallback_available,
        "INDEX.SEARCH_SEGMENT.BASE_ROW_RECHECK_REQUIRED",
        "index.search_segment.base_row_recheck_required");
  }
  if (request.segments.empty()) {
    return Fallback(
        InvertedSearchSegmentFallbackReason::missing_segment_exact_fallback,
        request.exact_base_table_fallback_available,
        "INDEX.SEARCH_SEGMENT.MISSING_EXACT_FALLBACK",
        "index.search_segment.missing_exact_fallback");
  }

  InvertedSearchSegmentAccessPlan plan;
  plan.status = OkStatus();
  plan.selected_category = IndexPlanCategory::inverted_search;
  plan.selected_access = "inverted_search_segment_scan";
  plan.fallback_reason =
      InvertedSearchSegmentFallbackReasonName(
          InvertedSearchSegmentFallbackReason::none);
  plan.segment_search_selected = true;
  plan.exact_base_table_fallback_selected = false;
  plan.exact_base_table_fallback_available =
      request.exact_base_table_fallback_available;
  plan.base_row_mga_recheck_required = true;
  plan.base_row_security_recheck_required = true;

  for (const auto& segment : request.segments) {
    if (!InvertedSearchSegmentDescriptorIdentityValid(segment)) {
      return Fallback(
          InvertedSearchSegmentFallbackReason::invalid_identity_exact_fallback,
          request.exact_base_table_fallback_available,
          "INDEX.SEARCH_SEGMENT.INVALID_IDENTITY_EXACT_FALLBACK",
          "index.search_segment.invalid_identity_exact_fallback");
    }
    if (!InvertedSearchSegmentDescriptorAuthorityClean(segment)) {
      return Fallback(
          InvertedSearchSegmentFallbackReason::
              non_authoritative_segment_exact_fallback,
          request.exact_base_table_fallback_available,
          "INDEX.SEARCH_SEGMENT.NON_AUTHORITATIVE_EXACT_FALLBACK",
          "index.search_segment.non_authoritative_exact_fallback",
          segment.authority_source);
    }
    if (!segment.persisted_record_present ||
        segment.state == InvertedSearchSegmentState::building ||
        segment.state == InvertedSearchSegmentState::validated ||
        segment.state == InvertedSearchSegmentState::publish_ready) {
      return Fallback(
          InvertedSearchSegmentFallbackReason::incomplete_segment_exact_fallback,
          request.exact_base_table_fallback_available,
          "INDEX.SEARCH_SEGMENT.INCOMPLETE_EXACT_FALLBACK",
          "index.search_segment.incomplete_exact_fallback",
          InvertedSearchSegmentStateName(segment.state));
    }
    if (!segment.checksum_valid ||
        segment.state == InvertedSearchSegmentState::refused ||
        segment.generation == 0) {
      return Fallback(
          InvertedSearchSegmentFallbackReason::corrupt_segment_exact_fallback,
          request.exact_base_table_fallback_available,
          "INDEX.SEARCH_SEGMENT.CORRUPT_EXACT_FALLBACK",
          "index.search_segment.corrupt_exact_fallback");
    }
    if (segment.stale || segment.state == InvertedSearchSegmentState::retired) {
      return Fallback(
          InvertedSearchSegmentFallbackReason::stale_segment_exact_fallback,
          request.exact_base_table_fallback_available,
          "INDEX.SEARCH_SEGMENT.STALE_EXACT_FALLBACK",
          "index.search_segment.stale_exact_fallback");
    }
    if (!InvertedSearchSegmentDescriptorUsable(segment)) {
      return Fallback(
          InvertedSearchSegmentFallbackReason::incomplete_segment_exact_fallback,
          request.exact_base_table_fallback_available,
          "INDEX.SEARCH_SEGMENT.INCOMPLETE_EXACT_FALLBACK",
          "index.search_segment.incomplete_exact_fallback");
    }
    plan.visible_segment_uuids.push_back(segment.segment_uuid);
  }

  plan.diagnostic = MakeInvertedSearchSegmentDiagnostic(
      plan.status,
      "INDEX.SEARCH_SEGMENT.SELECTED",
      "index.search_segment.selected");
  plan.actions.push_back("use_visible_authoritative_search_segments");
  plan.actions.push_back("apply_base_row_mga_recheck");
  plan.actions.push_back("apply_base_row_security_recheck");
  plan.actions.push_back(
      "do_not_use_search_segment_as_visibility_or_finality_authority");
  return plan;
}

InvertedSearchSegmentRecoveryResult ClassifyInvertedSearchSegmentReopen(
    const InvertedSearchSegmentLedger& ledger,
    bool exact_base_table_fallback_available) {
  InvertedSearchSegmentRecoveryResult result;
  bool saw_visible_usable = false;
  bool saw_incomplete_or_pending = false;
  bool saw_unsafe_visible = false;
  for (const auto& segment : ledger.segments) {
    if (segment.visible && InvertedSearchSegmentDescriptorUsable(segment)) {
      saw_visible_usable = true;
      continue;
    }
    if (segment.visible && !InvertedSearchSegmentDescriptorUsable(segment)) {
      saw_unsafe_visible = true;
    }
    if (segment.state == InvertedSearchSegmentState::building ||
        segment.state == InvertedSearchSegmentState::validated ||
        segment.state == InvertedSearchSegmentState::publish_ready) {
      saw_incomplete_or_pending = true;
    }
  }

  if (saw_unsafe_visible) {
    result.status = RefuseStatus();
    result.fail_closed = true;
    result.recovery_class =
        InvertedSearchSegmentRecoveryClass::unsafe_segment_refused;
    result.action =
        InvertedSearchSegmentRecoveryAction::refuse_unsafe_segment_use;
    result.stable_reason = "visible segment failed identity authority or MGA evidence validation";
    result.diagnostic = MakeInvertedSearchSegmentDiagnostic(
        result.status,
        "INDEX.SEARCH_SEGMENT.RECOVERY_UNSAFE_REFUSED",
        "index.search_segment.recovery_unsafe_refused",
        result.stable_reason);
    return result;
  }
  if (saw_visible_usable && !saw_incomplete_or_pending) {
    result.status = OkStatus();
    result.fail_closed = false;
    result.recovery_class =
        InvertedSearchSegmentRecoveryClass::clean_visible_segments;
    result.action = InvertedSearchSegmentRecoveryAction::keep_visible_segments;
    result.stable_reason = "visible segments retain authoritative evidence";
    result.diagnostic = MakeInvertedSearchSegmentDiagnostic(
        result.status,
        "INDEX.SEARCH_SEGMENT.RECOVERY_KEEP_VISIBLE",
        "index.search_segment.recovery_keep_visible",
        result.stable_reason);
    return result;
  }
  result.status = exact_base_table_fallback_available ? WarnStatus()
                                                      : RefuseStatus();
  result.fail_closed = !exact_base_table_fallback_available;
  result.recovery_class =
      exact_base_table_fallback_available
          ? InvertedSearchSegmentRecoveryClass::
                old_visible_segments_or_exact_fallback
          : InvertedSearchSegmentRecoveryClass::unsafe_segment_refused;
  result.action =
      exact_base_table_fallback_available
          ? InvertedSearchSegmentRecoveryAction::use_exact_base_table_fallback
          : InvertedSearchSegmentRecoveryAction::refuse_unsafe_segment_use;
  result.stable_reason =
      exact_base_table_fallback_available
          ? "incomplete or pending search segment state ignored in favor of old visible segments or exact fallback"
          : "incomplete search segment state cannot be used without exact fallback";
  result.diagnostic = MakeInvertedSearchSegmentDiagnostic(
      result.status,
      exact_base_table_fallback_available
          ? "INDEX.SEARCH_SEGMENT.RECOVERY_EXACT_FALLBACK"
          : "INDEX.SEARCH_SEGMENT.RECOVERY_UNSAFE_REFUSED",
      exact_base_table_fallback_available
          ? "index.search_segment.recovery_exact_fallback"
          : "index.search_segment.recovery_unsafe_refused",
      result.stable_reason);
  return result;
}

DiagnosticRecord MakeInvertedSearchSegmentDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.inverted_search_segment_publication");
}

}  // namespace scratchbird::core::index
