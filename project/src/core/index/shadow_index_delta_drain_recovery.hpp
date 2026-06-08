// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DPC_SHADOW_INDEX_DELTA_DRAIN_RECOVERY
#include "secondary_index_delta_ledger.hpp"
#include "shadow_index_build_lifecycle.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

inline constexpr const char* kShadowIndexDeltaDrainRecoverySearchKey =
    "DPC_SHADOW_INDEX_DELTA_DRAIN_RECOVERY";

enum class ShadowIndexDeltaDrainState : u32 {
  absent = 1,
  draining = 2,
  drained = 3,
  recovered = 4,
  refused = 5,
  corrupt = 6
};

enum class ShadowIndexDeltaDrainRecoveryClass : u32 {
  clean_complete = 1,
  recoverable_replay = 2,
  incomplete_refuse_visible_use = 3,
  corrupt_refuse_visible_use = 4,
  non_authoritative_refuse_visible_use = 5
};

enum class ShadowIndexDeltaDrainRecoveryAction : u32 {
  no_action = 1,
  replay_applied_deltas_once = 2,
  refuse_visible_index_use = 3
};

struct ShadowIndexAppliedDeltaRecord {
  SecondaryIndexDeltaEntry delta;
  std::string durable_delta_identity_ref;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
};

struct ShadowIndexDeltaDrainEvidenceRow {
  u64 sequence = 0;
  TypedUuid evidence_id;
  TypedUuid build_id;
  TypedUuid shadow_index_uuid;
  TypedUuid table_uuid;
  ShadowIndexDeltaDrainState state = ShadowIndexDeltaDrainState::absent;
  u64 scanned_delta_count = 0;
  u64 eligible_committed_delta_count = 0;
  u64 newly_applied_delta_count = 0;
  u64 idempotent_replayed_delta_count = 0;
  u64 skipped_uncommitted_delta_count = 0;
  u64 skipped_wrong_target_delta_count = 0;
  u64 applied_delta_total_count = 0;
  bool drain_complete = false;
  bool durable_transaction_inventory_authoritative = false;
  bool durable_transaction_horizon_authoritative = false;
  bool parser_finality_authority = false;
  bool client_state_authority = false;
  bool timestamp_ordering_authority = false;
  bool uuid_ordering_authority = false;
  bool event_stream_authority = false;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

struct ShadowIndexDeltaDrainLedger {
  std::vector<ShadowIndexDeltaDrainEvidenceRow> evidence;
  std::vector<ShadowIndexAppliedDeltaRecord> applied_deltas;
  std::vector<SecondaryIndexBaseEntry> shadow_entries;
  u64 next_evidence_sequence = 1;
  u64 ledger_generation = 1;
  u64 authoritative_visible_through_local_transaction_id = 0;
  bool drain_complete = false;
  bool durable_transaction_inventory_authoritative = false;
  bool durable_transaction_horizon_authoritative = false;
  TypedUuid build_id;
  TypedUuid shadow_index_uuid;
  TypedUuid table_uuid;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
};

struct ShadowIndexDeltaDrainRequest {
  TypedUuid build_id;
  TypedUuid shadow_index_uuid;
  TypedUuid table_uuid;
  u64 authoritative_visible_through_local_transaction_id = 0;
  bool durable_transaction_inventory_authoritative = false;
  bool durable_transaction_horizon_authoritative = false;
  std::string engine_mga_inventory_evidence_ref;
  std::string engine_mga_horizon_evidence_ref;
  u64 max_records_to_scan = 1024;
  u64 max_records_to_apply = 256;
  bool parser_finality_authority = false;
  bool client_state_authority = false;
  bool timestamp_ordering_authority = false;
  bool uuid_ordering_authority = false;
  bool event_stream_authority = false;
};

struct ShadowIndexDeltaDrainResult {
  Status status;
  bool drained = false;
  bool fail_closed = true;
  u64 scanned_delta_count = 0;
  u64 eligible_committed_delta_count = 0;
  u64 newly_applied_delta_count = 0;
  u64 idempotent_replayed_delta_count = 0;
  u64 skipped_uncommitted_delta_count = 0;
  u64 skipped_wrong_target_delta_count = 0;
  ShadowIndexDeltaDrainEvidenceRow evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && drained && !fail_closed; }
};

struct ShadowIndexDeltaDrainRecoveryResult {
  Status status;
  bool recovered = false;
  bool fail_closed = true;
  ShadowIndexDeltaDrainRecoveryClass recovery_class =
      ShadowIndexDeltaDrainRecoveryClass::corrupt_refuse_visible_use;
  ShadowIndexDeltaDrainRecoveryAction action =
      ShadowIndexDeltaDrainRecoveryAction::refuse_visible_index_use;
  u64 replayed_delta_count = 0;
  u64 idempotent_replayed_delta_count = 0;
  std::string stable_reason;
  ShadowIndexDeltaDrainEvidenceRow evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct ShadowIndexDeltaPublishEligibilityResult {
  Status status;
  bool publish_allowed = false;
  bool planner_visible = false;
  bool read_visible = false;
  TypedUuid visible_index_uuid;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && publish_allowed; }
};

const char* ShadowIndexDeltaDrainStateName(ShadowIndexDeltaDrainState state);
const char* ShadowIndexDeltaDrainRecoveryClassName(
    ShadowIndexDeltaDrainRecoveryClass recovery_class);
const char* ShadowIndexDeltaDrainRecoveryActionName(
    ShadowIndexDeltaDrainRecoveryAction action);

ShadowIndexDeltaDrainResult DrainShadowIndexCommittedDeltas(
    ShadowIndexDeltaDrainLedger* drain_ledger,
    const SecondaryIndexDeltaLedger& source_delta_ledger,
    const ShadowIndexDeltaDrainRequest& request);

ShadowIndexDeltaDrainRecoveryResult ClassifyShadowIndexDeltaDrainForRecovery(
    const ShadowIndexDeltaDrainLedger& drain_ledger,
    const ShadowIndexDeltaDrainRequest& request);
ShadowIndexDeltaDrainRecoveryResult RecoverShadowIndexDeltaDrain(
    ShadowIndexDeltaDrainLedger* drain_ledger,
    const ShadowIndexDeltaDrainRequest& request);

ShadowIndexDeltaPublishEligibilityResult EvaluateShadowIndexDeltaDrainPublishEligibility(
    const ShadowIndexBuildRecord& lifecycle_record,
    const ShadowIndexDeltaDrainLedger* drain_ledger);
ShadowIndexLifecycleResult PublishShadowIndexBuildWithDeltaDrainEvidence(
    ShadowIndexBuildLedger* lifecycle_ledger,
    ShadowIndexBuildRecord* lifecycle_record,
    const ShadowIndexDeltaDrainLedger* drain_ledger);

DiagnosticRecord MakeShadowIndexDeltaDrainDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {});

}  // namespace scratchbird::core::index
