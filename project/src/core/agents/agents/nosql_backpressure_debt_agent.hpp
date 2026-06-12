// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr const char* kNoSqlBackpressureDebtPlanned =
    "SB_NOSQL_BACKPRESSURE_DEBT_PLANNED";
inline constexpr const char* kNoSqlBackpressureDebtNoCandidateWork =
    "SB_NOSQL_BACKPRESSURE_DEBT_NO_CANDIDATE_WORK";
inline constexpr const char* kNoSqlBackpressureDebtUnsupportedFamily =
    "SB_NOSQL_BACKPRESSURE_DEBT_UNSUPPORTED_FAMILY";
inline constexpr const char* kNoSqlBackpressureDebtUnsupportedKind =
    "SB_NOSQL_BACKPRESSURE_DEBT_UNSUPPORTED_KIND";
inline constexpr const char* kNoSqlBackpressureDebtEvidenceNotAuthoritative =
    "SB_NOSQL_BACKPRESSURE_DEBT_EVIDENCE_NOT_AUTHORITATIVE";
inline constexpr const char* kNoSqlBackpressureDebtUnsafeAuthority =
    "SB_NOSQL_BACKPRESSURE_DEBT_UNSAFE_AUTHORITY";
inline constexpr const char* kNoSqlBackpressureDebtResultStale =
    "SB_NOSQL_BACKPRESSURE_RESULT_SUPPRESSED_STALE";
inline constexpr const char* kNoSqlBackpressureDebtResultOverBudget =
    "SB_NOSQL_BACKPRESSURE_RESULT_SUPPRESSED_OVER_BUDGET";
inline constexpr const char* kNoSqlBackpressureDebtResultUnsafe =
    "SB_NOSQL_BACKPRESSURE_RESULT_SUPPRESSED_UNSAFE";

enum class NoSqlBackpressureDebtFamily : u32 {
  unknown,
  key_value,
  document,
  search,
  vector,
  graph,
  time_series
};

enum class NoSqlBackpressureDebtKind : u32 {
  unknown,
  refresh,
  merge_compaction,
  generation_build,
  payload_policy,
  slowdown_backpressure_policy,
  strict_bulk_redirect_policy,
  result_suppression_policy
};

enum class NoSqlBackpressureDebtDecisionKind : u32 {
  planned,
  suppressed_result,
  no_op,
  refused_non_authoritative,
  refused
};

struct NoSqlBackpressureDebtEntry {
  NoSqlBackpressureDebtFamily family = NoSqlBackpressureDebtFamily::unknown;
  NoSqlBackpressureDebtKind debt_kind = NoSqlBackpressureDebtKind::unknown;
  std::string object_uuid;
  std::string result_id;
  u64 evidence_epoch = 0;
  u64 required_epoch = 0;
  u64 debt_units = 0;
  u64 observed_cost_units = 0;
  u64 budget_cost_units = 0;
  bool evidence_authoritative = false;
  bool stale_result = false;
  bool over_budget_result = false;
  bool unsafe_result = false;
};

struct NoSqlBackpressureDebtAuthority {
  bool engine_mga_authoritative = false;
  bool request_context_authoritative = false;
  bool security_snapshot_bound = false;
  bool grants_proven = false;
  bool row_mga_recheck_required = true;
  bool row_security_recheck_required = true;
  bool parser_or_reference_authority = false;
  bool provider_transaction_finality_authority = false;
  bool provider_visibility_authority = false;
  bool client_autocommit_authority = false;
  bool wal_recovery_authority = false;
};

struct NoSqlBackpressureDebtLedgerRow {
  NoSqlBackpressureDebtFamily family = NoSqlBackpressureDebtFamily::unknown;
  NoSqlBackpressureDebtKind debt_kind = NoSqlBackpressureDebtKind::unknown;
  std::string object_uuid;
  std::string result_id;
  u64 evidence_epoch = 0;
  u64 required_epoch = 0;
  u64 debt_units = 0;
  u64 observed_cost_units = 0;
  u64 budget_cost_units = 0;
  bool stale_result = false;
  bool over_budget_result = false;
  bool unsafe_result = false;
};

struct NoSqlBackpressureDebtPlanAction {
  NoSqlBackpressureDebtFamily family = NoSqlBackpressureDebtFamily::unknown;
  NoSqlBackpressureDebtKind debt_kind = NoSqlBackpressureDebtKind::unknown;
  std::string object_uuid;
  std::string action_kind;
  std::string policy_kind;
  u64 planned_work_units = 0;
};

struct NoSqlBackpressureResultSuppression {
  NoSqlBackpressureDebtFamily family = NoSqlBackpressureDebtFamily::unknown;
  std::string object_uuid;
  std::string result_id;
  std::string diagnostic_code;
  std::string diagnostic_detail;
  u64 evidence_epoch = 0;
  u64 required_epoch = 0;
  u64 observed_cost_units = 0;
  u64 budget_cost_units = 0;
  bool stale_result = false;
  bool over_budget_result = false;
  bool unsafe_result = false;
};

struct NoSqlBackpressureDebtEvidenceField {
  std::string key;
  std::string value;
};

struct NoSqlBackpressureDebtAgentRequest {
  NoSqlBackpressureDebtAuthority authority;
  std::vector<NoSqlBackpressureDebtEntry> entries;
  u64 now_microseconds = 0;
};

struct NoSqlBackpressureDebtAgentResult {
  Status status;
  NoSqlBackpressureDebtDecisionKind decision =
      NoSqlBackpressureDebtDecisionKind::refused;
  DiagnosticRecord diagnostic;
  std::vector<NoSqlBackpressureDebtLedgerRow> ledger_rows;
  std::vector<NoSqlBackpressureDebtPlanAction> actions;
  std::vector<NoSqlBackpressureResultSuppression> suppressions;
  std::vector<NoSqlBackpressureDebtEvidenceField> evidence;
  bool fail_closed = true;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* NoSqlBackpressureDebtFamilyName(
    NoSqlBackpressureDebtFamily family);
const char* NoSqlBackpressureDebtKindName(NoSqlBackpressureDebtKind kind);
const char* NoSqlBackpressureDebtDecisionKindName(
    NoSqlBackpressureDebtDecisionKind decision);

NoSqlBackpressureDebtAgentResult RunNoSqlBackpressureDebtAgent(
    const NoSqlBackpressureDebtAgentRequest& request);

DiagnosticRecord MakeNoSqlBackpressureDebtDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

const char* nosql_backpressure_debt_agent_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
