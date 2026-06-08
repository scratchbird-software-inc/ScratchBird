// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_request.hpp"

#include "cluster_candidate.hpp"
#include "optimizer_contract.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

void RequireString(const std::string& value,
                   const char* fact_name,
                   std::vector<OptimizerAuthorityFact>* facts,
                   std::vector<std::string>* diagnostics) {
  if (value.empty()) {
    facts->push_back(MakeAuthorityFact(fact_name, OptimizerAuthorityStatus::kMissing, true, "required optimizer authority fact is empty"));
    diagnostics->push_back(std::string("SB_OPT_AUTHORITY_MISSING.") + fact_name);
  } else {
    facts->push_back(MakeAuthorityFact(fact_name, OptimizerAuthorityStatus::kPresent, true));
  }
}

void RequireEpoch(std::uint64_t value,
                  const char* fact_name,
                  std::vector<OptimizerAuthorityFact>* facts,
                  std::vector<std::string>* diagnostics) {
  if (value == 0) {
    facts->push_back(MakeAuthorityFact(fact_name, OptimizerAuthorityStatus::kMissing, true, "epoch must be non-zero"));
    diagnostics->push_back(std::string("SB_OPT_AUTHORITY_MISSING.") + fact_name);
  } else {
    facts->push_back(MakeAuthorityFact(fact_name, OptimizerAuthorityStatus::kPresent, true));
  }
}

bool OptimizerPolicyTokenSafe(const std::string& token) {
  if (token.empty() || token.size() > 160) return false;
  return std::all_of(token.begin(), token.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '_' || ch == ':' || ch == '.' ||
           ch == '-' || ch == '=';
  });
}

void ValidateNormalizedOptimizerPolicyControls(
    const scratchbird::engine::planner::OptimizerPolicyMetadata& policy,
    std::vector<OptimizerAuthorityFact>* facts,
    std::vector<std::string>* diagnostics) {
  std::vector<std::string> tokens = {
      policy.normalized_controls.plan_profile_id,
      policy.normalized_controls.join_search_policy_id,
      policy.normalized_controls.memory_policy_id,
      policy.normalized_controls.spill_policy_id,
      policy.normalized_controls.parallelism_policy_id,
      policy.normalized_controls.what_if_policy_id,
  };
  tokens.insert(tokens.end(),
                policy.normalized_controls.safe_control_ids.begin(),
                policy.normalized_controls.safe_control_ids.end());
  tokens.insert(tokens.end(),
                policy.safe_control_ids.begin(),
                policy.safe_control_ids.end());

  for (const auto& token : tokens) {
    if (!OptimizerPolicyTokenSafe(token)) {
      facts->push_back(MakeAuthorityFact(
          "optimizer_policy_metadata",
          OptimizerAuthorityStatus::kRejected,
          true,
          "optimizer controls must be normalized safe policy tokens"));
      diagnostics->push_back(
          "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_unsafe_control_token");
      return;
    }
  }
}

void ValidateOptimizerPolicyMetadata(
    const scratchbird::engine::planner::OptimizerPolicyMetadata& policy,
    std::vector<OptimizerAuthorityFact>* facts,
    std::vector<std::string>* diagnostics) {
  if (!policy.optimizer_policy_metadata_present) {
    facts->push_back(MakeAuthorityFact("optimizer_policy_metadata",
                                       OptimizerAuthorityStatus::kMissing,
                                       false,
                                       "no optimizer policy metadata supplied"));
    return;
  }

  if (policy.raw_sql_text_present ||
      policy.parser_execution_authority_claimed ||
      policy.parser_session_directives_unbound ||
      policy.donor_or_legacy_policy_authority_claimed) {
    facts->push_back(MakeAuthorityFact(
        "optimizer_policy_metadata",
        OptimizerAuthorityStatus::kRejected,
        true,
        "OPCH_ENGINE_BOUNDARY_PARSER_SAFE_CONTROLS rejected unsafe policy authority"));
    if (policy.raw_sql_text_present) {
      diagnostics->push_back(
          "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_raw_sql_text");
    }
    if (policy.parser_execution_authority_claimed) {
      diagnostics->push_back(
          "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_parser_execution");
    }
    if (policy.parser_session_directives_unbound) {
      diagnostics->push_back(
          "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_unbound_parser_directive");
    }
    if (policy.donor_or_legacy_policy_authority_claimed) {
      diagnostics->push_back(
          "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_donor_or_legacy_authority");
    }
    return;
  }

  if (policy.policy_source_kind != "sblr_api" &&
      policy.policy_source_kind != "logical_plan" &&
      policy.policy_source_kind != "engine_api") {
    facts->push_back(MakeAuthorityFact(
        "optimizer_policy_metadata",
        OptimizerAuthorityStatus::kRejected,
        true,
        "optimizer policy source must be normalized SBLR/API/logical-plan metadata"));
    diagnostics->push_back(
        "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_source");
    return;
  }

  if (policy.policy_epoch == 0) {
    facts->push_back(MakeAuthorityFact("optimizer_policy_metadata",
                                       OptimizerAuthorityStatus::kMissing,
                                       true,
                                       "optimizer policy epoch is required"));
    diagnostics->push_back("SB_OPT_AUTHORITY_MISSING.optimizer_policy_epoch");
    return;
  }

  ValidateNormalizedOptimizerPolicyControls(policy, facts, diagnostics);
  if (!diagnostics->empty() &&
      diagnostics->back() ==
          "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_unsafe_control_token") {
    return;
  }

  facts->push_back(MakeAuthorityFact("optimizer_policy_metadata",
                                     OptimizerAuthorityStatus::kPresent,
                                     false,
                                     "OPCH_ENGINE_BOUNDARY_PARSER_SAFE_CONTROLS"));
}

