// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/policy_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "catalog/pinned_descriptor_cache.hpp"
#include "security/security_model.hpp"

namespace scratchbird::engine::internal_api {
namespace {

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string PolicyOptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, prefix)) { return option.substr(prefix.size()); }
  }
  return {};
}

bool FilesystemPolicyPackOptionPresent(const EngineApiRequest& request) {
  for (const auto& option : request.option_envelopes) {
    if (option == "reload_policy_pack" ||
        option == "filesystem_policy_pack" ||
        StartsWith(option, "policy_pack_root:") ||
        StartsWith(option, "filesystem_policy_pack:") ||
        StartsWith(option, "post_create_policy_pack_root:")) {
      return true;
    }
  }
  return false;
}

std::string PolicyMutationKind(const EnginePolicyMutationRequest& request) {
  if (!request.mutation_kind.empty()) { return request.mutation_kind; }
  const auto from_option = PolicyOptionValue(request, "policy_mutation:");
  return from_option.empty() ? "modify" : from_option;
}

std::string PolicyArea(const EnginePolicyMutationRequest& request) {
  if (!request.policy_area.empty()) { return request.policy_area; }
  return PolicyOptionValue(request, "policy_area:");
}

std::string PolicyMode(const EnginePolicyMutationRequest& request) {
  if (!request.policy_mode.empty()) { return request.policy_mode; }
  return PolicyOptionValue(request, "policy_mode:");
}

std::string PolicyEnvelope(const EnginePolicyMutationRequest& request) {
  if (!request.canonical_policy_envelope.empty()) {
    return request.canonical_policy_envelope;
  }
  return PolicyOptionValue(request, "canonical_policy_envelope:");
}

bool ValidPolicyMutationKind(const std::string& kind) {
  return kind == "create" || kind == "modify" || kind == "remove";
}

