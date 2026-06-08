// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_policy_lifecycle.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iomanip>
#include <map>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

constexpr const char* kPolicyLifecycleSchemaVersion =
    "sb.agent.policy.lifecycle.v1";

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

std::string HmacSha256Digest(const std::string& key,
                             const std::string& payload) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_size = 0;
  HMAC(EVP_sha256(),
       key.data(),
       static_cast<int>(key.size()),
       reinterpret_cast<const unsigned char*>(payload.data()),
       payload.size(),
       digest,
       &digest_size);
  return HexBytes(digest, digest_size);
}

bool IsProductionLivePolicy(const AgentSignedPolicyLifecycle& lifecycle) {
  return lifecycle.production_live_policy ||
         (lifecycle.policy.activation == AgentActivationProfile::live_action &&
          lifecycle.policy.allow_live_action);
}

bool AuthorityClean(const AgentSystemProfileForbiddenAuthority& authority) {
  return !authority.transaction_finality_authority &&
         !authority.visibility_authority &&
         !authority.authorization_security_authority &&
         !authority.recovery_authority &&
         !authority.parser_authority &&
         !authority.donor_authority &&
         !authority.wal_authority &&
         !authority.benchmark_authority &&
         !authority.optimizer_plan_authority &&
         !authority.index_finality_authority &&
         !authority.provider_finality_authority &&
         !authority.cluster_authority &&
         !authority.memory_authority &&
         !authority.agent_action_authority;
}

void AppendAuthorityDigestFields(std::ostringstream* payload,
                                 const AgentSystemProfileForbiddenAuthority& a) {
  *payload << (a.transaction_finality_authority ? "1" : "0") << '\n'
           << (a.visibility_authority ? "1" : "0") << '\n'
           << (a.authorization_security_authority ? "1" : "0") << '\n'
           << (a.recovery_authority ? "1" : "0") << '\n'
           << (a.parser_authority ? "1" : "0") << '\n'
           << (a.donor_authority ? "1" : "0") << '\n'
           << (a.wal_authority ? "1" : "0") << '\n'
           << (a.benchmark_authority ? "1" : "0") << '\n'
           << (a.optimizer_plan_authority ? "1" : "0") << '\n'
           << (a.index_finality_authority ? "1" : "0") << '\n'
           << (a.provider_finality_authority ? "1" : "0") << '\n'
           << (a.cluster_authority ? "1" : "0") << '\n'
           << (a.memory_authority ? "1" : "0") << '\n'
           << (a.agent_action_authority ? "1" : "0") << '\n';
}

bool OptionalDoubleEqual(const std::optional<double>& left,
                         const std::optional<double>& right) {
  if (left.has_value() != right.has_value()) { return false; }
  if (!left.has_value()) { return true; }
  return *left == *right;
}

bool ContainsSensitivePolicyValue(const std::string& value) {
  const std::string lower = [&value] {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return out;
  }();
  return lower.find("secret:") != std::string::npos ||
         lower.find("private-key") != std::string::npos ||
         lower.find("hmac-key") != std::string::npos ||
         lower.find("unredacted") != std::string::npos;
}

bool IsProductionKeyPolicy(const AgentSignedPolicyLifecycle& lifecycle) {
  return lifecycle.key_policy_id == "agent-policy-key-policy-v1" &&
         lifecycle.key_policy_provenance == "engine_local_policy_key_policy" &&
         lifecycle.key_policy_generation == 1 &&
         lifecycle.signing_key_id == "agent-policy-signing-key-v1" &&
         lifecycle.signing_key_provenance ==
             "engine_local_policy_hmac_key" &&
         lifecycle.signing_key_generation == 1;
}

bool IsRelaxedFixtureOrTestKeyPolicy(
    const AgentSignedPolicyLifecycle& lifecycle) {
  const std::string material = lifecycle.key_policy_id + " " +
                               lifecycle.key_policy_provenance + " " +
                               lifecycle.signing_key_id + " " +
                               lifecycle.signing_key_provenance;
  return material.find("fixture") != std::string::npos ||
         material.find("test") != std::string::npos ||
         material.find("relaxed") != std::string::npos;
}

