// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_hint_policy.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

void AddEvidence(OptimizerHintPolicyAdmission* admission,
                 std::string evidence) {
  if (admission == nullptr) return;
  admission->evidence.push_back(std::move(evidence));
}

OptimizerHintPolicyAdmission Refuse(const OptimizerHintPolicyRequest& request,
                                    std::string code,
                                    std::string evidence) {
  OptimizerHintPolicyAdmission admission;
  admission.accepted = false;
  admission.applied = false;
  admission.diagnostic_code = std::move(code);
  admission.policy_digest = request.policy_uuid.empty() ? "" : request.policy_uuid;
  AddEvidence(&admission, std::move(evidence));
  AddEvidence(&admission, "hint_policy.accepted=false");
  AddEvidence(&admission, "hint_policy.applied=false");
  AddEvidence(&admission, "hint_policy.advisory_only_required=true");
  AddEvidence(&admission, "hint_policy.parser_execution_authority=false");
  AddEvidence(&admission, "hint_policy.reference_authority=false");
  return admission;
}

bool SourceKindAllowed(const std::string& source_kind) {
  return source_kind == "sblr_api" ||
         source_kind == "logical_plan" ||
         source_kind == "engine_api";
}

bool SafeToken(const std::string& token) {
  if (token.empty() || token.size() > 160) return false;
  return std::all_of(token.begin(), token.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '_' || ch == ':' || ch == '.' ||
           ch == '-' || ch == '=';
  });
}

bool FirstClassHintToken(const std::string& token) {
  return token.rfind("hint:", 0) == 0 ||
         token.rfind("optimizer_hint:", 0) == 0 ||
         token.rfind("control:hint:", 0) == 0;
}

std::uint64_t StableHashAppend(std::uint64_t hash, std::string_view value) {
  for (const unsigned char ch : value) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::uint64_t StableHashAppend(std::uint64_t hash, std::uint64_t value) {
  std::ostringstream out;
  out << value;
  return StableHashAppend(hash, out.str());
}

std::string BuildPolicyDigest(const OptimizerHintPolicyRequest& request) {
  std::uint64_t hash = 1469598103934665603ull;
  hash = StableHashAppend(hash, request.policy_uuid);
  hash = StableHashAppend(hash, request.request_uuid);
  hash = StableHashAppend(hash, request.operation_id);
  hash = StableHashAppend(hash, request.sblr_digest);
  hash = StableHashAppend(hash, request.descriptor_set_digest);
  hash = StableHashAppend(hash, request.route_capability_digest);
  hash = StableHashAppend(hash, request.security_policy_digest);
  hash = StableHashAppend(hash, request.redaction_policy_digest);
  hash = StableHashAppend(hash, request.memory_policy_digest);
  hash = StableHashAppend(hash, request.cost_profile_id);
  hash = StableHashAppend(hash, request.policy_source_kind);
  hash = StableHashAppend(hash, request.policy_epoch);
  hash = StableHashAppend(hash, request.catalog_epoch);
  hash = StableHashAppend(hash, request.stats_epoch);
  hash = StableHashAppend(hash, request.security_epoch);
  hash = StableHashAppend(hash, request.redaction_epoch);
  hash = StableHashAppend(hash, request.name_resolution_epoch);
  hash = StableHashAppend(hash, request.resource_epoch);
  hash = StableHashAppend(hash, request.memory_policy_epoch);
  hash = StableHashAppend(hash, request.memory_feedback_generation);
  hash = StableHashAppend(hash, request.route_epoch);
  for (const auto& token : request.normalized_hint_tokens) {
    hash = StableHashAppend(hash, token);
  }
  std::ostringstream out;
  out << "hint-policy-fnv64:" << std::hex << hash;
  return out.str();
}

bool MissingIdentity(const OptimizerHintPolicyRequest& request) {
  return request.policy_uuid.empty() ||
         request.request_uuid.empty() ||
         request.operation_id.empty() ||
         request.sblr_digest.empty() ||
         request.descriptor_set_digest.empty() ||
         request.route_capability_digest.empty() ||
         request.security_policy_digest.empty() ||
         request.redaction_policy_digest.empty() ||
         request.memory_policy_digest.empty() ||
         request.cost_profile_id.empty();
}

bool MissingEpoch(const OptimizerHintPolicyRequest& request) {
  return request.policy_epoch == 0 ||
         request.catalog_epoch == 0 ||
         request.stats_epoch == 0 ||
         request.security_epoch == 0 ||
         request.redaction_epoch == 0 ||
         request.name_resolution_epoch == 0 ||
         request.resource_epoch == 0 ||
         request.memory_policy_epoch == 0 ||
         request.memory_feedback_generation == 0 ||
         request.route_epoch == 0;
}

}  // namespace

