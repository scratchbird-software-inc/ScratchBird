// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/identity_api.hpp"

#include "bootstrap_schema_roots.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/schema_tree_api.hpp"
#include "security/security_model.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction/mutation_authority.hpp"
#include "identity_publication_fence.hpp"
#include "disk_device.hpp"
#include "mga_relation_store/mga_metadata_record_codec.hpp"

#include <cctype>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string PathParent(const std::string& path) {
  const auto pos = path.rfind('.');
  return pos == std::string::npos ? std::string{} : path.substr(0, pos);
}

EngineUuid SchemaUuidForPath(const EngineRequestContext& context, const std::string& path,
                              EngineApiDiagnostic& diagnostic) {
  if (path.empty()) { return {}; }
  const auto schemas = VisibleSchemaTreeRecords(context, context.local_transaction_id, diagnostic);
  if (diagnostic.error) return {};
  for (const auto& schema : schemas) {
    for (const auto& name : schema.localized_names) {
      if (name.path == path) { return schema.schema_uuid; }
    }
  }
  return {};
}

bool IsSafeSchemaPathAtom(const std::string& value) {
  if (value.empty()) { return false; }
  for (const unsigned char ch : value) {
    if (!(std::isalnum(ch) || ch == '_' || ch == '-')) { return false; }
  }
  return true;
}

std::string IdentityPrincipalName(const EngineApiRequest& request) {
  const std::string explicit_principal = SecurityOptionValue(request, "principal_name:");
  if (!explicit_principal.empty()) { return explicit_principal; }
  if (!request.localized_names.empty() && !request.localized_names.front().name.empty()) {
    return request.localized_names.front().name;
  }
  return {};
}

EngineLocalizedName HomeSchemaName(const std::string& path, const std::string& name) {
  EngineLocalizedName localized;
  localized.language_tag = "en";
  localized.name_class = "home_schema";
  localized.path = path;
  localized.name = name;
  localized.default_name = true;
  localized.raw_name_text = name;
  localized.display_name = name;
  localized.normalized_lookup_key = path;
  localized.full_path_lookup_key = path;
  return localized;
}

struct HomeSchemaPolicy {
  bool create_home_schema = true;
  bool cluster_user = false;
  std::string policy_name = scratchbird::core::catalog::kLocalUserHomePolicyName;
  std::string path;
  EngineUuid parent_schema_uuid;
  EngineUuid schema_uuid;
};