std::string PolicySigningKeyMaterial(
    const AgentSignedPolicyLifecycle& lifecycle) {
  if (IsProductionKeyPolicy(lifecycle)) {
    return "scratchbird-agent-policy-key-v1-local-protected";
  }
  if (lifecycle.key_policy_id == "agent-policy-fixture-key-policy-v1" &&
      lifecycle.key_policy_provenance == "fixture_policy_key_policy" &&
      lifecycle.key_policy_generation == 1 &&
      lifecycle.signing_key_id == "agent-policy-fixture-signing-key-v1" &&
      lifecycle.signing_key_provenance == "fixture_policy_hmac_key" &&
      lifecycle.signing_key_generation == 1) {
    return "scratchbird-agent-policy-fixture-key-v1-local-protected";
  }
  return {};
}

std::vector<AgentPolicyTypedFieldEvidence> SortedTypedFields(
    std::vector<AgentPolicyTypedFieldEvidence> fields) {
  std::sort(fields.begin(), fields.end(),
            [](const AgentPolicyTypedFieldEvidence& left,
               const AgentPolicyTypedFieldEvidence& right) {
              return left.name < right.name;
            });
  return fields;
}

AgentRuntimeStatus ValidateTypedFieldEvidence(
    const AgentSignedPolicyLifecycle& lifecycle) {
  const auto schema = AgentPolicySchemaForFamily(lifecycle.policy.policy_family);
  if (schema.empty()) {
    return AgentError("SB_AGENT_POLICY_LIFECYCLE.UNKNOWN_POLICY_SCHEMA",
                      lifecycle.policy.policy_family);
  }

  std::map<std::string, AgentPolicyFieldSchema> schema_by_name;
  for (const auto& field_schema : schema) {
    schema_by_name.emplace(field_schema.name, field_schema);
  }

  std::map<std::string, const AgentPolicyTypedFieldEvidence*> evidence_by_name;
  for (const auto& field : lifecycle.typed_fields) {
    if (field.name.empty()) {
      return AgentError("SB_AGENT_POLICY_LIFECYCLE.TYPED_FIELD_NAME_REQUIRED",
                        lifecycle.policy.policy_uuid);
    }
    const auto inserted = evidence_by_name.emplace(field.name, &field);
    if (!inserted.second) {
      return AgentError("SB_AGENT_POLICY_LIFECYCLE.DUPLICATE_TYPED_FIELD",
                        lifecycle.policy.policy_family + ":" + field.name);
    }
  }

  for (const auto& config_field : lifecycle.policy.config_fields) {
    if (evidence_by_name.find(config_field.first) == evidence_by_name.end()) {
      return AgentError("SB_AGENT_POLICY_LIFECYCLE.TYPED_FIELD_REQUIRED",
                        lifecycle.policy.policy_family + ":" +
                            config_field.first);
    }
  }

  for (const auto& field_schema : schema) {
    const auto config = lifecycle.policy.config_fields.find(field_schema.name);
    const auto evidence = evidence_by_name.find(field_schema.name);
    if (field_schema.required) {
      if (config == lifecycle.policy.config_fields.end() ||
          config->second.empty() ||
          evidence == evidence_by_name.end()) {
        return AgentError("SB_AGENT_POLICY_LIFECYCLE.TYPED_FIELD_REQUIRED",
                          lifecycle.policy.policy_family + ":" +
                              field_schema.name);
      }
    }
    if (evidence == evidence_by_name.end()) { continue; }
    if (config == lifecycle.policy.config_fields.end()) {
      return AgentError("SB_AGENT_POLICY_LIFECYCLE.UNKNOWN_TYPED_FIELD",
                        lifecycle.policy.policy_family + ":" +
                            field_schema.name);
    }

    const auto& field = *evidence->second;
    if (field.type != field_schema.type) {
      return AgentError("SB_AGENT_POLICY_LIFECYCLE.FIELD_TYPE_MISMATCH",
                        lifecycle.policy.policy_family + ":" + field.name);
    }
    if (field.units != field_schema.units) {
      return AgentError("SB_AGENT_POLICY_LIFECYCLE.FIELD_UNIT_MISMATCH",
                        lifecycle.policy.policy_family + ":" + field.name);
    }
    if (!OptionalDoubleEqual(field.minimum, field_schema.minimum) ||
        !OptionalDoubleEqual(field.maximum, field_schema.maximum)) {
      return AgentError("SB_AGENT_POLICY_LIFECYCLE.FIELD_RANGE_MISMATCH",
                        lifecycle.policy.policy_family + ":" + field.name);
    }
    if (field.sensitivity != field_schema.sensitivity) {
      return AgentError("SB_AGENT_POLICY_LIFECYCLE.FIELD_SENSITIVITY_MISMATCH",
                        lifecycle.policy.policy_family + ":" + field.name);
    }
    if (field.sensitivity == AgentPolicyFieldSensitivity::sensitive ||
        ContainsSensitivePolicyValue(field.value)) {
      return AgentError("SB_AGENT_POLICY_LIFECYCLE.SENSITIVE_VALUE_FORBIDDEN",
                        lifecycle.policy.policy_family + ":" + field.name);
    }
    if (field.value != config->second) {
      return AgentError("SB_AGENT_POLICY_LIFECYCLE.FIELD_VALUE_MISMATCH",
                        lifecycle.policy.policy_family + ":" + field.name);
    }
  }

  for (const auto& field : lifecycle.typed_fields) {
    if (schema_by_name.find(field.name) == schema_by_name.end()) {
      return AgentError("SB_AGENT_POLICY_LIFECYCLE.UNKNOWN_TYPED_FIELD",
                        lifecycle.policy.policy_family + ":" + field.name);
    }
  }

  return AgentOk();
}

