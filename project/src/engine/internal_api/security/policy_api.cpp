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

#include <string_view>

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

constexpr std::string_view kPolicyBlockedDiagnosticUuid =
    "cd16f861-90a2-520e-97a7-79d2f28cc355";

bool UuidPresent(const EngineUuid& value) {
  return !value.canonical.empty();
}

bool ObjectReferenceEmpty(const EngineObjectReference& value) {
  return value.uuid.canonical.empty() && value.object_kind.empty();
}

bool SqlObjectReferenceEmpty(const EngineSqlObjectReference& value) {
  return value.expected_object_type.empty() &&
         value.path_type == "unqualified" && !value.no_search_path &&
         value.path_components.empty() && value.object_name.raw_text.empty() &&
         value.object_name.quote_style.empty() &&
         value.object_name.identifier_profile_uuid.empty() &&
         value.object_name.normalized_lookup_key.empty() &&
         value.object_name.exact_lookup_key.empty() &&
         !value.object_name.was_quoted &&
         !value.object_name.requires_exact_match &&
         value.object_name.source_span.empty();
}

bool BoundObjectIdentityEmpty(const EngineBoundObjectIdentity& value) {
  return value.object_uuid.canonical.empty() &&
         value.resolved_object_type.empty() &&
         value.resolved_schema_uuid.canonical.empty() &&
         value.parent_object_uuid.canonical.empty() &&
         value.object_descriptor_generation == 0 &&
         value.catalog_generation_id == 0 && value.security_epoch == 0 &&
         value.resource_epoch == 0;
}

bool PolicyObservationInputsAreClosed(
    const EngineEvaluatePolicyRequest& request) {
  const auto& packet = request.native_row_packet;
  return ObjectReferenceEmpty(request.target_database) &&
         ObjectReferenceEmpty(request.target_schema) &&
         ObjectReferenceEmpty(request.target_object) &&
         request.related_objects.empty() && request.localized_names.empty() &&
         SqlObjectReferenceEmpty(request.sql_object_reference) &&
         BoundObjectIdentityEmpty(request.bound_object_identity) &&
         request.descriptors.empty() && request.columns.empty() &&
         request.constraints.empty() && request.indexes.empty() &&
         !packet.present && packet.version == 0 && packet.row_count == 0 &&
         packet.column_count == 0 && packet.field_order.empty() &&
         packet.column_type_tags.empty() && packet.packet_bytes.empty() &&
         packet.row_offsets.empty() && packet.row_sizes.empty() &&
         request.rows.empty() && request.shared_row_field_order.empty() &&
         request.assignments.empty() && request.predicate.predicate_kind.empty() &&
         request.predicate.canonical_predicate_envelope.empty() &&
         request.predicate.bound_values.empty() &&
         request.projection.canonical_projection_envelopes.empty() &&
         request.ordering.canonical_ordering_envelopes.empty() &&
         request.physical_profile.names.empty() &&
         request.physical_profile.encoded_profiles.empty() &&
         request.policy_profile.names.empty() &&
         request.policy_profile.encoded_profiles.empty() &&
         request.compatibility_profile.names.empty() &&
         request.compatibility_profile.encoded_profiles.empty() &&
         request.option_envelopes.empty() &&
         request.diagnostic_options.empty();
}

bool PolicyObservationKindIsValid(EnginePolicyObservationKind kind) {
  return kind == EnginePolicyObservationKind::current_statement_gate ||
         kind ==
             EnginePolicyObservationKind::current_diagnostic_policy_refusal;
}

EngineEvaluatePolicyResult PolicyObservationFailure(
    const EngineEvaluatePolicyRequest& request,
    std::string code,
    std::string detail) {
  auto result = SecurityFailure<EngineEvaluatePolicyResult>(
      request.context, "security.evaluate_policy",
      MakeSecurityDiagnostic(std::move(code), std::move(detail)));
  result.policy_blocked = false;
  result.observation_kind = request.observation_kind;
  AddApiBehaviorEvidence(&result, "policy_gate_observation", "refused");
  return result;
}

