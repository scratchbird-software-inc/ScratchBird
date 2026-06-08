// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_SHADOW_INDEX_BUILD_LIFECYCLE
#include "secondary_index_delta_overlay.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kShadowIndexBuildLifecycleSearchKey =
    "DPC_SHADOW_INDEX_BUILD_LIFECYCLE";

enum class ShadowIndexBuildState : u32 {
  requested = 1,
  building = 2,
  built = 3,
  validated = 4,
  publish_ready = 5,
  published = 6,
  cancelled = 7,
  refused = 8
};

enum class ShadowIndexBuildDecision : u32 {
  accepted = 1,
  cancelled = 2,
  refused = 3
};

struct ShadowIndexBuildEvidenceRow {
  u64 sequence = 0;
  TypedUuid evidence_id;
  TypedUuid build_id;
  TypedUuid shadow_index_uuid;
  TypedUuid table_uuid;
  ShadowIndexBuildState state = ShadowIndexBuildState::requested;
  bool planner_visible = false;
  bool read_visible = false;
  bool validation_evidence_present = false;
  bool publish_barrier_evidence_present = false;
  bool publish_barrier_engine_owned_mga = false;
  bool parser_finality_authority = false;
  bool client_state_authority = false;
  bool timestamp_ordering_authority = false;
  bool uuid_ordering_authority = false;
  bool event_stream_authority = false;
  std::string validation_evidence_ref;
  std::string publish_barrier_evidence_ref;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

struct ShadowIndexBuildLedger {
  std::vector<ShadowIndexBuildEvidenceRow> evidence;
  u64 next_evidence_sequence = 1;
  u64 ledger_generation = 1;
};

struct ShadowIndexBuildRecord {
  TypedUuid build_id;
  TypedUuid shadow_index_uuid;
  TypedUuid table_uuid;
  TypedUuid published_index_uuid;
  SecondaryIndexKind index_kind = SecondaryIndexKind::non_unique;
  ShadowIndexBuildState state = ShadowIndexBuildState::requested;
  bool validation_evidence_present = false;
  bool publish_barrier_evidence_present = false;
  bool publish_barrier_engine_owned_mga = false;
  bool planner_visible = false;
  bool read_visible = false;
  std::string validation_evidence_ref;
  std::string publish_barrier_evidence_ref;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
};

struct ShadowIndexBuildRequest {
  TypedUuid build_id;
  TypedUuid shadow_index_uuid;
  TypedUuid table_uuid;
  SecondaryIndexKind index_kind = SecondaryIndexKind::non_unique;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
};

struct ShadowIndexValidationRequest {
  std::string validation_evidence_ref;
  bool validation_succeeded = false;
  bool engine_mga_inventory_evidence_present = false;
};

struct ShadowIndexPublishBarrierRequest {
  std::string publish_barrier_evidence_ref;
  bool engine_owned_mga_publish_barrier = false;
};

struct ShadowIndexLifecycleResult {
  Status status;
  ShadowIndexBuildDecision decision = ShadowIndexBuildDecision::refused;
  ShadowIndexBuildRecord record;
  ShadowIndexBuildEvidenceRow evidence;
  DiagnosticRecord diagnostic;
  bool fail_closed = true;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct ShadowIndexPlannerVisibilityResult {
  Status status;
  bool planner_visible = false;
  bool read_visible = false;
  TypedUuid visible_index_uuid;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && planner_visible && read_visible; }
};

const char* ShadowIndexBuildStateName(ShadowIndexBuildState state);
const char* ShadowIndexBuildDecisionName(ShadowIndexBuildDecision decision);

ShadowIndexLifecycleResult RequestShadowIndexBuild(
    ShadowIndexBuildLedger* ledger,
    const ShadowIndexBuildRequest& request);
ShadowIndexLifecycleResult StartShadowIndexBuild(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record);
ShadowIndexLifecycleResult CompleteShadowIndexBuild(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record);
ShadowIndexLifecycleResult ValidateShadowIndexBuild(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record,
    const ShadowIndexValidationRequest& request);
ShadowIndexLifecycleResult MarkShadowIndexPublishReady(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record,
    const ShadowIndexPublishBarrierRequest& request);
ShadowIndexLifecycleResult PublishShadowIndexBuild(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record);
ShadowIndexLifecycleResult CancelShadowIndexBuild(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record,
    std::string reason);
ShadowIndexLifecycleResult RefuseShadowIndexBuild(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record,
    std::string diagnostic_code,
    std::string reason);

ShadowIndexPlannerVisibilityResult EvaluateShadowIndexPlannerVisibility(
    const ShadowIndexBuildRecord& record);

DiagnosticRecord MakeShadowIndexBuildDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {});

}  // namespace scratchbird::core::index
