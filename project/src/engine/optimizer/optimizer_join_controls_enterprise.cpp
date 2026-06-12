// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_join_controls_enterprise.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <string_view>

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace {

constexpr std::size_t kEnterpriseMaxJoinRelations = 16;
constexpr std::size_t kEnterpriseMaxFrontierWidth = 128;
constexpr std::uint64_t kEnterpriseMaxTransitionBudget = 1000000;

void Refuse(EnterpriseJoinControlResult* result, std::string diagnostic) {
  result->ok = false;
  result->refusal_diagnostic = std::move(diagnostic);
  result->diagnostics.push_back(result->refusal_diagnostic);
}

void AddUnique(std::vector<std::string>* values, std::string value) {
  if (std::find(values->begin(), values->end(), value) == values->end()) {
    values->push_back(std::move(value));
  }
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() &&
         value.substr(0, prefix.size()) == prefix;
}

bool ParseUnsigned(std::string_view value, std::uint64_t* out) {
  if (value.empty() || out == nullptr) return false;
  std::uint64_t parsed = 0;
  const auto* begin = value.data();
  const auto* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end) return false;
  *out = parsed;
  return true;
}

bool SafePolicyToken(const std::string& token) {
  if (token.empty() || token.size() > 160) return false;
  for (const unsigned char ch : token) {
    if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == ':' || ch == '.' || ch == '/') {
      continue;
    }
    return false;
  }
  const auto lowered = [&]() {
    std::string copy = token;
    std::transform(copy.begin(), copy.end(), copy.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return copy;
  }();
  return lowered.find("select") == std::string::npos &&
         lowered.find("insert") == std::string::npos &&
         lowered.find("update") == std::string::npos &&
         lowered.find("delete") == std::string::npos &&
         lowered.find(" from ") == std::string::npos &&
         lowered.find("where") == std::string::npos &&
         lowered.find("sql") == std::string::npos &&
         lowered.find("reference") == std::string::npos;
}

bool SafePolicySource(const std::string& source) {
  return source == "sblr_api" ||
         source == "engine_api" ||
         source == "logical_plan" ||
         source == "normalized_sblr" ||
         source == "sblr.logical_plan";
}

bool StrategyFromPolicyId(const std::string& id, JoinSearchStrategy* strategy) {
  if (strategy == nullptr) return false;
  if (id == "join_search:default" || id == "join_search:auto") {
    *strategy = JoinSearchStrategy::kAuto;
    return true;
  }
  if (id == "join_search:exhaustive_dp") {
    *strategy = JoinSearchStrategy::kExhaustiveDp;
    return true;
  }
  if (id == "join_search:bounded_dp") {
    *strategy = JoinSearchStrategy::kBoundedDp;
    return true;
  }
  if (id == "join_search:hypergraph_greedy") {
    *strategy = JoinSearchStrategy::kHypergraphGreedy;
    return true;
  }
  if (id == "join_search:heuristic_greedy") {
    *strategy = JoinSearchStrategy::kHeuristicGreedy;
    return true;
  }
  if (id == "join_search:input_order") {
    *strategy = JoinSearchStrategy::kInputOrder;
    return true;
  }
  return false;
}

std::vector<std::string> JoinControlTokens(const planner::OptimizerPolicyMetadata& policy) {
  std::vector<std::string> tokens = policy.normalized_controls.safe_control_ids;
  tokens.insert(tokens.end(), policy.safe_control_ids.begin(), policy.safe_control_ids.end());
  return tokens;
}

