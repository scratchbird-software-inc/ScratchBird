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

#include <cctype>
#include <string>
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

std::string SchemaUuidForPath(const EngineRequestContext& context, const std::string& path) {
  if (path.empty()) { return {}; }
  for (const auto& schema : VisibleSchemaTreeRecords(context, context.local_transaction_id)) {
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
  std::string parent_schema_uuid;
  std::string schema_uuid;
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

  policy->schema_uuid = SecurityOptionValue(request, "home_schema_uuid:");
  if (policy->schema_uuid.empty()) { policy->schema_uuid = GenerateCrudEngineUuid("schema"); }

  policy->path = SecurityOptionValue(request, "home_schema_path:");
  if (policy->path.empty()) {
    if (!IsSafeSchemaPathAtom(principal_name)) {
      return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_INVALID", "home_schema_path_required_for_principal");
    }
    policy->path = std::string(policy->cluster_user ? scratchbird::core::catalog::kClusterUserHomePolicyRoot
                                                    : scratchbird::core::catalog::kLocalUserHomePolicyRoot) +
                   "." + principal_name;
  }

  policy->parent_schema_uuid = SecurityOptionValue(request, "home_schema_parent_uuid:");
  if (policy->parent_schema_uuid.empty()) {
    const std::string parent_path = PathParent(policy->path);
    if (!parent_path.empty()) {
      policy->parent_schema_uuid = SchemaUuidForPath(request.context, parent_path);
      if (policy->parent_schema_uuid.empty()) {
        return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_PARENT_MISSING",
                                      "home_schema_parent_path_not_visible:" + parent_path);
      }
    } else if (!policy->cluster_user &&
               StartsWith(policy->path, std::string(scratchbird::core::catalog::kLocalUserHomePolicyRoot) + ".")) {
      policy->parent_schema_uuid = SchemaUuidForPath(request.context, scratchbird::core::catalog::kLocalUserHomePolicyRoot);
      if (policy->parent_schema_uuid.empty()) {
        return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_PARENT_MISSING",
                                      "local_user_home_root_not_visible");
      }
    }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic CreateIdentityHomeSchema(const EngineCreateIdentityRequest& request,
                                             const std::string& identity_uuid,
                                             const std::string& principal_name,
                                             const HomeSchemaPolicy& policy) {
  std::vector<EngineLocalizedName> names{HomeSchemaName(policy.path, principal_name)};
  if (const auto conflict = SchemaTreePathConflict(request.context,
                                                  policy.schema_uuid,
                                                  policy.parent_schema_uuid,
                                                  names,
                                                  request.context.local_transaction_id)) {
    return MakeSecurityDiagnostic("SECURITY.IDENTITY.HOME_SCHEMA_CONFLICT", *conflict);
  }

  EngineSchemaTreeRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.schema_uuid = policy.schema_uuid;
  record.parent_schema_uuid = policy.parent_schema_uuid;
  record.localized_names = names;
  record.default_name = principal_name;
  record.localized_comments.push_back({"en", "Default home schema for local ScratchBird user " + principal_name});
  record.payload = SchemaTreePayload(record.parent_schema_uuid, record.localized_names, record.localized_comments);
  record.payload += ";home_schema=1;home_schema_owner_identity_uuid=" + identity_uuid;
  record.payload += ";home_schema_policy=" + policy.policy_name;
  record.payload += policy.cluster_user ? ";identity_scope=cluster" : ";identity_scope=local";

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
EngineCreateIdentityResult EngineCreateIdentity(const EngineCreateIdentityRequest& request) {
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
  auto result = PersistedRecordResult<EngineCreateIdentityResult>(
      request,
      "security.create_identity",
      kind.empty() ? "security_identity" : "security_" + kind,
      true,
      "active");
  if (result.ok) {
    if (kind == "user") {
      const std::string principal_name = IdentityPrincipalName(request);
      if (principal_name.empty()) {
        return MakeApiBehaviorDiagnostic<EngineCreateIdentityResult>(
            request.context,
            "security.create_identity",
            MakeSecurityDiagnostic("SECURITY.IDENTITY.PRINCIPAL_REQUIRED", "principal_name_required"));
      }
      HomeSchemaPolicy policy;
      const auto resolved_policy = ResolveHomeSchemaPolicy(request, principal_name, &policy);
      if (resolved_policy.error) {
        return MakeApiBehaviorDiagnostic<EngineCreateIdentityResult>(request.context,
                                                                     "security.create_identity",
                                                                     resolved_policy);
      }
      if (policy.create_home_schema) {
        const auto home_schema = CreateIdentityHomeSchema(request,
                                                         result.primary_object.uuid.canonical,
                                                         principal_name,
                                                         policy);
        if (home_schema.error) {
          return MakeApiBehaviorDiagnostic<EngineCreateIdentityResult>(request.context,
                                                                       "security.create_identity",
                                                                       home_schema);
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
  auto result = PersistedRecordResult<EngineAlterIdentityResult>(request, "security.alter_identity", "security_identity", true, "altered");
  if (result.ok) {
    AddSecurityEvidence(&result, "security_evidence_before_success", "identity_alter");
    AddSecurityEvidence(&result, "security_admin", "identity");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