EnginePolicyMutationResult PolicyMutationFailure(const EnginePolicyMutationRequest& request,
                                                 std::string detail) {
  EnginePolicyMutationResult result = SecurityFailure<EnginePolicyMutationResult>(
      request.context,
      request.operation_id.empty() ? "security.policy_mutation" : request.operation_id,
      MakeSecurityDiagnostic("SECURITY.POLICY.MUTATION_REFUSED", std::move(detail)));
  result.filesystem_pack_rejected = FilesystemPolicyPackOptionPresent(request);
  return result;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_POLICY_API_BEHAVIOR
EngineEvaluatePolicyResult EngineEvaluatePolicy(const EngineEvaluatePolicyRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineEvaluatePolicyResult>(request.context, "security.evaluate_policy");
  for (const auto& profile : request.policy_profile.encoded_profiles) {
    if (profile.find("invalid") != std::string::npos || profile.find("unsafe") != std::string::npos) {
      result.ok = false;
      result.diagnostics.push_back(MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", "invalid_policy_profile"));
      AddApiBehaviorEvidence(&result, "policy_decision", "deny_invalid_profile");
      AddApiBehaviorRow(&result, {{"decision", "deny"}, {"reason", "invalid_policy_profile"}});
      return result;
    }
  }
  if (!request.context.security_context_present && !request.target_object.uuid.canonical.empty()) {
    result.ok = false;
    result.diagnostics.push_back(MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID", "security_context_required"));
    AddApiBehaviorEvidence(&result, "policy_decision", "deny_missing_security_context");
    AddApiBehaviorRow(&result, {{"decision", "deny"}, {"reason", "security_context_required"}});
    return result;
  }
  if (!request.target_object.uuid.canonical.empty() && !SecurityContextHasRight(request.context, "POLICY_ADMIN")) {
    result.ok = false;
    result.diagnostics.push_back(MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", "POLICY_ADMIN"));
    AddApiBehaviorEvidence(&result, "policy_decision", "deny_missing_policy_admin");
    AddApiBehaviorRow(&result, {{"decision", "deny"}, {"reason", "POLICY_ADMIN"}});
    return result;
  }
  AddApiBehaviorEvidence(&result, "policy_decision", "allow_explicit_policy");
  AddApiBehaviorRow(&result, {{"decision", "allow"}, {"policy_profile_count", std::to_string(request.policy_profile.encoded_profiles.size())}, {"payload", ApiBehaviorPayloadFromRequest(request)}});
  return result;
}

// SEARCH_KEY: POLICY_CATALOG_MUTATION
EnginePolicyMutationResult EngineMutatePolicy(const EnginePolicyMutationRequest& request) {
  const std::string operation_id =
      request.operation_id.empty() ? "security.policy_mutation" : request.operation_id;
  if (request.context.read_only_mode) {
    return PolicyMutationFailure(request, "read_only_context");
  }
  if (FilesystemPolicyPackOptionPresent(request)) {
    auto result = PolicyMutationFailure(request, "filesystem_policy_pack_not_post_create_authority");
    AddSecurityEvidence(&result, "filesystem_policy_pack_rejected", "post_create_not_authority");
    AddSecurityRow(&result,
                   {{"decision", "refuse"},
                    {"reason", "filesystem_policy_pack_not_post_create_authority"},
                    {"post_create_filesystem_authority", "false"},
                    {"database_command_required", "true"}});
    return result;
  }
  if (request.context.local_transaction_id == 0) {
    return PolicyMutationFailure(request, "local_transaction_id_required");
  }
  if (!request.context.security_context_present) {
    return PolicyMutationFailure(request, "security_context_required");
  }
  if (!SecurityContextHasRight(request.context, "POLICY_ADMIN",
                               request.target_object.uuid.canonical)) {
    return PolicyMutationFailure(request, "POLICY_ADMIN");
  }
  if (request.context.catalog_generation_id == 0 ||
      request.context.security_epoch == 0) {
    return PolicyMutationFailure(request, "catalog_generation_and_security_epoch_required");
  }

  const std::string mutation_kind = PolicyMutationKind(request);
  const std::string policy_area = PolicyArea(request);
  const std::string policy_mode = PolicyMode(request);
  const std::string policy_envelope = PolicyEnvelope(request);
  if (!ValidPolicyMutationKind(mutation_kind)) {
    return PolicyMutationFailure(request, "unknown_policy_mutation:" + mutation_kind);
  }
  if (policy_area.empty()) {
    return PolicyMutationFailure(request, "policy_area_required");
  }
  if (mutation_kind != "remove" && policy_mode.empty()) {
    return PolicyMutationFailure(request, "policy_mode_required");
  }

  const EngineApiU64 next_policy_epoch =
      request.context.security_epoch >= request.context.catalog_generation_id
          ? request.context.security_epoch + 1
          : request.context.catalog_generation_id + 1;
  const std::string target_uuid = request.target_object.uuid.canonical;
  const std::string payload =
      "mutation_kind=" + mutation_kind +
      ";policy_area=" + policy_area +
      ";policy_mode=" + policy_mode +
      ";canonical_policy_envelope=" + policy_envelope +
      ";previous_policy_epoch=" + std::to_string(request.context.security_epoch) +
      ";new_policy_epoch=" + std::to_string(next_policy_epoch) +
      ";catalog_generation_id=" + std::to_string(request.context.catalog_generation_id) +
      ";mga_catalog_commit_required=true" +
      ";audit_required=true" +
      ";generation_invalidation_required=true" +
      ";database_command_authority=true" +
      ";post_create_filesystem_authority=false" +
      ";parser_sql_text_authority=false";

  const auto audit = AppendSecurityEvidenceEvent(
      request.context,
      operation_id,
      "policy_mutation",
      "mutation_kind=" + mutation_kind + ";policy_area=" + policy_area +
          ";new_policy_epoch=" + std::to_string(next_policy_epoch));
  if (audit.error) {
    return SecurityFailure<EnginePolicyMutationResult>(request.context, operation_id, audit);
  }

  auto result = PersistedRecordResultWithPayload<EnginePolicyMutationResult>(
      request,
      operation_id,
      "policy_catalog_mutation",
      true,
      mutation_kind == "remove" ? "removed" : "active",
      mutation_kind == "remove",
      payload);
  if (!result.ok) { return result; }

  auto invalidation = CatalogPinnedDescriptorInvalidationEventForMutation(
      "policy_catalog_mutation",
      result.primary_object.uuid.canonical.empty() ? target_uuid : result.primary_object.uuid.canonical,
      request.context.catalog_generation_id);
  invalidation.reason = "policy_generation_invalidated";
  const auto invalidated = GlobalCatalogPinnedDescriptorCache().Invalidate(invalidation);

  result.mutation_performed = true;
  result.mga_catalog_commit_required = true;
  result.audit_evidence_recorded = true;
  result.generation_invalidated = true;
  result.filesystem_pack_rejected = false;
  result.previous_policy_epoch = request.context.security_epoch;
  result.new_policy_epoch = next_policy_epoch;
  AddSecurityEvidence(&result, "database_policy_command", mutation_kind);
  AddSecurityEvidence(&result, "mga_catalog_commit", std::to_string(request.context.local_transaction_id));
  AddSecurityEvidence(&result, "policy_audit_event", "recorded");
  AddSecurityEvidence(&result, "policy_generation_invalidated", std::to_string(next_policy_epoch));
  AddSecurityEvidence(&result,
                      "catalog_pinned_descriptor_cache_invalidated",
                      std::to_string(invalidated.invalidated_entries.size()));
  AddSecurityEvidence(&result, "filesystem_policy_pack_authority", "false_after_create");
  AddSecurityRow(&result,
                 {{"operation_id", operation_id},
                  {"mutation_kind", mutation_kind},
                  {"policy_area", policy_area},
                  {"policy_mode", policy_mode},
                  {"object_uuid", result.primary_object.uuid.canonical},
                  {"mga_catalog_commit_required", "true"},
                  {"audit_evidence_recorded", "true"},
                  {"generation_invalidated", "true"},
                  {"previous_policy_epoch", std::to_string(result.previous_policy_epoch)},
                  {"new_policy_epoch", std::to_string(result.new_policy_epoch)},
                  {"post_create_filesystem_authority", "false"},
                  {"database_command_authority", "true"},
                  {"parser_sql_text_authority", "false"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
