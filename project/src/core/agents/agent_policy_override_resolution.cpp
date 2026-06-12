// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_policy_override_resolution.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <map>
#include <openssl/sha.h>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

std::string HexBytes(const unsigned char* bytes, std::size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

std::string Sha256Digest(const std::string& payload) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(payload.data()),
         payload.size(),
         digest);
  return HexBytes(digest, SHA256_DIGEST_LENGTH);
}

int ScopeRank(AgentPolicyScopeKind kind) {
  switch (kind) {
    case AgentPolicyScopeKind::root: return 0;
    case AgentPolicyScopeKind::database: return 1;
    case AgentPolicyScopeKind::filespace: return 2;
    case AgentPolicyScopeKind::tenant: return 3;
    case AgentPolicyScopeKind::application: return 4;
    case AgentPolicyScopeKind::session: return 5;
    case AgentPolicyScopeKind::unknown: return -1;
  }
  return -1;
}

bool ContainsField(const std::vector<std::string>& fields,
                   const std::string& field) {
  return std::find(fields.begin(), fields.end(), field) != fields.end() ||
         std::find(fields.begin(), fields.end(), "*") != fields.end();
}

bool AuthorityClean(const AgentSystemProfileForbiddenAuthority& authority) {
  return !authority.transaction_finality_authority &&
         !authority.visibility_authority &&
         !authority.authorization_security_authority &&
         !authority.recovery_authority &&
         !authority.parser_authority &&
         !authority.reference_authority &&
         !authority.wal_authority &&
         !authority.benchmark_authority &&
         !authority.optimizer_plan_authority &&
         !authority.index_finality_authority &&
         !authority.provider_finality_authority &&
         !authority.cluster_authority &&
         !authority.memory_authority &&
         !authority.agent_action_authority;
}

void AddConflict(AgentPolicyOverrideResolutionResult* result,
                 AgentPolicyScopeKind scope_kind,
                 const std::string& scope_uuid,
                 const std::string& field_name,
                 std::string code,
                 std::string detail) {
  AgentPolicyOverrideConflict conflict;
  conflict.scope_kind = scope_kind;
  conflict.scope_uuid = scope_uuid;
  conflict.field_name = field_name;
  conflict.diagnostic_code = std::move(code);
  conflict.detail = std::move(detail);
  result->conflicts.push_back(std::move(conflict));
}

AgentPolicyOverrideResolutionResult ErrorResult(
    AgentPolicyOverrideResolutionResult result,
    AgentPolicyScopeKind scope_kind,
    const std::string& scope_uuid,
    const std::string& field_name,
    std::string code,
    std::string detail = {}) {
  AddConflict(&result, scope_kind, scope_uuid, field_name, code, detail);
  result.status = AgentError(std::move(code), std::move(detail));
  result.conflicts_absent = false;
  return result;
}

std::vector<std::string> BaseEvidenceFields() {
  return {
      "source=agent_policy_override_resolution",
      "schema_version=sb.agent.policy.override_resolution.v1",
      "transaction_finality_authority=false",
      "visibility_authority=false",
      "authorization_security_authority=false",
      "recovery_authority=false",
      "parser_authority=false",
      "reference_authority=false",
      "wal_authority=false",
      "benchmark_authority=false",
      "optimizer_plan_authority=false",
      "index_finality_authority=false",
      "provider_finality_authority=false",
      "cluster_authority=false",
      "memory_authority=false",
      "agent_action_authority=false"};
}

bool StructuralPolicyFieldsMatch(const AgentPolicy& base,
                                 const AgentPolicy& candidate) {
  return base.policy_family == candidate.policy_family &&
         base.action_mode == candidate.action_mode &&
         base.invalid_policy_behavior == candidate.invalid_policy_behavior &&
         base.activation == candidate.activation &&
         base.enabled == candidate.enabled &&
         base.allow_live_action == candidate.allow_live_action &&
         base.require_manual_approval == candidate.require_manual_approval &&
         base.require_dry_run_before_live ==
             candidate.require_dry_run_before_live &&
         base.evidence_required == candidate.evidence_required &&
         base.explainability_required == candidate.explainability_required;
}

struct FieldState {
  std::string value;
  AgentPolicyScopeKind source_scope_kind = AgentPolicyScopeKind::unknown;
  std::string source_scope_uuid;
  std::string source_policy_uuid;
  u64 source_policy_generation = 0;
  bool overridden = false;
  std::string prior_value;
  std::string approval_uuid;
};

}  // namespace

