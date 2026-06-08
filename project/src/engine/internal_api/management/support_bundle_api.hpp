// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "current_row_map.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "page_finality_evidence.hpp"
#include "savepoint.hpp"
#include "transaction_cleanup.hpp"
#include "transaction_horizon.hpp"
#include "transaction_inventory.hpp"
#include "transaction_lock.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_MANAGEMENT_SUPPORT_BUNDLE_API
// SEARCH_KEY: DPC_MANAGEMENT_SUPPORT_BUNDLE_OBSERVABILITY
struct EngineSupportBundleAgentEvidenceSource {
  std::string agent_type_id;
  std::string agent_uuid;
  std::string filespace_uuid;
  std::string policy_uuid;
  std::string evidence_uuid;
  std::string evidence_kind;
  std::string result_state;
  std::string diagnostic_code;
  std::string payload_digest;
  std::string retention_class;
  std::string retention_policy_ref;
  std::string retention_deadline;
  bool legal_hold = false;
  bool maintenance_hold = false;
  bool purge_eligible = false;
  std::string physical_path;
  std::string unsafe_payload;
  bool payload_redacted = true;
};

struct EngineSupportBundleTransactionEvidenceSnapshot {
  bool inventory_present = false;
  bool inventory_authoritative = false;
  scratchbird::transaction::mga::LocalTransactionInventory inventory;

  bool horizons_present = false;
  bool horizons_authoritative = false;
  scratchbird::transaction::mga::LocalTransactionHorizons horizons;

  bool current_row_decision_present = false;
  scratchbird::transaction::mga::CurrentRowMapDecision current_row_decision;

  bool page_finality_decision_present = false;
  scratchbird::transaction::mga::PageFinalityEvidenceDecision page_finality_decision;

  bool cleanup_result_present = false;
  scratchbird::transaction::mga::LocalCleanupWorksetResult cleanup_result;

  bool lock_result_present = false;
  scratchbird::transaction::mga::TransactionLockResult lock_result;

  bool savepoint_plan_present = false;
  scratchbird::transaction::mga::SavepointRollbackPlan savepoint_plan;

  bool support_bundle_is_authority = false;
  bool parser_finality_authority = false;
  bool client_finality_authority = false;
  bool donor_finality_authority = false;
  bool timestamp_finality_authority = false;
  bool uuid_ordering_finality_authority = false;
  bool event_stream_finality_authority = false;
  bool wal_recovery_authority = false;
};

struct EnginePrepareSupportBundleRequest : EngineApiRequest {
  std::vector<EngineSupportBundleAgentEvidenceSource> agent_runtime_evidence;
  PerformanceOptimizationSurfaceSnapshot performance_optimization_snapshot;
  bool performance_optimization_snapshot_present = false;
  std::vector<PerformanceOptimizationConfigOverride>
      performance_optimization_config_overrides;
  EngineSupportBundleTransactionEvidenceSnapshot transaction_evidence_snapshot;
  bool transaction_evidence_snapshot_present = false;
};
struct EnginePrepareSupportBundleResult : EngineApiResult {
  bool redaction_applied = false;
  bool forbidden_fields_absent = false;
  bool flush_required_before_export = false;
  bool agent_runtime_evidence_collected = false;
  bool performance_optimization_surface_collected = false;
  bool transaction_evidence_collected = false;
  std::string retention_policy_ref;
  std::string redaction_profile_ref;
  std::string authority_path;
  std::string audit_envelope_ref;
  std::string support_bundle_json;
};
EnginePrepareSupportBundleResult EnginePrepareSupportBundle(const EnginePrepareSupportBundleRequest& request);

}  // namespace scratchbird::engine::internal_api
