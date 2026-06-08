// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "agents/nosql_backpressure_debt_agent.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class EngineNoSqlBackpressureDebtKind {
  kUnknown,
  kRefresh,
  kMergeCompaction,
  kGenerationBuild,
  kPayloadPolicy,
  kSlowdownBackpressurePolicy,
  kStrictBulkRedirectPolicy,
  kResultSuppressionPolicy
};

const char* EngineNoSqlBackpressureDebtKindName(
    EngineNoSqlBackpressureDebtKind kind);

struct EngineNoSqlBackpressureDebtEntry {
  EngineNoSqlProviderFamily family = EngineNoSqlProviderFamily::kUnknown;
  EngineNoSqlBackpressureDebtKind debt_kind =
      EngineNoSqlBackpressureDebtKind::kUnknown;
  std::string object_uuid;
  std::string result_id;
  EngineApiU64 evidence_epoch = 0;
  EngineApiU64 required_epoch = 0;
  EngineApiU64 debt_units = 0;
  EngineApiU64 observed_cost_units = 0;
  EngineApiU64 budget_cost_units = 0;
  bool evidence_authoritative = false;
  bool stale_result = false;
  bool over_budget_result = false;
  bool unsafe_result = false;
};

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_BACKPRESSURE_DEBT_API
struct EnginePlanNoSqlBackpressureDebtRequest : EngineApiRequest {
  std::vector<EngineNoSqlBackpressureDebtEntry> entries;
  EngineApiU64 now_microseconds = 0;

  bool engine_mga_authoritative = false;
  bool request_context_authoritative = false;
  bool security_snapshot_bound = false;
  bool grants_proven = false;
  bool row_mga_recheck_required = true;
  bool row_security_recheck_required = true;
  bool parser_or_donor_authority = false;
  bool provider_claims_transaction_finality_authority = false;
  bool provider_claims_visibility_authority = false;
  bool client_claims_autocommit_authority = false;
  bool write_ahead_log_claims_recovery_authority = false;  // wal-not-authority
};

struct EnginePlanNoSqlBackpressureDebtResult : EngineApiResult {
  scratchbird::core::agents::implemented_agents::
      NoSqlBackpressureDebtAgentResult agent_result;
};

EnginePlanNoSqlBackpressureDebtResult EnginePlanNoSqlBackpressureDebt(
    const EnginePlanNoSqlBackpressureDebtRequest& request);

}  // namespace scratchbird::engine::internal_api