bool PolicyObservationCohortMatches(const EngineRequestContext& context) {
  const auto& authorization = context.authorization_context;
  const auto& observation = context.current_policy_gate;
  return observation.present && UuidPresent(context.statement_uuid) &&
         UuidPresent(context.transaction_uuid) &&
         context.local_transaction_id != 0 &&
         UuidPresent(context.session_uuid) && UuidPresent(context.principal_uuid) &&
         context.security_context_present && authorization.present &&
         UuidPresent(authorization.authority_uuid) &&
         authorization.security_context_generation != 0 &&
         authorization.principal_uuid.canonical == context.principal_uuid.canonical &&
         authorization.security_epoch != 0 &&
         authorization.security_epoch == context.security_epoch &&
         authorization.policy_epoch != 0 &&
         authorization.catalog_generation_id != 0 &&
         authorization.catalog_generation_id == context.catalog_generation_id &&
         context.resource_epoch != 0 &&
         UuidPresent(context.transaction_policy_snapshot_uuid) &&
         context.transaction_policy_snapshot_generation != 0 &&
         observation.statement_uuid.canonical == context.statement_uuid.canonical &&
         observation.transaction_uuid.canonical ==
             context.transaction_uuid.canonical &&
         observation.local_transaction_id == context.local_transaction_id &&
         observation.authorization_context_uuid.canonical ==
             authorization.authority_uuid.canonical &&
         observation.authorization_context_generation ==
             authorization.security_context_generation &&
         observation.policy_snapshot_uuid.canonical ==
             context.transaction_policy_snapshot_uuid.canonical &&
         observation.policy_snapshot_generation ==
             context.transaction_policy_snapshot_generation &&
         observation.security_epoch == context.security_epoch &&
         observation.policy_epoch == authorization.policy_epoch &&
         observation.catalog_generation_id == context.catalog_generation_id &&
         observation.resource_epoch == context.resource_epoch;
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
  if (!request.operation_id.empty() &&
      request.operation_id != "security.evaluate_policy") {
    return PolicyObservationFailure(request, "SBLR.OPERAND_INVALID",
                                    "policy_observation_operation_invalid");
  }
  if (!PolicyObservationKindIsValid(request.observation_kind)) {
    return PolicyObservationFailure(request, "SBLR.OPERAND_INVALID",
                                    "policy_observation_kind_invalid");
  }
  if (!PolicyObservationInputsAreClosed(request)) {
    return PolicyObservationFailure(
        request, "SBLR.OPERAND_INVALID",
        "policy_observation_accepts_no_caller_policy_or_target_input");
  }
  if (!UuidPresent(request.context.statement_uuid)) {
    return PolicyObservationFailure(request, "SBSQL.NO_STATEMENT",
                                    "statement_policy_observation_unavailable");
  }
  if (!PolicyObservationCohortMatches(request.context)) {
    return PolicyObservationFailure(request, "SECURITY.ACCESS_DENIED",
                                    "statement_policy_authority_mismatch");
  }
  if (request.context.query_cancellation_requested &&
      request.context.query_cancellation_requested()) {
    return PolicyObservationFailure(request, "PROCESS.CANCELLED",
                                    "policy_observation_cancelled");
  }

  auto result = MakeApiBehaviorSuccess<EngineEvaluatePolicyResult>(
      request.context, "security.evaluate_policy");
  result.observation_kind = request.observation_kind;
  result.policy_blocked =
      request.observation_kind ==
              EnginePolicyObservationKind::current_statement_gate
          ? request.context.current_policy_gate.blocked
          : request.context.current_diagnostic_uuid.canonical ==
                kPolicyBlockedDiagnosticUuid;
  AddApiBehaviorEvidence(&result, "policy_gate_observation",
                         result.policy_blocked ? "blocked" : "not_blocked");
  AddApiBehaviorRow(
      &result,
      {{"policy_blocked", result.policy_blocked ? "true" : "false"},
       {"observation_kind",
        request.observation_kind ==
                EnginePolicyObservationKind::current_statement_gate
            ? "current_statement_gate"
            : "current_diagnostic_policy_refusal"},
       {"statement_uuid", request.context.statement_uuid.canonical},
       {"policy_snapshot_uuid",
        request.context.transaction_policy_snapshot_uuid.canonical},
       {"policy_snapshot_generation",
        std::to_string(
            request.context.transaction_policy_snapshot_generation)}});
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
