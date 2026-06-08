// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"
#include "dynamic_cleanup_debt_scheduler.hpp"
#include "transaction_cleanup_horizon_service.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::AuthoritativeCleanupHorizonRequest;
using scratchbird::transaction::mga::AuthoritativeCleanupHorizonResult;

inline constexpr const char* kNoSqlMaintenanceCleanupHorizonNotAuthoritative =
    "SB_NOSQL_MAINTENANCE_MGA_CLEANUP_HORIZON_NOT_AUTHORITATIVE";
inline constexpr const char* kNoSqlMaintenanceGenerationEvidenceNotAuthoritative =
    "SB_NOSQL_MAINTENANCE_GENERATION_EVIDENCE_NOT_AUTHORITATIVE";
inline constexpr const char* kNoSqlMaintenanceTtlEvidenceNotAuthoritative =
    "SB_NOSQL_MAINTENANCE_TTL_EVIDENCE_NOT_AUTHORITATIVE";
inline constexpr const char* kNoSqlMaintenanceGenerationNotBelowMgaHorizon =
    "SB_NOSQL_MAINTENANCE_GENERATION_NOT_BELOW_MGA_HORIZON";
inline constexpr const char* kNoSqlMaintenanceTtlNotBelowMgaHorizon =
    "SB_NOSQL_MAINTENANCE_TTL_NOT_BELOW_MGA_HORIZON";
inline constexpr const char* kNoSqlMaintenanceUnsupportedFamily =
    "SB_NOSQL_MAINTENANCE_UNSUPPORTED_FAMILY";
inline constexpr const char* kNoSqlMaintenanceNoCandidateWork =
    "SB_NOSQL_MAINTENANCE_NO_CANDIDATE_WORK";
inline constexpr const char* kNoSqlMaintenancePlanned =
    "SB_NOSQL_MAINTENANCE_PLANNED";
inline constexpr const char* kNoSqlMaintenanceExecuted =
    "SB_NOSQL_MAINTENANCE_EXECUTED";

enum class NoSqlFamilyMaintenanceFamily : u32 {
  unknown,
  key_value,
  document,
  search,
  vector,
  graph,
  time_series
};

enum class NoSqlFamilyMaintenanceDecisionKind : u32 {
  planned,
  executed,
  no_op,
  suppressed_by_mga_horizon,
  refused_non_authoritative,
  refused
};

struct NoSqlFamilyMaintenanceCandidate {
  NoSqlFamilyMaintenanceFamily family = NoSqlFamilyMaintenanceFamily::unknown;
  std::string generation_id;
  std::string generation_kind;
  u64 sealed_local_transaction_id = 0;
  u64 superseded_local_transaction_id = 0;
  u64 expires_after_local_transaction_id = 0;
  u64 estimated_bytes = 0;
  bool generation_evidence_authoritative = false;
  bool ttl_evidence_authoritative = false;
};

struct NoSqlFamilyMaintenanceAction {
  NoSqlFamilyMaintenanceFamily family = NoSqlFamilyMaintenanceFamily::unknown;
  std::string generation_id;
  std::string action_kind;
  std::string policy_kind;
  u64 cleanup_horizon_local_transaction_id = 0;
  u64 governing_local_transaction_id = 0;
  u64 estimated_bytes = 0;
  bool executed = false;
};

struct NoSqlFamilyMaintenanceSuppression {
  NoSqlFamilyMaintenanceFamily family = NoSqlFamilyMaintenanceFamily::unknown;
  std::string generation_id;
  std::string diagnostic_code;
  u64 cleanup_horizon_local_transaction_id = 0;
  u64 governing_local_transaction_id = 0;
};

struct NoSqlFamilyMaintenanceEvidenceField {
  std::string key;
  std::string value;
};

struct NoSqlFamilyMaintenanceAgentRequest {
  AuthoritativeCleanupHorizonRequest horizon_request;
  scratchbird::core::agents::DynamicCleanupDebtSchedulerPolicy scheduler_policy;
  std::vector<NoSqlFamilyMaintenanceCandidate> candidates;
  u64 now_microseconds = 0;
  bool engine_mga_authoritative = false;
  bool foreground_work_active = false;
  bool execute_plan = false;
};

struct NoSqlFamilyMaintenanceAgentResult {
  Status status;
  NoSqlFamilyMaintenanceDecisionKind decision =
      NoSqlFamilyMaintenanceDecisionKind::refused;
  DiagnosticRecord diagnostic;
  AuthoritativeCleanupHorizonResult horizon;
  scratchbird::core::agents::DynamicCleanupDebtSchedulerResult scheduler_result;
  std::vector<NoSqlFamilyMaintenanceAction> actions;
  std::vector<NoSqlFamilyMaintenanceSuppression> suppressions;
  std::vector<NoSqlFamilyMaintenanceEvidenceField> evidence;
  bool fail_closed = true;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* NoSqlFamilyMaintenanceFamilyName(NoSqlFamilyMaintenanceFamily family);
const char* NoSqlFamilyMaintenanceDecisionKindName(
    NoSqlFamilyMaintenanceDecisionKind decision);

NoSqlFamilyMaintenanceAgentResult RunNoSqlFamilyMaintenanceAgent(
    const NoSqlFamilyMaintenanceAgentRequest& request);

DiagnosticRecord MakeNoSqlFamilyMaintenanceAgentDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

const char* nosql_family_maintenance_agent_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