AgentRuntimeStatus ValidateLifecycleStateAgainstPolicy(
    const AgentSignedPolicyLifecycle& lifecycle) {
  switch (lifecycle.lifecycle_state) {
    case AgentPolicyLifecycleState::disabled:
      if (lifecycle.policy.enabled ||
          lifecycle.policy.activation != AgentActivationProfile::disabled ||
          lifecycle.policy.action_mode != "disabled") {
        return AgentError("SB_AGENT_POLICY_LIFECYCLE.DISABLED_INCONSISTENT",
                          lifecycle.policy.policy_uuid);
      }
      return AgentOk();
    case AgentPolicyLifecycleState::advisory:
      if (!lifecycle.policy.enabled ||
          lifecycle.policy.activation == AgentActivationProfile::disabled ||
          lifecycle.policy.activation == AgentActivationProfile::live_action ||
          lifecycle.policy.allow_live_action) {
        return AgentError("SB_AGENT_POLICY_LIFECYCLE.ADVISORY_INCONSISTENT",
                          lifecycle.policy.policy_uuid);
      }
      return AgentOk();
    case AgentPolicyLifecycleState::active:
      if (!lifecycle.policy.enabled ||
          lifecycle.policy.activation == AgentActivationProfile::disabled) {
        return AgentError("SB_AGENT_POLICY_LIFECYCLE.ACTIVE_INCONSISTENT",
                          lifecycle.policy.policy_uuid);
      }
      return AgentOk();
    case AgentPolicyLifecycleState::superseded:
    case AgentPolicyLifecycleState::rollback_only:
    case AgentPolicyLifecycleState::retired:
      if (IsProductionLivePolicy(lifecycle)) {
        return AgentError("SB_AGENT_POLICY_LIFECYCLE.TERMINAL_LIVE_POLICY",
                          lifecycle.policy.policy_uuid);
      }
      return AgentOk();
  }
  return AgentError("SB_AGENT_POLICY_LIFECYCLE.UNKNOWN_LIFECYCLE_STATE",
                    lifecycle.policy.policy_uuid);
}

AgentPolicyLifecycleValidationResult ErrorResult(
    AgentPolicyLifecycleValidationResult result,
    std::string code,
    std::string detail = {}) {
  result.status = AgentError(std::move(code), std::move(detail));
  return result;
}

std::vector<std::string> BaseEvidenceFields(
    const AgentSignedPolicyLifecycle& lifecycle) {
  return {
      "source=agent_policy_lifecycle",
      "schema_version=" + lifecycle.schema_version,
      "agent_type_id=" + lifecycle.agent_type_id,
      "policy_uuid=" + lifecycle.policy.policy_uuid,
      "policy_family=" + lifecycle.policy.policy_family,
      "policy_generation=" + std::to_string(lifecycle.policy.policy_generation),
      "lifecycle_state=" +
          std::string(AgentPolicyLifecycleStateName(
              lifecycle.lifecycle_state)),
      "activation=" +
          std::string(AgentActivationProfileName(lifecycle.policy.activation)),
      "production_live_policy=" + BoolText(IsProductionLivePolicy(lifecycle)),
      "transaction_finality_authority=false",
      "visibility_authority=false",
      "authorization_security_authority=false",
      "recovery_authority=false",
      "parser_authority=false",
      "donor_authority=false",
      "wal_authority=false",
      "benchmark_authority=false",
      "optimizer_plan_authority=false",
      "index_finality_authority=false",
      "provider_finality_authority=false",
      "cluster_authority=false",
      "memory_authority=false",
      "agent_action_authority=false"};
}

}  // namespace