bool ApplyControlToken(const std::string& token,
                       const EnterpriseJoinControlRequest& request,
                       EnterpriseJoinControlResult* result) {
  if (!SafePolicyToken(token)) {
    Refuse(result, "SB_OPT_JOIN_CONTROL_UNSAFE_POLICY_TOKEN");
    return false;
  }

  std::uint64_t parsed = 0;
  if (StartsWith(token, "join.frontier_width.")) {
    if (!ParseUnsigned(std::string_view(token).substr(20), &parsed) ||
        parsed == 0 || parsed > kEnterpriseMaxFrontierWidth) {
      Refuse(result, "SB_OPT_JOIN_CONTROL_FRONTIER_WIDTH_INVALID");
      return false;
    }
    result->join_policy.frontier_width = static_cast<std::size_t>(parsed);
    AddUnique(&result->evidence, "join_control.frontier_width=" + std::to_string(parsed));
    return true;
  }
  if (StartsWith(token, "join.exhaustive_limit.")) {
    if (!ParseUnsigned(std::string_view(token).substr(22), &parsed) ||
        parsed == 0 || parsed > kEnterpriseMaxJoinRelations) {
      Refuse(result, "SB_OPT_JOIN_CONTROL_EXHAUSTIVE_LIMIT_INVALID");
      return false;
    }
    result->join_policy.exhaustive_relation_limit = static_cast<std::size_t>(parsed);
    AddUnique(&result->evidence, "join_control.exhaustive_limit=" + std::to_string(parsed));
    return true;
  }
  if (StartsWith(token, "join.bounded_limit.")) {
    if (!ParseUnsigned(std::string_view(token).substr(19), &parsed) ||
        parsed == 0 || parsed > kEnterpriseMaxJoinRelations) {
      Refuse(result, "SB_OPT_JOIN_CONTROL_BOUNDED_LIMIT_INVALID");
      return false;
    }
    result->join_policy.bounded_relation_limit = static_cast<std::size_t>(parsed);
    AddUnique(&result->evidence, "join_control.bounded_limit=" + std::to_string(parsed));
    return true;
  }
  if (StartsWith(token, "join.transition_budget.")) {
    if (!ParseUnsigned(std::string_view(token).substr(23), &parsed) ||
        parsed == 0 || parsed > kEnterpriseMaxTransitionBudget ||
        (request.max_transition_budget != 0 && parsed > request.max_transition_budget)) {
      Refuse(result, "SB_OPT_JOIN_CONTROL_TRANSITION_BUDGET_INVALID");
      return false;
    }
    result->join_policy.transition_budget = parsed;
    AddUnique(&result->evidence, "join_control.transition_budget=" + std::to_string(parsed));
    return true;
  }
  if (token == "join.preserve_property_frontier" ||
      token == "join_frontier:retain_property") {
    result->join_policy.preserve_property_frontier = true;
    AddUnique(&result->evidence, "join_control.property_frontier=preserve");
    return true;
  }
  if (token == "join.disable_property_frontier") {
    result->join_policy.preserve_property_frontier = false;
    AddUnique(&result->evidence, "join_control.property_frontier=single_state");
    return true;
  }

  AddUnique(&result->evidence, "join_control.ignored_safe_token=" + token);
  return true;
}

bool UnsafeAuthority(const EnterpriseJoinControlAuthority& authority) {
  return !authority.engine_optimizer_policy_authority ||
         !authority.normalized_sblr_or_api_metadata ||
         authority.parser_execution_authority ||
         authority.raw_sql_text_authority ||
         authority.reference_or_legacy_authority ||
         authority.client_finality_or_visibility_authority ||
         authority.metric_finality_or_visibility_authority ||
         authority.recovery_authority ||
         authority.cluster_authority ||
         authority.fixture_or_test_authority;
}

}  // namespace