void ValidateLogicalPlanPropertyMetadata(
    const scratchbird::engine::planner::LogicalPlanPropertyMetadata& metadata,
    std::vector<OptimizerAuthorityFact>* facts,
    std::vector<std::string>* diagnostics) {
  if (!metadata.metadata_present) {
    facts->push_back(MakeAuthorityFact("logical_property_metadata",
                                       OptimizerAuthorityStatus::kMissing,
                                       false,
                                       "no logical-plan property metadata supplied"));
    return;
  }

  if (metadata.raw_sql_text_present ||
      metadata.parser_execution_authority_claimed ||
      metadata.parser_visibility_or_finality_authority_claimed) {
    facts->push_back(MakeAuthorityFact(
        "logical_property_metadata",
        OptimizerAuthorityStatus::kRejected,
        true,
        "OPCH_LOGICAL_PROPERTY_METADATA rejected parser/sql authority claim"));
    if (metadata.raw_sql_text_present) {
      diagnostics->push_back(
          "SB_OPT_AUTHORITY_REJECTED.logical_property_raw_sql_text");
    }
    if (metadata.parser_execution_authority_claimed) {
      diagnostics->push_back(
          "SB_OPT_AUTHORITY_REJECTED.logical_property_parser_execution");
    }
    if (metadata.parser_visibility_or_finality_authority_claimed) {
      diagnostics->push_back(
          "SB_OPT_AUTHORITY_REJECTED.logical_property_parser_visibility_finality");
    }
    return;
  }

  if (!scratchbird::engine::planner::LogicalPlanPropertyMetadataSafe(metadata)) {
    facts->push_back(MakeAuthorityFact(
        "logical_property_metadata",
        OptimizerAuthorityStatus::kRejected,
        true,
        "logical property facts must be normalized SBLR/logical-plan metadata"));
    diagnostics->push_back(
        "SB_OPT_AUTHORITY_REJECTED.logical_property_metadata_not_normalized");
    return;
  }

  facts->push_back(MakeAuthorityFact("logical_property_metadata",
                                     OptimizerAuthorityStatus::kPresent,
                                     false,
                                     "OPCH_LOGICAL_PROPERTY_METADATA"));
}

}  // namespace

const char* OptimizerAuthorityStatusName(OptimizerAuthorityStatus status) {
  switch (status) {
    case OptimizerAuthorityStatus::kPresent: return "present";
    case OptimizerAuthorityStatus::kMissing: return "missing";
    case OptimizerAuthorityStatus::kRejected: return "rejected";
    case OptimizerAuthorityStatus::kRedacted: return "redacted";
  }
  return "missing";
}

OptimizerAuthorityFact MakeAuthorityFact(std::string fact_name,
                                         OptimizerAuthorityStatus status,
                                         bool required,
                                         std::string detail) {
  OptimizerAuthorityFact fact;
  fact.fact_name = std::move(fact_name);
  fact.status = status;
  fact.required = required;
  fact.detail = std::move(detail);
  return fact;
}