const char* AgentPolicyLifecycleStateName(
    AgentPolicyLifecycleState state) {
  switch (state) {
    case AgentPolicyLifecycleState::disabled: return "disabled";
    case AgentPolicyLifecycleState::advisory: return "advisory";
    case AgentPolicyLifecycleState::active: return "active";
    case AgentPolicyLifecycleState::superseded: return "superseded";
    case AgentPolicyLifecycleState::rollback_only: return "rollback_only";
    case AgentPolicyLifecycleState::retired: return "retired";
  }
  return "unknown";
}

std::string AgentPolicyLifecycleDigest(
    const AgentSignedPolicyLifecycle& lifecycle) {
  std::ostringstream payload;
  payload << "agent_policy_lifecycle_v1\n"
          << lifecycle.schema_version << '\n'
          << lifecycle.agent_type_id << '\n'
          << lifecycle.policy.policy_uuid << '\n'
          << lifecycle.policy.policy_name << '\n'
          << lifecycle.policy.policy_family << '\n'
          << lifecycle.policy.scope << '\n'
          << lifecycle.policy.action_mode << '\n'
          << lifecycle.policy.invalid_policy_behavior << '\n'
          << AgentActivationProfileName(lifecycle.policy.activation) << '\n'
          << (lifecycle.policy.enabled ? "1" : "0") << '\n'
          << (lifecycle.policy.allow_live_action ? "1" : "0") << '\n'
          << (lifecycle.policy.require_manual_approval ? "1" : "0") << '\n'
          << (lifecycle.policy.require_dry_run_before_live ? "1" : "0")
          << '\n'
          << (lifecycle.policy.evidence_required ? "1" : "0") << '\n'
          << (lifecycle.policy.explainability_required ? "1" : "0") << '\n'
          << lifecycle.policy.run_interval_microseconds << '\n'
          << lifecycle.policy.jitter_microseconds << '\n'
          << lifecycle.policy.lease_microseconds << '\n'
          << lifecycle.policy.cooldown_microseconds << '\n'
          << lifecycle.policy.max_runtime_microseconds << '\n'
          << lifecycle.policy.max_restart_attempts << '\n'
          << lifecycle.policy.initial_backoff_microseconds << '\n'
          << lifecycle.policy.max_backoff_microseconds << '\n'
          << lifecycle.policy.max_history_query_rows << '\n'
          << lifecycle.policy.max_evidence_fanout << '\n'
          << lifecycle.policy.max_label_cardinality << '\n'
          << lifecycle.policy.action_budget_per_window << '\n'
          << lifecycle.policy.policy_generation << '\n';
  for (const auto& metric : lifecycle.policy.required_metric_families) {
    payload << "metric=" << metric << '\n';
  }
  for (const auto& dependency : lifecycle.policy.policy_dependencies) {
    payload << "dependency=" << dependency << '\n';
  }
  for (const auto& field : lifecycle.policy.config_fields) {
    payload << "config=" << field.first << '=' << field.second << '\n';
  }
  for (const auto& field : SortedTypedFields(lifecycle.typed_fields)) {
    payload << "typed=" << field.name << '\n'
            << AgentPolicyFieldTypeName(field.type) << '\n'
            << field.units << '\n'
            << field.value << '\n'
            << (field.minimum.has_value() ? std::to_string(*field.minimum) : "")
            << '\n'
            << (field.maximum.has_value() ? std::to_string(*field.maximum) : "")
            << '\n'
            << AgentPolicyFieldSensitivityName(field.sensitivity) << '\n'
            << (field.required ? "1" : "0") << '\n';
  }
  payload << lifecycle.author_principal_uuid << '\n'
          << lifecycle.approver_principal_uuid << '\n'
          << lifecycle.approval_evidence_uuid << '\n'
          << AgentPolicyLifecycleStateName(lifecycle.lifecycle_state) << '\n'
          << lifecycle.generation << '\n'
          << lifecycle.issued_at_microseconds << '\n'
          << lifecycle.activation_time_microseconds << '\n'
          << lifecycle.expiry_time_microseconds << '\n'
          << lifecycle.max_staleness_microseconds << '\n'
          << lifecycle.supersedes_policy_uuid << '\n'
          << lifecycle.supersedes_policy_generation << '\n'
          << lifecycle.rollback_policy_uuid << '\n'
          << lifecycle.rollback_policy_generation << '\n'
          << lifecycle.engine_min_generation << '\n'
          << lifecycle.engine_max_generation << '\n'
          << lifecycle.coupled_profile_agent_type_id << '\n'
          << lifecycle.coupled_profile_generation << '\n'
          << lifecycle.coupled_profile_digest << '\n'
          << lifecycle.key_policy_id << '\n'
          << lifecycle.key_policy_provenance << '\n'
          << lifecycle.key_policy_generation << '\n'
          << lifecycle.signing_key_id << '\n'
          << lifecycle.signing_key_provenance << '\n'
          << lifecycle.signing_key_generation << '\n'
          << lifecycle.policy_digest_algorithm << '\n'
          << lifecycle.policy_signature_algorithm << '\n'
          << (lifecycle.production_live_policy ? "1" : "0") << '\n'
          << (lifecycle.local_cluster_policy_claim ? "1" : "0") << '\n';
  AppendAuthorityDigestFields(&payload, lifecycle.no_authority);
  return Sha256Digest(payload.str());
}