EnterpriseJoinControlResult BuildEnterpriseJoinSearchPolicy(
    const EnterpriseJoinControlRequest& request) {
  EnterpriseJoinControlResult result;
  result.join_policy.memory_budget_bytes = request.runtime_memory_budget_bytes;
  result.join_policy.preserve_property_frontier = true;

  if (UnsafeAuthority(request.authority)) {
    Refuse(&result, "SB_OPT_JOIN_CONTROL_UNSAFE_AUTHORITY");
    return result;
  }
  if (request.production_mode && request.runtime_memory_budget_bytes == 0) {
    Refuse(&result, "SB_OPT_JOIN_CONTROL_MEMORY_BUDGET_REQUIRED");
    return result;
  }
  if (request.relation_count > kEnterpriseMaxJoinRelations) {
    Refuse(&result, "SB_OPT_JOIN_CONTROL_RELATION_LIMIT_EXCEEDED");
    return result;
  }

  const auto& policy = request.policy_metadata;
  if (!policy.optimizer_policy_metadata_present || policy.policy_epoch == 0 ||
      !SafePolicySource(policy.policy_source_kind)) {
    Refuse(&result, "SB_OPT_JOIN_CONTROL_POLICY_SCOPE_INVALID");
    return result;
  }
  if (policy.raw_sql_text_present ||
      policy.parser_execution_authority_claimed ||
      policy.parser_session_directives_unbound ||
      policy.reference_or_legacy_policy_authority_claimed) {
    Refuse(&result, "SB_OPT_JOIN_CONTROL_POLICY_AUTHORITY_INVALID");
    return result;
  }
  if (!StrategyFromPolicyId(policy.normalized_controls.join_search_policy_id,
                            &result.join_policy.strategy)) {
    Refuse(&result, "SB_OPT_JOIN_CONTROL_UNKNOWN_STRATEGY");
    return result;
  }

  for (const auto& token : JoinControlTokens(policy)) {
    if (!ApplyControlToken(token, request, &result)) return result;
  }

  result.ok = true;
  result.evidence.push_back("join_control.policy_epoch=" + std::to_string(policy.policy_epoch));
  result.evidence.push_back("join_control.policy_source=" + policy.policy_source_kind);
  result.evidence.push_back(std::string("join_control.strategy=") +
                            JoinSearchStrategyName(result.join_policy.strategy));
  result.evidence.push_back("join_control.parser_authority=false");
  result.evidence.push_back("join_control.reference_authority=false");
  result.evidence.push_back("join_control.cluster_authority=false");
  result.diagnostics.push_back("SB_OPT_JOIN_CONTROL_POLICY_ACCEPTED");
  return result;
}

bool ValidateEnterpriseJoinStrategyTelemetry(const EnterpriseJoinControlResult& controls,
                                             const JoinOrderPlan& plan,
                                             std::vector<std::string>* diagnostics) {
  if (diagnostics == nullptr) return false;
  if (!controls.ok) {
    diagnostics->push_back("SB_OPT_JOIN_TELEMETRY_CONTROLS_REFUSED");
    return false;
  }
  if (!plan.ok) {
    diagnostics->push_back("SB_OPT_JOIN_TELEMETRY_PLAN_NOT_OK");
    return false;
  }
  if (controls.join_policy.strategy != JoinSearchStrategy::kAuto &&
      plan.requested_strategy != controls.join_policy.strategy) {
    diagnostics->push_back("SB_OPT_JOIN_TELEMETRY_REQUESTED_STRATEGY_MISMATCH");
    return false;
  }
  if (plan.selected_strategy == JoinSearchStrategy::kAuto) {
    diagnostics->push_back("SB_OPT_JOIN_TELEMETRY_SELECTED_STRATEGY_MISSING");
    return false;
  }
  if (plan.enumerated_subsets == 0 && controls.join_policy.strategy != JoinSearchStrategy::kInputOrder) {
    diagnostics->push_back("SB_OPT_JOIN_TELEMETRY_ENUMERATION_MISSING");
    return false;
  }
  if (plan.selected_property_signature.empty()) {
    diagnostics->push_back("SB_OPT_JOIN_TELEMETRY_PROPERTY_SIGNATURE_MISSING");
    return false;
  }
  diagnostics->push_back("SB_OPT_JOIN_TELEMETRY_VALIDATED");
  return true;
}

}  // namespace scratchbird::engine::optimizer