const char* AgentPolicyScopeKindName(AgentPolicyScopeKind kind) {
  switch (kind) {
    case AgentPolicyScopeKind::root: return "root";
    case AgentPolicyScopeKind::database: return "database";
    case AgentPolicyScopeKind::filespace: return "filespace";
    case AgentPolicyScopeKind::tenant: return "tenant";
    case AgentPolicyScopeKind::application: return "application";
    case AgentPolicyScopeKind::session: return "session";
    case AgentPolicyScopeKind::unknown: return "unknown";
  }
  return "unknown";
}

std::string AgentPolicyOverrideResolutionDigest(
    const std::vector<AgentPolicyOverrideLayer>& layers) {
  std::ostringstream payload;
  payload << "agent_policy_override_resolution_v1\n";
  for (const auto& layer : layers) {
    payload << AgentPolicyScopeKindName(layer.scope_kind) << '\n'
            << layer.scope_uuid << '\n'
            << layer.lifecycle.agent_type_id << '\n'
            << layer.lifecycle.policy.policy_uuid << '\n'
            << layer.lifecycle.policy.policy_generation << '\n'
            << layer.lifecycle.policy_digest << '\n'
            << layer.lifecycle.policy_signature << '\n'
            << (layer.approval.override_permission_granted ? "1" : "0")
            << '\n'
            << layer.approval.approval_uuid << '\n'
            << layer.approval.approver_principal_uuid << '\n'
            << layer.approval.created_at_microseconds << '\n'
            << layer.approval.expires_at_microseconds << '\n';
    for (const auto& field : layer.approval.allowed_override_fields) {
      payload << "allowed=" << field << '\n';
    }
  }
  return Sha256Digest(payload.str());
}