OptimizerHintPolicyAdmission EvaluateOptimizerHintPolicyAdmission(
    const OptimizerHintPolicyRequest& request) {
  if (MissingIdentity(request)) {
    return Refuse(request,
                  "SB_OPT_HINT_POLICY.MISSING_IDENTITY",
                  "hint_policy.identity_required");
  }
  if (MissingEpoch(request)) {
    return Refuse(request,
                  "SB_OPT_HINT_POLICY.MISSING_EPOCH",
                  "hint_policy.nonzero_epoch_evidence_required");
  }
  if (!SourceKindAllowed(request.policy_source_kind)) {
    return Refuse(request,
                  "SB_OPT_HINT_POLICY.UNSAFE_SOURCE",
                  "hint_policy.source_must_be_sblr_logical_or_engine_api");
  }
  if (!request.engine_policy_enabled || !request.advisory_only) {
    return Refuse(request,
                  "SB_OPT_HINT_POLICY.DISABLED_OR_AUTHORITATIVE",
                  "hint_policy.must_be_enabled_and_advisory_only");
  }
  if (!request.security_context_present ||
      !request.transaction_context_present ||
      !request.grants_proven) {
    return Refuse(request,
                  "SB_OPT_HINT_POLICY.MISSING_CONTEXT",
                  "hint_policy.security_transaction_and_grants_required");
  }
  if (!request.mga_visibility_recheck_required ||
      !request.exact_recheck_required ||
      !request.security_recheck_required ||
      !request.redaction_policy_bound ||
      !request.catalog_descriptor_bound) {
    return Refuse(request,
                  "SB_OPT_HINT_POLICY.MISSING_RECHECK",
                  "hint_policy.mga_exact_security_redaction_descriptor_rechecks_required");
  }
  if (request.raw_sql_text_present ||
      request.parser_execution_authority_claimed ||
      request.parser_session_directive_unbound) {
    return Refuse(request,
                  "SB_OPT_HINT_POLICY.PARSER_SQL_REFUSED",
                  "hint_policy.sql_text_or_parser_authority_refused");
  }
  if (request.reference_or_legacy_authority_claimed ||
      request.name_authority_claimed ||
      request.metric_or_benchmark_authority_claimed) {
    return Refuse(request,
                  "SB_OPT_HINT_POLICY.UNSAFE_AUTHORITY_REFUSED",
                  "hint_policy.reference_name_metric_or_benchmark_authority_refused");
  }
  if (request.normalized_hint_tokens.empty()) {
    return Refuse(request,
                  "SB_OPT_HINT_POLICY.HINT_REQUIRED",
                  "hint_policy.normalized_hint_token_required");
  }
  for (const auto& token : request.normalized_hint_tokens) {
    if (!SafeToken(token) || !FirstClassHintToken(token)) {
      return Refuse(request,
                    "SB_OPT_HINT_POLICY.UNSAFE_TOKEN",
                    "hint_policy.normalized_first_class_hint_token_required");
    }
  }

  OptimizerBarrierInput barrier_input = request.barriers;
  barrier_input.security_context_present =
      barrier_input.security_context_present || request.security_context_present;
  barrier_input.grants_proven = barrier_input.grants_proven || request.grants_proven;
  barrier_input.parser_hint_claim = true;
  barrier_input.engine_policy_accepts_hint = request.engine_policy_enabled;

  OptimizerHintPolicyAdmission admission;
  admission.barrier_decision = EvaluateOptimizerBarriers(barrier_input);
  if (!admission.barrier_decision.may_apply_hint ||
      !admission.barrier_decision.diagnostics.empty()) {
    admission = Refuse(request,
                       "SB_OPT_HINT_POLICY.BARRIER_REFUSED",
                       "hint_policy.optimizer_barrier_refused_hint");
    admission.barrier_decision = EvaluateOptimizerBarriers(barrier_input);
    for (const auto& diagnostic : admission.barrier_decision.diagnostics) {
      AddEvidence(&admission, "hint_policy.barrier_diagnostic=" + diagnostic);
    }
    return admission;
  }

  admission.accepted = true;
  admission.applied = true;
  admission.diagnostic_code = "SB_OPT_HINT_POLICY.OK";
  admission.policy_digest = BuildPolicyDigest(request);
  AddEvidence(&admission, "hint_policy.accepted=true");
  AddEvidence(&admission, "hint_policy.applied=true");
  AddEvidence(&admission, "hint_policy.source=" + request.policy_source_kind);
  AddEvidence(&admission, "hint_policy.policy_digest=" + admission.policy_digest);
  AddEvidence(&admission, "hint_policy.authority=normalized_engine_policy");
  AddEvidence(&admission, "hint_policy.advisory_only=true");
  AddEvidence(&admission, "hint_policy.raw_sql_text_present=false");
  AddEvidence(&admission, "hint_policy.parser_execution_authority=false");
  AddEvidence(&admission, "hint_policy.reference_authority=false");
  AddEvidence(&admission, "hint_policy.name_authority=false");
  AddEvidence(&admission, "hint_policy.metric_or_benchmark_authority=false");
  AddEvidence(&admission, "hint_policy.mga_visibility_recheck_required=true");
  AddEvidence(&admission, "hint_policy.exact_recheck_required=true");
  AddEvidence(&admission, "hint_policy.security_recheck_required=true");
  AddEvidence(&admission, "hint_policy.transaction_finality_authority=engine_transaction_inventory");
  AddEvidence(&admission, "hint_policy.catalog_descriptor_bound=true");
  AddEvidence(&admission, "hint_policy.redaction_policy_bound=true");
  AddEvidence(&admission, "hint_policy.barrier_clean=true");
  for (const auto& token : request.normalized_hint_tokens) {
    AddEvidence(&admission, "hint_policy.normalized_hint=" + token);
  }
  return admission;
}

}  // namespace scratchbird::engine::optimizer