EngineApiDiagnostic ResolveHomeSchemaPolicy(const EngineCreateIdentityRequest& request,
                                            const std::string& principal_name,
                                            HomeSchemaPolicy* policy) {
  policy->create_home_schema = SecurityOptionBool(request, "create_home_schema:", true);
  if (!policy->create_home_schema) {
    return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  }

  std::string scope = SecurityLower(SecurityOptionValue(request, "identity_scope:"));
  if (scope.empty()) { scope = request.context.cluster_authority_available ? "cluster" : "local"; }
  policy->cluster_user = scope == "cluster";
  if (policy->cluster_user && !request.context.cluster_authority_available) {
    return MakeSecurityDiagnostic("SECURITY.IDENTITY.CLUSTER_SCHEMA_ABSENT", "cluster_user_requires_cluster_authority");
  }

  policy->policy_name = SecurityOptionValue(request, "home_schema_policy:");
  if (policy->policy_name.empty()) { policy->policy_name = scratchbird::core::catalog::kLocalUserHomePolicyName; }

  const auto schema_bytes = SecurityOptionValue(request, "home_schema_uuid:");
  if (schema_bytes.empty()) {
    policy->schema_uuid = GenerateCrudEngineUuid("schema");
  } else if (!ReadMetadataUuid(schema_bytes, &policy->schema_uuid) ||
             !core::uuid::IsEngineIdentityUuid(policy->schema_uuid)) {
    return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_INVALID",
                                  "home_schema_binary_identity_required");
  }
  if (!core::uuid::IsEngineIdentityUuid(policy->schema_uuid)) {
    return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_INVALID",
                                  "home_schema_identity_issuance_failed");
  }

  policy->path = SecurityOptionValue(request, "home_schema_path:");
  if (policy->path.empty()) {
    if (!IsSafeSchemaPathAtom(principal_name)) {
      return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_INVALID", "home_schema_path_required_for_principal");
    }
    policy->path = std::string(policy->cluster_user ? scratchbird::core::catalog::kClusterUserHomePolicyRoot
                                                    : scratchbird::core::catalog::kLocalUserHomePolicyRoot) +
                   "." + principal_name;
  }

  EngineApiDiagnostic schema_diagnostic;
  const auto parent_bytes = SecurityOptionValue(request, "home_schema_parent_uuid:");
  if (!parent_bytes.empty() &&
      (!ReadMetadataUuid(parent_bytes, &policy->parent_schema_uuid) ||
       !core::uuid::IsEngineIdentityUuid(policy->parent_schema_uuid))) {
    return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_INVALID",
                                  "home_schema_parent_binary_identity_required");
  }
  if (policy->parent_schema_uuid.is_nil()) {
    const std::string parent_path = PathParent(policy->path);
    if (!parent_path.empty()) {
      policy->parent_schema_uuid = SchemaUuidForPath(request.context, parent_path, schema_diagnostic);
      if (schema_diagnostic.error) return schema_diagnostic;
      if (policy->parent_schema_uuid.is_nil()) {
        return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_PARENT_MISSING",
                                      "home_schema_parent_path_not_visible:" + parent_path);
      }
    } else if (!policy->cluster_user &&
               StartsWith(policy->path, std::string(scratchbird::core::catalog::kLocalUserHomePolicyRoot) + ".")) {
      policy->parent_schema_uuid = SchemaUuidForPath(request.context, scratchbird::core::catalog::kLocalUserHomePolicyRoot, schema_diagnostic);
      if (schema_diagnostic.error) return schema_diagnostic;
      if (policy->parent_schema_uuid.is_nil()) {
        return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_PARENT_MISSING",
                                      "local_user_home_root_not_visible");
      }
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic CreateIdentityHomeSchema(const EngineCreateIdentityRequest& request,
                                             const EngineUuid& identity_uuid,
                                             const std::string& principal_name,
                                             const HomeSchemaPolicy& policy) {
  std::vector<EngineLocalizedName> names{HomeSchemaName(policy.path, principal_name)};
  EngineApiDiagnostic schema_diagnostic;
  const auto conflict = SchemaTreePathConflict(request.context, policy.schema_uuid, policy.parent_schema_uuid,
      names, request.context.local_transaction_id, schema_diagnostic);
  if (schema_diagnostic.error) return schema_diagnostic;
  if (conflict) {
    return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_CONFLICT", *conflict);
  }

  EngineSchemaTreeRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.schema_uuid = policy.schema_uuid;
  record.parent_schema_uuid = policy.parent_schema_uuid;
  record.localized_names = names;
  record.default_name = principal_name;
  record.localized_comments.push_back({"en", "Default home schema for local ScratchBird user " + principal_name});
  BinaryCatalogMetadata extensions;
  extensions.identities.emplace("home_schema_owner_identity_uuid", identity_uuid);
  extensions.text.emplace("home_schema", "1");
  extensions.text.emplace("home_schema_policy", policy.policy_name);
  extensions.text.emplace("identity_scope", policy.cluster_user ? "cluster" : "local");
  record.payload = SchemaTreePayload(record.parent_schema_uuid, record.localized_names,
                                    record.localized_comments, std::move(extensions));
  if (record.payload.empty()) {
    return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_INVALID",
                                  "home_schema_binary_metadata_encoding_failed");
  }

  const auto appended = PersistSchemaTreeRecord(request.context, record, "security.create_identity.home_schema");
  if (appended.error) { return appended; }
  return PersistNameRegistryEntriesForObject(request.context,
                                             "security.create_identity.home_schema",
                                             record.schema_uuid,
                                             "schema",
                                             record.parent_schema_uuid,
                                             record.localized_names,
                                             record.default_name);
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_IDENTITY_API_BEHAVIOR
EngineCreateIdentityResult EngineCreateIdentity(const EngineCreateIdentityRequest& input) {
  auto request = input;
  if (!request.context.security_context_present) {
    return SecurityFailure<EngineCreateIdentityResult>(
        request.context,
        "security.create_identity",
        MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID", "security_context_required"));
  }
  if (!SecurityContextHasAnyAdmin(request.context, {"SEC_IDENTITY_ADMIN", "SEC_MEMBERSHIP_ADMIN"})) {
    return SecurityFailure<EngineCreateIdentityResult>(
        request.context,
        "security.create_identity",
        MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", "SEC_IDENTITY_ADMIN"));
  }
  const std::string kind = SecurityOptionValue(request, "identity_kind:");
  // User/service/system-actor identities must exist in the authentication
  // owner's durable principal catalog. A generic behavior row alone cannot
  // establish a principal, even if its names/home-schema projection succeeded.
  const bool principal_kind = kind == "user" || kind == "service" || kind == "system_actor";
  std::unique_lock<std::recursive_mutex> principal_guard;
  const std::string principal_name = principal_kind ? IdentityPrincipalName(request) : std::string{};
  std::optional<HomeSchemaPolicy> prepared_home_policy;
  storage::database::IdentityPublicationFence publication(
      request.context.database_path, request.context.database_uuid,
      request.context.local_transaction_id, request.context.transaction_uuid);
  const auto describe_publication_failure = [&](EngineApiDiagnostic diagnostic, std::string phase) {
    diagnostic.identity_fields.emplace_back("database_uuid", request.context.database_uuid);
    diagnostic.identity_fields.emplace_back("transaction_uuid", request.context.transaction_uuid);
    diagnostic.fields.emplace_back("publication_phase", std::move(phase));
    const auto observed = storage::database::AcquireStrongLocalTransactionInventorySnapshot(request.context.database_path);
    std::string exclusion = "unknown";
    if (observed.ok() && observed.snapshot->inventory.publication_base &&
        observed.snapshot->inventory.publication_base->database_uuid == request.context.database_uuid) {
      const auto entry = transaction::mga::LookupLocalTransaction(observed.snapshot->inventory,
          transaction::mga::MakeLocalTransactionId(request.context.local_transaction_id));
      if (entry.ok() && entry.entry.identity.transaction_uuid.value == request.context.transaction_uuid)
        exclusion = entry.entry.rollback_only ? "retained" : "absent";
    }
    diagnostic.fields.emplace_back("observed_native_commit_exclusion", std::move(exclusion));
    return MakeApiBehaviorDiagnostic<EngineCreateIdentityResult>(request.context,
        "security.create_identity", diagnostic);
  };
  const auto publication_failure = [&](const core::platform::DiagnosticRecord& cause, std::string phase) {
    auto diagnostic = MakeEngineApiDiagnosticFromNative(cause,
        "SB_ENGINE_API_INVALID_REQUEST", "engine.api.invalid_request", phase);
    return describe_publication_failure(std::move(diagnostic), std::move(phase));
  };
  if (principal_kind) {
    for (const auto& option : request.option_envelopes)
      for (const std::string_view prefix : {"password:", "credential:", "credential_evidence:",
                                           "credential_fingerprint:", "credential_protected_material_ref:"})
        if (option.starts_with(prefix))
          return SecurityFailure<EngineCreateIdentityResult>(request.context, "security.create_identity",
              MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID",
                                     "identity_creation_is_not_credential_provisioning"));
    principal_guard = AcquireTransactionInventoryGuard(request.context.database_path);
    if (kind == "user") {
      if (principal_name.empty()) return SecurityFailure<EngineCreateIdentityResult>(request.context,
          "security.create_identity", MakeSecurityDiagnostic(
              "SECURITY.IDENTITY.PRINCIPAL_REQUIRED", "principal_name_required"));
      HomeSchemaPolicy policy;
      const auto status = ResolveHomeSchemaPolicy(request, principal_name, &policy);
      if (status.error) return SecurityFailure<EngineCreateIdentityResult>(request.context,
          "security.create_identity", status);
      if (policy.create_home_schema) {
        EngineApiDiagnostic diagnostic;
        const auto conflict = SchemaTreePathConflict(request.context, policy.schema_uuid,
            policy.parent_schema_uuid, {HomeSchemaName(policy.path, principal_name)},
            request.context.local_transaction_id, diagnostic);
        if (diagnostic.error) return SecurityFailure<EngineCreateIdentityResult>(request.context,
            "security.create_identity", diagnostic);
        if (conflict) return SecurityFailure<EngineCreateIdentityResult>(request.context,
            "security.create_identity", MakeSecurityDiagnostic(
                "SECURITY.IDENTITY.HOME_SCHEMA_CONFLICT", *conflict));
      }
      prepared_home_policy = std::move(policy);
    }
    const auto authority = ValidateCurrentMutatingTransactionAuthority(request.context, "security.create_identity");
    if (authority.error) return SecurityFailure<EngineCreateIdentityResult>(request.context,
        "security.create_identity", authority);
    EngineSecurityCreatePrincipalRequest principal;
    static_cast<EngineApiRequest&>(principal) = request;
    principal.principal_uuid = request.target_object.uuid;
    principal.principal_name = principal_name;
    principal.principal_kind = kind;
    const auto principal_preflight = ValidateEngineSecurityCreatePrincipal(principal);
    if (principal_preflight.error) return SecurityFailure<EngineCreateIdentityResult>(request.context,
        "security.create_identity", principal_preflight);
    const auto fenced = publication.Begin();
    if (!fenced.ok()) return publication_failure(fenced.diagnostic, "identity_exclusion_publication");
    request.context.identity_publication_fence = &publication;
    principal.context.identity_publication_fence = &publication;
    // This is structural identity provisioning, not credential provisioning.
    // Only the dedicated native credential API may install a verifier; there
    // is no compiled password, option-envelope secret or inferred fingerprint.
    const auto created = EngineSecurityCreatePrincipal(principal);
    if (!created.ok || !created.principal_created) {
      const auto diagnostic = created.diagnostics.empty() ? MakeInvalidRequestDiagnostic(
          "security.create_identity", "native_principal_publication_not_completed") : created.diagnostics.front();
      return describe_publication_failure(diagnostic, "native_principal_publication");
    }
  }
  auto result = PersistedRecordResult<EngineCreateIdentityResult>(
      request,
      "security.create_identity",
      kind.empty() ? "security_identity" : "security_" + kind,
      true,
      "active");
  if (result.ok) {
    if (kind == "user") {
      const auto& policy = *prepared_home_policy;
      if (policy.create_home_schema) {
        const auto home_schema = CreateIdentityHomeSchema(request,
                                                         result.primary_object.uuid,
                                                         principal_name,
                                                         policy);
        if (home_schema.error) {
          return describe_publication_failure(home_schema, "home_schema_publication");
        }
        AddSecurityEvidence(&result, "home_schema", policy.schema_uuid);
        AddSecurityEvidence(&result, "home_schema_path", policy.path);
        AddSecurityEvidence(&result, "home_schema_policy", policy.policy_name);
        AddSecurityRow(&result, {{"object_uuid", policy.schema_uuid},
                                 {"object_kind", "schema"},
                                 {"name", principal_name},
                                 {"path", policy.path},
                                 {"parent_schema_uuid", policy.parent_schema_uuid},
                                 {"state", "active"}});
      }
    }
    AddSecurityEvidence(&result, "security_evidence_before_success", "identity_create");
    AddSecurityEvidence(&result, "security_admin", "identity");
  }
  if (!result.ok && principal_kind) {
    const auto diagnostic = result.diagnostics.empty() ? MakeInvalidRequestDiagnostic(
        "security.create_identity", "identity_projection_not_completed") : result.diagnostics.front();
    return describe_publication_failure(diagnostic, "identity_projection_publication");
  }
  if (result.ok && principal_kind) {
    // Complete response construction and every stream synchronization while
    // COMMIT remains durably excluded. No allocating response work follows
    // the successful release of this invocation's exact publication base.
    for (const std::string suffix : {".sb.api_events.v2", ".sb.name_events.v2"}) {
      const auto path = request.context.database_path + suffix;
      std::error_code error;
      if (!std::filesystem::exists(path, error)) {
        if (error) throw std::filesystem::filesystem_error("identity publication stream status", path, error);
        return describe_publication_failure(MakeInvalidRequestDiagnostic("security.create_identity",
            "required_publication_journal_missing"), "identity_stream_sync");
      }
      const auto sync = storage::disk::SyncFilesystemPath(path, true);
      if (!sync.ok()) return publication_failure(sync.diagnostic, "identity_stream_sync");
      const auto directory = storage::disk::SyncParentDirectoryPath(path);
      if (!directory.ok()) return publication_failure(directory.diagnostic, "identity_directory_sync");
    }
    const auto released = publication.Complete();
    if (!released.ok()) return publication_failure(released.diagnostic, "identity_exclusion_release");
    static_assert(std::is_nothrow_move_constructible_v<EngineCreateIdentityResult>);
  }
  return result;
}

EngineAlterIdentityResult EngineAlterIdentity(const EngineAlterIdentityRequest& request) {
  if (!request.context.security_context_present) {
    return SecurityFailure<EngineAlterIdentityResult>(
        request.context,
        "security.alter_identity",
        MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID", "security_context_required"));
  }
  if (!SecurityContextHasAnyAdmin(request.context, {"SEC_IDENTITY_ADMIN", "SEC_MEMBERSHIP_ADMIN"})) {
    return SecurityFailure<EngineAlterIdentityResult>(
        request.context,
        "security.alter_identity",
        MakeSecurityDiagnostic("SECURITY.AUTHORIZATION.DENIED", "SEC_IDENTITY_ADMIN"));
  }
  // Agent decisions are requests, not observed effects. A lock must update the
  // authentication owner's principal catalog, never a generic behavior record.
  const std::string* agent_action = nullptr;
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, "identity_uuid:")) {
      return SecurityFailure<EngineAlterIdentityResult>(request.context,
          "security.alter_identity", MakeSecurityDiagnostic(
              "SECURITY.IDENTITY.TARGET_INVALID", "use_native_target_object_identity"));
    }
    if (!StartsWith(option, "agent_decision:")) continue;
    if (agent_action != nullptr || option != "agent_decision:lock_user") {
      return SecurityFailure<EngineAlterIdentityResult>(request.context,
          "security.alter_identity", MakeSecurityDiagnostic(
              "SECURITY.IDENTITY.ACTION_INVALID", "exact_single_lock_user_action_required"));
    }
    agent_action = &option;
  }
  if (agent_action != nullptr) {
    if (!core::uuid::IsEngineIdentityUuid(request.target_object.uuid)) {
      return SecurityFailure<EngineAlterIdentityResult>(request.context,
          "security.alter_identity", MakeSecurityDiagnostic(
              "SECURITY.IDENTITY.TARGET_INVALID", "native_system_principal_identity_required"));
    }
    EngineSecurityAlterPrincipalRequest principal;
    // Do not forward unrelated rename/credential options on a lock-only route.
    principal.context = request.context;
    principal.principal_uuid = request.target_object.uuid;
    principal.lifecycle_state = "disabled";
    auto altered = EngineSecurityAlterPrincipal(principal);
    EngineAlterIdentityResult result;
    static_cast<EngineApiResult&>(result) = std::move(altered);
    result.operation_id = "security.alter_identity";
    return result;
  }
  auto result = PersistedRecordResult<EngineAlterIdentityResult>(request, "security.alter_identity", "security_identity", true, "altered");
  if (result.ok) {
    AddSecurityEvidence(&result, "security_evidence_before_success", "identity_alter");
    AddSecurityEvidence(&result, "security_admin", "identity");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