AgentPolicyOverrideResolutionResult ResolveAgentPolicyOverrides(
    const std::vector<AgentPolicyOverrideLayer>& layers,
    const AgentPolicyOverrideResolutionContext& context) {
  AgentPolicyOverrideResolutionResult result;
  result.evidence_fields = BaseEvidenceFields();
  result.conflicts_absent = true;

  if (layers.empty()) {
    return ErrorResult(
        std::move(result), AgentPolicyScopeKind::unknown, {}, {},
        "SB_AGENT_POLICY_OVERRIDE.ROOT_REQUIRED",
        "root_policy_layer_required");
  }
  if (layers.front().scope_kind != AgentPolicyScopeKind::root) {
    return ErrorResult(
        std::move(result), layers.front().scope_kind, layers.front().scope_uuid,
        {},
        "SB_AGENT_POLICY_OVERRIDE.ROOT_REQUIRED",
        "first_policy_layer_must_be_root");
  }
  result.root_present = true;

  int previous_rank = -1;
  std::set<int> seen_ranks;
  for (const auto& layer : layers) {
    const int rank = ScopeRank(layer.scope_kind);
    if (rank < 0 || layer.scope_uuid.empty()) {
      return ErrorResult(
          std::move(result), layer.scope_kind, layer.scope_uuid, {},
          "SB_AGENT_POLICY_OVERRIDE.SCOPE_REQUIRED",
          "scope_kind_and_scope_uuid_required");
    }
    if (rank <= previous_rank) {
      const std::string code =
          seen_ranks.find(rank) != seen_ranks.end()
              ? "SB_AGENT_POLICY_OVERRIDE.DUPLICATE_SCOPE"
              : "SB_AGENT_POLICY_OVERRIDE.SCOPE_ORDER_INVALID";
      return ErrorResult(std::move(result), layer.scope_kind, layer.scope_uuid,
                         {}, code, "scope_order_must_be_root_to_session");
    }
    previous_rank = rank;
    seen_ranks.insert(rank);
  }
  result.order_valid = true;

  AgentPolicyLifecycleValidationContext lifecycle_context =
      context.lifecycle_context;
  lifecycle_context.production_environment =
      lifecycle_context.production_environment || context.production_environment;
  if (lifecycle_context.now_microseconds == 0) {
    lifecycle_context.now_microseconds = context.now_microseconds;
  }

  std::vector<AgentPolicyLifecycleValidationResult> lifecycle_results;
  lifecycle_results.reserve(layers.size());
  for (const auto& layer : layers) {
    if (!AuthorityClean(layer.lifecycle.no_authority)) {
      return ErrorResult(
          std::move(result), layer.scope_kind, layer.scope_uuid, {},
          "SB_AGENT_POLICY_OVERRIDE.FORBIDDEN_AUTHORITY",
          layer.lifecycle.policy.policy_uuid);
    }
    if (layer.lifecycle.local_cluster_policy_claim) {
      return ErrorResult(
          std::move(result), layer.scope_kind, layer.scope_uuid, {},
          "SB_AGENT_POLICY_OVERRIDE.LOCAL_CLUSTER_POLICY_FORBIDDEN",
          layer.lifecycle.policy.policy_uuid);
    }
    const auto lifecycle_result =
        ValidateAgentSignedPolicyLifecycle(layer.lifecycle, lifecycle_context);
    if (!lifecycle_result.status.ok) {
      return ErrorResult(
          std::move(result), layer.scope_kind, layer.scope_uuid, {},
          "SB_AGENT_POLICY_OVERRIDE.INVALID_LIFECYCLE",
          lifecycle_result.status.diagnostic_code + ":" +
              lifecycle_result.status.detail);
    }
    if (!lifecycle_result.authority_clean) {
      return ErrorResult(
          std::move(result), layer.scope_kind, layer.scope_uuid, {},
          "SB_AGENT_POLICY_OVERRIDE.FORBIDDEN_AUTHORITY",
          layer.lifecycle.policy.policy_uuid);
    }
    lifecycle_results.push_back(lifecycle_result);
  }
  result.all_lifecycles_valid = true;
  result.authority_clean = true;

  const auto& root_layer = layers.front();
  const AgentPolicy& root_policy = root_layer.lifecycle.policy;
  std::map<std::string, FieldState> current_fields;
  for (const auto& config : root_policy.config_fields) {
    FieldState state;
    state.value = config.second;
    state.source_scope_kind = root_layer.scope_kind;
    state.source_scope_uuid = root_layer.scope_uuid;
    state.source_policy_uuid = root_policy.policy_uuid;
    state.source_policy_generation = root_policy.policy_generation;
    current_fields.emplace(config.first, std::move(state));
  }

  result.applied_layers.push_back(
      std::string(AgentPolicyScopeKindName(root_layer.scope_kind)) + ":" +
      root_layer.scope_uuid + ":" + root_policy.policy_uuid);

  bool approvals_required = false;
  for (std::size_t i = 1; i < layers.size(); ++i) {
    const auto& layer = layers[i];
    const auto& policy = layer.lifecycle.policy;
    if (layer.lifecycle.agent_type_id != root_layer.lifecycle.agent_type_id ||
        policy.policy_family != root_policy.policy_family) {
      return ErrorResult(
          std::move(result), layer.scope_kind, layer.scope_uuid, {},
          "SB_AGENT_POLICY_OVERRIDE.AGENT_POLICY_MISMATCH",
          layer.lifecycle.agent_type_id + ":" + policy.policy_family);
    }
    if (!StructuralPolicyFieldsMatch(root_policy, policy)) {
      return ErrorResult(
          std::move(result), layer.scope_kind, layer.scope_uuid, {},
          "SB_AGENT_POLICY_OVERRIDE.STRUCTURAL_OVERRIDE_FORBIDDEN",
          policy.policy_uuid);
    }

    std::vector<std::string> changed_fields;
    for (const auto& config : policy.config_fields) {
      const auto root_field = root_policy.config_fields.find(config.first);
      if (root_field == root_policy.config_fields.end() ||
          root_field->second != config.second) {
        changed_fields.push_back(config.first);
      }
    }
    std::sort(changed_fields.begin(), changed_fields.end());

    if (!changed_fields.empty()) {
      approvals_required = true;
      if (!layer.approval.override_permission_granted) {
        return ErrorResult(
            std::move(result), layer.scope_kind, layer.scope_uuid,
            changed_fields.front(),
            "SB_AGENT_POLICY_OVERRIDE.PERMISSION_REQUIRED",
            policy.policy_uuid);
      }
      if (layer.approval.approval_uuid.empty() ||
          layer.approval.approver_principal_uuid.empty() ||
          layer.approval.created_at_microseconds == 0 ||
          layer.approval.expires_at_microseconds == 0 ||
          layer.approval.expires_at_microseconds <=
              layer.approval.created_at_microseconds) {
        return ErrorResult(
            std::move(result), layer.scope_kind, layer.scope_uuid,
            changed_fields.front(),
            "SB_AGENT_POLICY_OVERRIDE.APPROVAL_REQUIRED",
            policy.policy_uuid);
      }
      if (context.now_microseconds != 0 &&
          context.now_microseconds > layer.approval.expires_at_microseconds) {
        return ErrorResult(
            std::move(result), layer.scope_kind, layer.scope_uuid,
            changed_fields.front(),
            "SB_AGENT_POLICY_OVERRIDE.APPROVAL_EXPIRED",
            layer.approval.approval_uuid);
      }
      for (const auto& field : changed_fields) {
        if (!ContainsField(layer.approval.allowed_override_fields, field)) {
          return ErrorResult(
              std::move(result), layer.scope_kind, layer.scope_uuid, field,
              "SB_AGENT_POLICY_OVERRIDE.FIELD_NOT_PERMITTED",
              layer.approval.approval_uuid);
        }
      }
    }

    for (const auto& field : changed_fields) {
      const auto config = policy.config_fields.find(field);
      FieldState state;
      state.value = config->second;
      state.source_scope_kind = layer.scope_kind;
      state.source_scope_uuid = layer.scope_uuid;
      state.source_policy_uuid = policy.policy_uuid;
      state.source_policy_generation = policy.policy_generation;
      state.overridden = true;
      const auto previous = current_fields.find(field);
      if (previous != current_fields.end()) {
        state.prior_value = previous->second.value;
      }
      state.approval_uuid = layer.approval.approval_uuid;
      current_fields[field] = std::move(state);
    }

    result.applied_layers.push_back(
        std::string(AgentPolicyScopeKindName(layer.scope_kind)) + ":" +
        layer.scope_uuid + ":" + policy.policy_uuid);
  }
  result.approvals_valid = !approvals_required ||
                           std::any_of(layers.begin() + 1, layers.end(),
                                       [](const AgentPolicyOverrideLayer& layer) {
                                         return layer.approval
                                             .override_permission_granted;
                                       });

  result.resolved_lifecycle = root_layer.lifecycle;
  result.resolved_lifecycle.policy.config_fields.clear();
  for (const auto& entry : current_fields) {
    result.resolved_lifecycle.policy.config_fields[entry.first] =
        entry.second.value;
  }
  result.resolved_lifecycle.policy.scope =
      std::string(AgentPolicyScopeKindName(layers.back().scope_kind)) + ":" +
      layers.back().scope_uuid;
  result.resolved_lifecycle.policy.policy_generation =
      layers.back().lifecycle.policy.policy_generation;
  result.resolved_lifecycle.generation =
      result.resolved_lifecycle.policy.policy_generation;
  result.resolved_lifecycle.typed_fields =
      AgentPolicyTypedFieldEvidenceFromPolicy(result.resolved_lifecycle.policy);
  FinalizeAgentSignedPolicyLifecycle(&result.resolved_lifecycle);

  for (const auto& entry : current_fields) {
    AgentPolicyResolvedField field;
    field.field_name = entry.first;
    field.value = entry.second.value;
    field.source_scope_kind = entry.second.source_scope_kind;
    field.source_scope_uuid = entry.second.source_scope_uuid;
    field.source_policy_uuid = entry.second.source_policy_uuid;
    field.source_policy_generation = entry.second.source_policy_generation;
    field.overridden = entry.second.overridden;
    field.prior_value = entry.second.prior_value;
    field.approval_uuid = entry.second.approval_uuid;
    result.resolved_fields.push_back(std::move(field));
  }

  result.resolved_policy_digest =
      AgentPolicyOverrideResolutionDigest(layers) + ":" +
      result.resolved_lifecycle.policy_digest;
  result.resolved_policy_uuid = DeterministicAgentRuntimeObjectUuidFromKey(
      "agent_policy_override_resolution|" +
      result.resolved_lifecycle.agent_type_id + "|" +
      result.resolved_policy_digest);
  result.digest_valid = !result.resolved_policy_digest.empty();
  result.conflicts_absent = result.conflicts.empty();

  result.evidence_fields.push_back("root_present=true");
  result.evidence_fields.push_back("order_valid=true");
  result.evidence_fields.push_back("all_lifecycles_valid=true");
  result.evidence_fields.push_back("approvals_valid=" +
                                   BoolText(result.approvals_valid));
  result.evidence_fields.push_back("conflicts_absent=true");
  result.evidence_fields.push_back("authority_clean=true");
  result.evidence_fields.push_back("resolved_policy_uuid=" +
                                   result.resolved_policy_uuid);
  result.evidence_fields.push_back("resolved_policy_digest=" +
                                   result.resolved_policy_digest);
  for (const auto& layer : result.applied_layers) {
    result.evidence_fields.push_back("applied_layer=" + layer);
  }
  for (const auto& field : result.resolved_fields) {
    result.evidence_fields.push_back(
        "resolved_field=" + field.field_name + ":" +
        AgentPolicyScopeKindName(field.source_scope_kind) + ":" +
        field.source_scope_uuid + ":overridden=" + BoolText(field.overridden));
  }

  result.status = AgentRuntimeStatus{
      true,
      "SB_AGENT_POLICY_OVERRIDE.RESOLVED",
      result.resolved_policy_uuid};
  return result;
}

}  // namespace scratchbird::core::agents