std::string AgentPolicyLifecycleSignatureDigest(
    const AgentSignedPolicyLifecycle& lifecycle) {
  const std::string key_material = PolicySigningKeyMaterial(lifecycle);
  if (key_material.empty()) { return {}; }
  std::ostringstream payload;
  payload << "agent_policy_lifecycle_signature_v1\n"
          << lifecycle.schema_version << '\n'
          << lifecycle.agent_type_id << '\n'
          << lifecycle.policy.policy_uuid << '\n'
          << lifecycle.generation << '\n'
          << lifecycle.policy_digest << '\n'
          << lifecycle.key_policy_id << '\n'
          << lifecycle.key_policy_generation << '\n'
          << lifecycle.signing_key_id << '\n'
          << lifecycle.signing_key_generation << '\n'
          << AgentPolicyLifecycleStateName(lifecycle.lifecycle_state) << '\n'
          << (IsProductionLivePolicy(lifecycle) ? "1" : "0") << '\n';
  return HmacSha256Digest(key_material, payload.str());
}

void FinalizeAgentSignedPolicyLifecycle(
    AgentSignedPolicyLifecycle* lifecycle) {
  if (lifecycle == nullptr) { return; }
  lifecycle->policy_digest = AgentPolicyLifecycleDigest(*lifecycle);
  lifecycle->policy_signature =
      AgentPolicyLifecycleSignatureDigest(*lifecycle);
}

std::vector<AgentPolicyTypedFieldEvidence>
AgentPolicyTypedFieldEvidenceFromPolicy(const AgentPolicy& policy) {
  std::vector<AgentPolicyTypedFieldEvidence> fields;
  for (const auto& config : policy.config_fields) {
    const auto schema = FindAgentPolicyFieldSchema(policy.policy_family,
                                                   config.first);
    if (!schema.has_value()) { continue; }
    AgentPolicyTypedFieldEvidence evidence;
    evidence.name = config.first;
    evidence.type = schema->type;
    evidence.units = schema->units;
    evidence.value = config.second;
    evidence.minimum = schema->minimum;
    evidence.maximum = schema->maximum;
    evidence.sensitivity = schema->sensitivity;
    evidence.required = schema->required;
    fields.push_back(std::move(evidence));
  }
  return SortedTypedFields(std::move(fields));
}