OptimizerRequestValidation ValidateBoundOptimizerRequest(const BoundOptimizerRequest& request) {
  OptimizerRequestValidation validation;
  auto facts = request.authority_facts;
  std::vector<std::string> diagnostics;

  RequireString(request.context.request_uuid, "request_uuid", &facts, &diagnostics);
  RequireString(request.context.operation_id, "operation_id", &facts, &diagnostics);
  RequireString(request.context.sblr_digest, "sblr_digest", &facts, &diagnostics);
  RequireString(request.context.descriptor_set_digest, "descriptor_set_digest", &facts, &diagnostics);
  RequireString(request.context.executor_capability_set_id, "executor_capability_set_id", &facts, &diagnostics);
  RequireEpoch(request.context.catalog_epoch, "catalog_epoch", &facts, &diagnostics);
  RequireEpoch(request.context.security_epoch, "security_epoch", &facts, &diagnostics);

  if (!request.context.security_context_present) {
    facts.push_back(MakeAuthorityFact("security_context", OptimizerAuthorityStatus::kMissing, true, "security context is required before planning"));
    diagnostics.push_back("SB_OPT_AUTHORITY_MISSING.security_context");
  } else {
    facts.push_back(MakeAuthorityFact("security_context", OptimizerAuthorityStatus::kPresent, true));
  }

  if (!request.context.transaction_context_present) {
    facts.push_back(MakeAuthorityFact("transaction_context", OptimizerAuthorityStatus::kMissing, true, "transaction and visibility context is required before planning"));
    diagnostics.push_back("SB_OPT_AUTHORITY_MISSING.transaction_context");
  } else {
    facts.push_back(MakeAuthorityFact("transaction_context", OptimizerAuthorityStatus::kPresent, true));
  }

  if (request.context.parser_owned_claims_present) {
    facts.push_back(MakeAuthorityFact("parser_owned_claims", OptimizerAuthorityStatus::kRejected, true, "parser claims are advisory and must be re-bound by the engine"));
    diagnostics.push_back("SB_OPT_AUTHORITY_REJECTED.parser_owned_claims");
  }

  if (request.context.name_authority_present) {
    facts.push_back(MakeAuthorityFact("name_authority", OptimizerAuthorityStatus::kRejected, true, "object names are labels, not durable optimizer authority"));
    diagnostics.push_back("SB_OPT_AUTHORITY_REJECTED.name_authority");
  }

  ValidateOptimizerPolicyMetadata(request.logical_plan.optimizer_policy,
                                  &facts,
                                  &diagnostics);
  ValidateLogicalPlanPropertyMetadata(request.logical_plan.property_metadata,
                                      &facts,
                                      &diagnostics);

  if (!request.logical_plan.ok || request.logical_plan.nodes.empty()) {
    facts.push_back(MakeAuthorityFact("logical_plan", OptimizerAuthorityStatus::kMissing, true, "bound logical plan is required"));
    diagnostics.push_back("SB_OPT_AUTHORITY_MISSING.logical_plan");
  } else {
    facts.push_back(MakeAuthorityFact("logical_plan", OptimizerAuthorityStatus::kPresent, true));
  }

  validation.ok = diagnostics.empty();
  validation.diagnostics = std::move(diagnostics);
  validation.authority_facts = std::move(facts);
  return validation;
}

BoundOptimizerResult MakeRefusedOptimizerResult(const BoundOptimizerRequest& request,
                                                std::string diagnostic_code,
                                                std::vector<std::string> diagnostics) {
  BoundOptimizerResult result;
  result.ok = false;
  result.diagnostic_code = std::move(diagnostic_code);
  result.plan_id = request.logical_plan.plan_id;
  result.diagnostics = std::move(diagnostics);
  if (result.diagnostics.empty()) result.diagnostics.push_back(result.diagnostic_code);
  return result;
}

BoundOptimizerResult OptimizeBoundRequest(const BoundOptimizerRequest& request) {
  const auto validation = ValidateBoundOptimizerRequest(request);
  if (!validation.ok) {
    return MakeRefusedOptimizerResult(request, "SB_OPT_REQUEST_REFUSED", validation.diagnostics);
  }

  auto optimized = request.catalog_access_path_request
                       ? OptimizeLogicalPlanWithAccessPathRequest(request.logical_plan,
                                                                  *request.catalog_access_path_request)
                       : OptimizeLogicalPlanWithStatistics(request.logical_plan, request.statistics);
  BoundOptimizerResult result;
  result.ok = optimized.ok;
  result.diagnostic_code = optimized.ok ? "SB_OPT_OK" : "SB_OPT_NO_SELECTABLE_PLAN";
  result.plan_id = request.logical_plan.plan_id;
  result.optimizer_profile = optimized.optimizer_profile;
  for (const auto& candidate : optimized.candidates) result.candidates.push_back(candidate.plan_candidate);
  result.diagnostics = optimized.diagnostics;

  if (request.context.cluster_build_enabled) {
    const ClusterCandidateFacts facts;
    result.candidates.push_back(BuildClusterFragmentCandidate(facts));
    result.candidates.push_back(BuildRemoteNodePushdownCandidate(facts));
  }
  return result;
}

}  // namespace scratchbird::engine::optimizer