AgentPolicyLifecycleValidationResult ValidateAgentSignedPolicyLifecycle(
    const AgentSignedPolicyLifecycle& lifecycle,
    const AgentPolicyLifecycleValidationContext& context) {
  AgentPolicyLifecycleValidationResult result;
  result.production_live_policy = IsProductionLivePolicy(lifecycle);
  result.evidence_fields = BaseEvidenceFields(lifecycle);

  if (lifecycle.schema_version != kPolicyLifecycleSchemaVersion) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.UNKNOWN_SCHEMA",
                       lifecycle.schema_version);
  }

  const auto registry_status = ValidateCanonicalAgentRegistry();
  if (!registry_status.ok) {
    result.status = registry_status;
    return result;
  }

  const auto descriptor = FindAgentType(lifecycle.agent_type_id);
  if (!descriptor.has_value()) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.NON_CANONICAL_AGENT",
                       lifecycle.agent_type_id);
  }

  if (lifecycle.generation == 0 ||
      lifecycle.policy.policy_generation == 0 ||
      lifecycle.generation != lifecycle.policy.policy_generation) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.GENERATION_MISMATCH",
                       lifecycle.policy.policy_uuid);
  }

  const auto policy_status = ValidateAgentPolicy(lifecycle.policy, *descriptor);
  if (!policy_status.ok) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.POLICY_INVALID",
                       policy_status.diagnostic_code + ":" +
                           policy_status.detail);
  }

  const auto lifecycle_state_status =
      ValidateLifecycleStateAgainstPolicy(lifecycle);
  if (!lifecycle_state_status.ok) {
    return ErrorResult(std::move(result),
                       lifecycle_state_status.diagnostic_code,
                       lifecycle_state_status.detail);
  }

  const auto typed_status = ValidateTypedFieldEvidence(lifecycle);
  if (!typed_status.ok) {
    return ErrorResult(std::move(result), typed_status.diagnostic_code,
                       typed_status.detail);
  }
  result.typed_fields_valid = true;

  if (lifecycle.author_principal_uuid.empty() ||
      lifecycle.approver_principal_uuid.empty() ||
      lifecycle.approval_evidence_uuid.empty()) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.AUTHOR_APPROVER_REQUIRED",
                       lifecycle.policy.policy_uuid);
  }

  if (lifecycle.issued_at_microseconds == 0 ||
      lifecycle.activation_time_microseconds == 0 ||
      lifecycle.expiry_time_microseconds == 0 ||
      lifecycle.max_staleness_microseconds == 0 ||
      lifecycle.activation_time_microseconds < lifecycle.issued_at_microseconds ||
      lifecycle.expiry_time_microseconds <= lifecycle.activation_time_microseconds) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.TIME_BOUNDS_REQUIRED",
                       lifecycle.policy.policy_uuid);
  }
  if (context.now_microseconds != 0 &&
      context.now_microseconds < lifecycle.activation_time_microseconds) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.NOT_YET_ACTIVE",
                       lifecycle.policy.policy_uuid);
  }
  if (context.now_microseconds != 0 &&
      context.now_microseconds > lifecycle.expiry_time_microseconds) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.EXPIRED",
                       lifecycle.policy.policy_uuid);
  }
  if (context.now_microseconds != 0 &&
      context.now_microseconds >
          lifecycle.issued_at_microseconds +
              lifecycle.max_staleness_microseconds) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.STALE",
                       lifecycle.policy.policy_uuid);
  }
  result.lifecycle_time_valid = true;

  if ((!lifecycle.supersedes_policy_uuid.empty() &&
       (lifecycle.supersedes_policy_generation == 0 ||
        lifecycle.supersedes_policy_generation >= lifecycle.generation)) ||
      (lifecycle.supersedes_policy_uuid.empty() &&
       lifecycle.supersedes_policy_generation != 0)) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.SUPERSEDES_INVALID",
                       lifecycle.policy.policy_uuid);
  }
  if (result.production_live_policy &&
      lifecycle.rollback_policy_uuid.empty()) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.ROLLBACK_POLICY_REQUIRED",
                       lifecycle.policy.policy_uuid);
  }
  if ((!lifecycle.rollback_policy_uuid.empty() &&
       (lifecycle.rollback_policy_generation == 0 ||
        lifecycle.rollback_policy_generation >= lifecycle.generation)) ||
      (lifecycle.rollback_policy_uuid.empty() &&
       lifecycle.rollback_policy_generation != 0)) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.ROLLBACK_INVALID",
                       lifecycle.policy.policy_uuid);
  }

  if (lifecycle.engine_min_generation == 0 ||
      lifecycle.engine_max_generation == 0 ||
      lifecycle.engine_min_generation > lifecycle.engine_max_generation ||
      context.engine_generation == 0 ||
      context.engine_generation < lifecycle.engine_min_generation ||
      context.engine_generation > lifecycle.engine_max_generation) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.ENGINE_BOUNDS_VIOLATION",
                       lifecycle.policy.policy_uuid);
  }
  result.engine_bounds_valid = true;

  if (lifecycle.key_policy_id.empty() ||
      lifecycle.key_policy_provenance.empty() ||
      lifecycle.key_policy_generation == 0 ||
      lifecycle.signing_key_id.empty() ||
      lifecycle.signing_key_provenance.empty() ||
      lifecycle.signing_key_generation == 0) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.KEY_POLICY_REQUIRED",
                       lifecycle.policy.policy_uuid);
  }
  if ((context.production_environment || result.production_live_policy) &&
      (!IsProductionKeyPolicy(lifecycle) ||
       IsRelaxedFixtureOrTestKeyPolicy(lifecycle))) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.PRODUCTION_KEY_POLICY_REQUIRED",
                       lifecycle.policy.policy_uuid);
  }

  result.digest_valid =
      lifecycle.policy_digest_algorithm == "sha256-v1" &&
      !lifecycle.policy_digest.empty() &&
      lifecycle.policy_digest == AgentPolicyLifecycleDigest(lifecycle);
  if (!result.digest_valid) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.POLICY_DIGEST_INVALID",
                       lifecycle.policy.policy_uuid);
  }

  result.signature_valid =
      lifecycle.policy_signature_algorithm == "hmac-sha256-v1" &&
      !lifecycle.policy_signature.empty() &&
      lifecycle.policy_signature == AgentPolicyLifecycleSignatureDigest(lifecycle);
  if (!result.signature_valid) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.POLICY_SIGNATURE_INVALID",
                       lifecycle.policy.policy_uuid);
  }

  result.authority_clean = AuthorityClean(lifecycle.no_authority);
  if (!result.authority_clean) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.FORBIDDEN_AUTHORITY",
                       lifecycle.policy.policy_uuid);
  }

  if (lifecycle.local_cluster_policy_claim) {
    return ErrorResult(std::move(result),
                       "SB_AGENT_POLICY_LIFECYCLE.LOCAL_CLUSTER_POLICY_FORBIDDEN",
                       lifecycle.policy.policy_uuid);
  }

  if (result.production_live_policy) {
    if (context.system_profile == nullptr) {
      return ErrorResult(
          std::move(result),
          "SB_AGENT_POLICY_LIFECYCLE.PROFILE_VALIDATION_REQUIRED",
          lifecycle.policy.policy_uuid);
    }
    if (lifecycle.coupled_profile_agent_type_id !=
            context.system_profile->agent_type_id ||
        lifecycle.coupled_profile_generation !=
            context.system_profile->profile_generation ||
        lifecycle.coupled_profile_digest !=
            context.system_profile->profile_digest) {
      return ErrorResult(std::move(result),
                         "SB_AGENT_POLICY_LIFECYCLE.PROFILE_GENERATION_MISMATCH",
                         lifecycle.policy.policy_uuid);
    }
    result.profile_coupling_valid = true;

    auto profile_context = context.profile_context;
    if (profile_context.now_microseconds == 0) {
      profile_context.now_microseconds = context.now_microseconds;
    }
    const auto profile_result =
        ValidateAgentSystemProfileClaim(*context.system_profile,
                                        profile_context);
    if (!profile_result.status.ok) {
      return ErrorResult(
          std::move(result),
          "SB_AGENT_POLICY_LIFECYCLE.PROFILE_VALIDATION_FAILED",
          profile_result.status.diagnostic_code + ":" +
              profile_result.status.detail);
    }
    if (!profile_result.production_live_claim) {
      return ErrorResult(
          std::move(result),
          "SB_AGENT_POLICY_LIFECYCLE.PROFILE_PRODUCTION_LIVE_REQUIRED",
          context.system_profile->agent_type_id);
    }
    result.profile_validation_passed = true;
  } else if (context.system_profile != nullptr) {
    if (lifecycle.coupled_profile_agent_type_id !=
            context.system_profile->agent_type_id ||
        lifecycle.coupled_profile_generation !=
            context.system_profile->profile_generation ||
        lifecycle.coupled_profile_digest !=
            context.system_profile->profile_digest) {
      return ErrorResult(std::move(result),
                         "SB_AGENT_POLICY_LIFECYCLE.PROFILE_GENERATION_MISMATCH",
                         lifecycle.policy.policy_uuid);
    }
    result.profile_coupling_valid = true;
  }

  result.status = AgentRuntimeStatus{
      true, "SB_AGENT_POLICY_LIFECYCLE.VALIDATED",
      lifecycle.policy.policy_uuid};
  return result;
}

}  // namespace scratchbird::core::agents
