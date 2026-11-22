// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/authentication_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "metric_contracts.hpp"
#include "metric_producer.hpp"
#include "native_database_open_discovery.hpp"
#include "native_served_database_facts.hpp"
#include "native_login_material.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog_language.hpp"
#include "catalog_security_constants.hpp"
#include "security/auth_provider_model.hpp"
#include "security/security_crypto_policy.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "security/runtime_principal_credential.hpp"
#include "security/local_password_credential.hpp"
#include "security/durable_authorization_projection.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

std::string AuthMetricProvider(const EngineAuthenticateRequest& request) {
  const std::string provider = !request.provider_family.empty()
      ? request.provider_family
      : SecurityOptionValue(request, "provider:");
  return provider.empty() ? "local_password" : provider;
}

void RecordAuthenticationAttempt(const EngineAuthenticateRequest& request) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_identity_auth_attempts_total",
      scratchbird::core::metrics::Labels({{"component", "security.authenticate"},
                                          {"provider_family", AuthMetricProvider(request)}}),
      1.0,
      "security_auth");
}

void RecordAuthenticationFailure(const EngineAuthenticateRequest& request, const std::string& reason) {
  (void)scratchbird::core::metrics::IncrementCounter(
      "sb_auth_failures_total",
      scratchbird::core::metrics::Labels({{"component", "security.authenticate"},
                                          {"provider_family", AuthMetricProvider(request)},
                                          {"reason", reason.empty() ? "unknown" : reason}}),
      1.0,
      "security_auth");
}

EngineAuthenticateResult AuthenticationFailureResult(const EngineAuthenticateRequest& request,
                                                     std::string code,
                                                     std::string detail) {
  RecordAuthenticationFailure(request, detail);
  return SecurityFailure<EngineAuthenticateResult>(
      request.context,
      "security.authenticate",
      MakeSecurityDiagnostic(std::move(code), std::move(detail)));
}

// Preserve the owning source's complete canonical vector, including native
// storage/I/O cause and redaction metadata. Rebuilding it from code/detail loses
// that cause and incorrectly substitutes the generic security message key.
EngineAuthenticateResult AuthenticationFailureResult(
    const EngineAuthenticateRequest& request, EngineApiDiagnostic diagnostic) {
  RecordAuthenticationFailure(request, diagnostic.code);
  return SecurityFailure<EngineAuthenticateResult>(
      request.context, "security.authenticate", std::move(diagnostic));
}

// SEARCH_KEY: SB_NATIVE_LOGIN_UNSERVED (was SB_NATIVE_LOGIN_UNAVAILABLE: owner Q301 = yes, 2026-11-19; Q312: keep the
// refusal until S4c replaces it). S4c REPLACED it for a native database a server SERVES (the marker): see
// SB_NATIVE_LOGIN below. What remains of it is the one case with no loop to read through: a request with a database
// path and NO marker whose file is a valid native database (nobody serves it in this process, so the engine's own
// read-only discovery is all there is). That refuses with its own specific code, SECURITY.AUTHENTICATION.
// NATIVE_DATABASE_NOT_SERVED (reason native_database_not_served), not a credential verdict, before any loader runs and
// without opening, creating or writing any file beyond a read-only discovery of page zero. A file that is not a valid
// native database (legacy, missing, owned elsewhere) falls through to the present behaviour unchanged. The vector is
// structured: reason, binary database identity; never prose, never the principal claim.
bool NativeDatabaseNotServedRefusal(const EngineRequestContext& context, EngineApiDiagnostic* out) {
  EngineUuid database_uuid;
  if (context.database_path.empty()) return false;
  const auto discovered = scratchbird::storage::database::DiscoverNativeDatabaseAtPath(context.database_path);
  if (!discovered.ok()) return false;
  database_uuid = discovered.page_zero->bootstrap.database_uuid;
  *out = MakeEngineApiDiagnostic("SECURITY.AUTHENTICATION.NATIVE_DATABASE_NOT_SERVED",
                                 "security.authentication.native_database_not_served", {}, true);
  out->fields.push_back({"reason", "native_database_not_served"});
  out->identity_fields.push_back({"database_uuid", database_uuid});
  return true;
}

// ---- SEARCH_KEY: SB_NATIVE_LOGIN (thin-slice S4c, plan 40; owner Q306-Q312, the 2026-11-20 name-model rulings, the
// 2026-11-21 snapshot-point ruling, the Firebird login review) --------------------------------------------------------
// Login on a native database a server serves (the marker is present). Sequence, all on the database loop thread inside
// ONE snapshot-point scope (no durable transaction, nothing written, no number burned):
//   1. the presented login text (AS DATA, never an identifier into the engine) -> a principal UUID through the
//      EXISTING resolver (the names source, the principal name scope = the authorization policy body's user schema,
//      the session language then the database default language), then the hidden-name policy;
//   2. the S4a credential read for that principal UUID and the password.
// What the native path takes from the request: the claim text, the password (credential_evidence, opaque: its bytes
// are the password, nothing in it is parsed) and the session language. Everything else is IGNORED on purpose - the
// option envelopes (`credential:valid`, `fixture_fail:`, `principal:`, storage authority...), parsed evidence fields
// (`principal_uuid=`), a claimed durable principal UUID (refused outright): no field of a request can inject a
// principal, a role or a "trusted" flag (the Firebird lesson).
// The FAILURE MODEL: every credential cause (unknown / hidden / ambiguous name, principal absent / not active / of
// the wrong family, no credential, an unsupported credential, password mismatch or inadmissible, an agent or service
// principal) returns ONE diagnostic - SECURITY.AUTHENTICATION.LOGIN_FAILED, structured, with no name, no principal and
// no cause - byte-identical across causes; the specific code travels on result.native_login_audit for the server
// layer's audit trail. Infrastructure failures (database failed / not serving, busy, closed, crypto, memory, custody,
// names or security source) keep their own specific client-visible codes: a client can tell an unavailable server from
// a wrong password.
constexpr std::string_view kLoginFailedCode = "SECURITY.AUTHENTICATION.LOGIN_FAILED";

EngineApiDiagnostic NativeLoginFailedDiagnostic(const EngineUuid& database_uuid) {
  auto diagnostic = MakeEngineApiDiagnostic(std::string(kLoginFailedCode), "security.authentication.login_failed", {}, true);
  diagnostic.fields.push_back({"reason", "login_failed"});
  diagnostic.identity_fields.push_back({"database_uuid", database_uuid});
  return diagnostic;
}

EngineApiDiagnostic NativeLoginInfrastructureDiagnostic(const char* code, const char* cause, const EngineUuid& database_uuid) {
  auto diagnostic = MakeEngineApiDiagnostic(code, "security.authentication.native_login_unavailable", {}, true);
  diagnostic.fields.push_back({"reason", cause});
  diagnostic.identity_fields.push_back({"database_uuid", database_uuid});
  return diagnostic;
}

std::string NativeLanguageTagOf(const EngineUuid& language) {
  for (const auto& known : scratchbird::core::catalog::kCatalogLanguages)
    if (known.uuid == language) return std::string(known.tag);
  return "";
}

// Step 1, run ON THE LOOP THREAD inside the scope. Reads names, resolves, applies the hidden-name policy.
NativeLoginSelection SelectNativeLoginPrincipal(const EngineRequestContext& caller, const std::string& name_text) {
  NativeLoginSelection selection;
  const auto& facts = *caller.native_served_facts;
  if (!facts.names_source) {
    selection.error = NativeLoginSelectionError::names_unavailable;
    selection.names_error = NativeNamesSnapshotError::names_source_failure;
    return selection;
  }
  // The principals' name scope and the database's default language come from one names read (a read that ENDS before
  // the resolver's own read starts: a nested runtime read reports busy).
  // SB_NAMES_SCOPE_ARGUMENT: this read needs the principals' schema (the source reports it whatever the filter) and the
  // DATABASE object's own default name (its language); no other scope's rows are retained. The resolver's own read after it
  // is the registry projection (all scopes).
  NativeNamesReadFilter filter;
  filter.all_scopes = false;
  filter.objects.push_back(facts.database_uuid);
  const auto snapshot = facts.names_source->Read(filter);
  if (!snapshot.ok()) {
    selection.error = NativeLoginSelectionError::names_unavailable;
    selection.names_error = snapshot.error;
    return selection;
  }
  if (snapshot.principal_schemas.empty()) {
    selection.error = NativeLoginSelectionError::scope_absent;
    return selection;
  }
  if (snapshot.principal_schemas.size() > 1) {
    selection.error = NativeLoginSelectionError::scope_ambiguous;
    return selection;
  }
  // SB_NATIVE_LOGIN_LANGUAGE: the database's default language = the language of the DATABASE object's own default name
  // (the file keeps no separate field: every object's default name carries its language). The session language, when
  // the request carries one, is tried first by the resolver; this is its fallback.
  std::string default_language;
  for (const auto& row : snapshot.rows)
    if (row.object_uuid == facts.database_uuid && row.default_for_object) {
      default_language = NativeLanguageTagOf(row.language_uuid);
      break;
    }
  // A FRESH minimal context: nothing of the caller's request (policy profiles, search path, schemas) can steer it.
  EngineResolveNameRequest request;
  request.context.native_served_facts = caller.native_served_facts;
  request.context.database_uuid = facts.database_uuid;
  request.context.language_context.language_tag = caller.language_context.language_tag;
  request.context.language_context.default_language_tag = default_language;
  request.context.identifier_profile_uuid = "sbsql_v3";   // SB_PART_B_PROFILE_LABEL: the shim's label for the SBsql profile
  request.target_schema.uuid = snapshot.principal_schemas.front();
  auto& name = request.sql_object_reference.object_name;
  name.raw_text = name_text;                              // exactly as given: the engine does not fold, trim or interpret
  name.was_quoted = false;                                // the handoff carries no quoting; the PARSER's job (Part B)
  name.requires_exact_match = false;
  name.quote_style = "none";
  name.identifier_profile_uuid = "sbsql_v3";
  if (name_text.empty()) {
    selection.error = NativeLoginSelectionError::name_not_found;  // the resolver refuses "" (Part B); nothing to look up
    return selection;
  }
  const auto matches = ResolveNameRegistryPublic(request, std::string());
  if (!matches.ok) {
    if (matches.diagnostic.code == "CATALOG.NAME.NOT_FOUND") {
      selection.error = NativeLoginSelectionError::name_not_found;
    } else {
      selection.error = NativeLoginSelectionError::resolver_failed;
      selection.resolver_code = matches.diagnostic.code;
    }
    return selection;
  }
  // The hidden-name policy (S4b Q1: the resolver returns the matched entry's class; the LOGIN applies the policy): a
  // hidden-class name does not authenticate; primary, alias, compatibility and generated names do.
  std::vector<EngineUuid> visible;
  bool any_hidden = false;
  for (const auto& entry : matches.matches) {
    if (entry.name_class == "hidden") {
      any_hidden = true;
      continue;
    }
    if (std::find(visible.begin(), visible.end(), entry.object_uuid) == visible.end()) visible.push_back(entry.object_uuid);
  }
  if (visible.empty()) {
    selection.error = any_hidden ? NativeLoginSelectionError::name_hidden : NativeLoginSelectionError::name_not_found;
    return selection;
  }
  if (visible.size() > 1) {
    selection.error = NativeLoginSelectionError::name_ambiguous;
    return selection;
  }
  selection.error = NativeLoginSelectionError::none;
  selection.principal_uuid = visible.front();
  return selection;
}

const char* NativeScopeErrorCode(NativeLoginScopeError e) {
  switch (e) {
    case NativeLoginScopeError::database_not_serving:
      return NativeLoginMaterialErrorCode(NativeLoginMaterialError::database_not_serving);
    case NativeLoginScopeError::database_failed: return NativeLoginMaterialErrorCode(NativeLoginMaterialError::database_failed);
    case NativeLoginScopeError::busy: return NativeLoginMaterialErrorCode(NativeLoginMaterialError::busy);
    default: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_SCOPE_FAILED";
  }
}

// The S4a causes that are the client's problem (the one generic failure) versus the server's (their own code).
bool NativeMaterialErrorIsCredentialCause(NativeLoginMaterialError e) {
  switch (e) {
    case NativeLoginMaterialError::password_inadmissible:
    case NativeLoginMaterialError::principal_absent:
    case NativeLoginMaterialError::principal_wrong_family:
    case NativeLoginMaterialError::principal_not_active:
    case NativeLoginMaterialError::credential_absent:
    case NativeLoginMaterialError::credential_unsupported:
    case NativeLoginMaterialError::password_mismatch:
    case NativeLoginMaterialError::principal_generation_invalid:
    case NativeLoginMaterialError::external_binding_refused:
      return true;
    default:
      return false;
  }
}

// SB_NATIVE_AUTH_ADAPTER_DELETE_WHEN_NATIVE_AUTHORIZATION (owner Q307 = 1, 2026-11-19: native security rows reach
// authorization through an ADAPTER to the existing lifecycle-state shape for the first slice, with a deletion note).
// DELETE THIS ADAPTER when a native authorization input exists (authorization then takes the native login material
// directly). It builds the shape the legacy path hands to ApplyDurableAuthorizationProjectionToSession /
// DurableAuthorizationStateFromLifecycle (EngineSecurityPrincipalLifecycleState) from the native login material:
// binary identities, generations and constants only - NO names (definitions hold no names), no fingerprint, and
// authority_class is IGNORED for users (S4a Q6). S4e Part 1: the state carries the principal's EFFECTIVE role and group
// records (the transitive closure the material reports), the allow-effect membership rows of the principal and of those
// roles / groups, and the privilege rows of the principal AND of those roles / groups (allow and deny), the same
// facts the legacy lifecycle state hands the shared materializer, which resolves the effective subjects from them.
std::string NativeRightTag(scratchbird::core::catalog::CatalogSecurityRight right) {
  for (const auto& known : scratchbird::core::catalog::kCatalogSecurityRights)
    if (known.right == right) return std::string(known.tag);
  return {};
}
const char* NativeSubjectKindText(scratchbird::core::catalog::CatalogSecuritySubjectKind kind) {
  using K = scratchbird::core::catalog::CatalogSecuritySubjectKind;
  switch (kind) {
    case K::role: return "role";
    case K::group: return "group";
    default: return "principal";
  }
}
std::shared_ptr<const EngineSecurityPrincipalLifecycleState> NativeLoginLifecycleState(const NativeLoginMaterial& material) {
  namespace cat = scratchbird::core::catalog;
  auto state = std::make_shared<EngineSecurityPrincipalLifecycleState>();
  state->security_context_generation = material.security_context_generation;
  state->security_generation = material.security_generation;
  state->policy_generation = material.policy_generation;
  state->verified_native_sysarch_role_uuid = material.sysarch_role_uuid;
  EngineSecurityPrincipalRecord principal;
  principal.principal_uuid = material.principal_uuid;
  principal.principal_kind = material.principal_kind == cat::CatalogSecurityPrincipalKind::service ? "service" : "user";
  principal.lifecycle_state = "active";
  principal.security_generation = material.security_generation;
  state->principals.push_back(std::move(principal));
  for (const auto& role : material.effective_role_uuids) {
    EngineSecurityRoleRecord record;
    record.role_uuid = role;
    record.security_generation = material.security_generation;
    state->roles.push_back(std::move(record));
  }
  for (const auto& group : material.effective_group_uuids) {
    EngineSecurityGroupRecord record;
    record.group_uuid = group;
    record.security_generation = material.security_generation;
    state->groups.push_back(std::move(record));
  }
  for (const auto& edge : material.edges) {
    if (edge.grant_class == cat::CatalogSecurityGrantClass::role_membership ||
        edge.grant_class == cat::CatalogSecurityGrantClass::group_membership) {
      // A deny-effect membership never makes anyone a member. A membership whose member is a role / group is kept
      // only when that role / group is in the effective sets (the material already filtered; stated again here).
      if (edge.effect.has_value() && *edge.effect == cat::CatalogSecurityEffect::deny) continue;
      const auto in = [&](const std::vector<scratchbird::core::platform::Uuid>& set, const scratchbird::core::platform::Uuid& id) {
        return std::find(set.begin(), set.end(), id) != set.end();
      };
      if (!in(material.effective_role_uuids, edge.parent_uuid) && !in(material.effective_group_uuids, edge.parent_uuid)) continue;
      EngineSecurityMembershipRecord record;
      record.membership_uuid = edge.grant_uuid;
      record.member_principal_uuid = edge.member_uuid;
      record.container_uuid = edge.parent_uuid;
      // The member / parent KINDS decide the edge, not the legacy class label (coordinator correction 2026-11-12: a
      // principal may be a member of a group through a row of class role_membership). OLD: the class label chose the kind.
      record.container_kind = edge.parent_kind == cat::CatalogSecuritySubjectKind::group  ? "group"
                              : edge.parent_kind == cat::CatalogSecuritySubjectKind::role ? "role"
                              : edge.grant_class == cat::CatalogSecurityGrantClass::role_membership ? "role" : "group";
      record.security_generation = edge.security_generation;
      state->memberships.push_back(std::move(record));
    } else if (edge.grant_class == cat::CatalogSecurityGrantClass::privilege && edge.right.has_value()) {
      EngineSecurityPrivilegeGrantRecord record;
      record.grant_uuid = edge.grant_uuid;
      record.grantee_uuid = edge.subject_uuid;
      record.grantee_kind = NativeSubjectKindText(edge.subject_kind);
      record.target_object_uuid = edge.target_uuid;
      record.privilege = NativeRightTag(*edge.right);
      record.grant_effect = edge.effect.has_value() && *edge.effect == cat::CatalogSecurityEffect::deny ? "deny" : "allow";
      record.security_generation = edge.security_generation;
      if (!record.privilege.empty()) state->grants.push_back(std::move(record));
    }
  }
  return state;
}

EngineAuthenticateResult NativeLoginFailure(const EngineAuthenticateRequest& request, EngineApiDiagnostic client_visible,
                                            std::string internal_code, bool credential_failure,
                                            const EngineUuid& principal_uuid) {
  // The audit/metric reason is the INTERNAL code (a bounded enum-derived token; no name, no secret).
  RecordAuthenticationFailure(request, internal_code);
  auto result = SecurityFailure<EngineAuthenticateResult>(request.context, "security.authenticate", std::move(client_visible));
  result.native_login_audit.native = true;
  result.native_login_audit.credential_failure = credential_failure;
  result.native_login_audit.internal_code = std::move(internal_code);
  result.native_login_audit.principal_uuid = principal_uuid;
  return result;
}

EngineAuthenticateResult NativeLogin(const EngineAuthenticateRequest& request) {
  const auto facts = request.context.native_served_facts;
  const EngineUuid database_uuid = facts->database_uuid;
  const auto generic = [&](const std::string& internal, const EngineUuid& principal = {}) {
    return NativeLoginFailure(request, NativeLoginFailedDiagnostic(database_uuid), internal, true, principal);
  };
  // A claimed principal UUID is never an input of the native path (the handoff never sets one).
  if (!request.durable_principal_uuid.is_nil()) {
    return NativeLoginFailure(request,
                              NativeLoginInfrastructureDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID",
                                                                  "native_principal_claim_forbidden", database_uuid),
                              "SECURITY.AUTHENTICATION.REQUEST_INVALID", false, {});
  }
  const std::string provider = request.provider_family;
  if (!provider.empty() && provider != "local_password") {
    return NativeLoginFailure(request,
                              NativeLoginInfrastructureDiagnostic("SECURITY.AUTHENTICATION.NATIVE_PROVIDER_UNSUPPORTED",
                                                                  "native_provider_unsupported", database_uuid),
                              "SECURITY.AUTHENTICATION.NATIVE_PROVIDER_UNSUPPORTED", false, {});
  }
  if (!facts->login_source) {
    return NativeLoginFailure(request,
                              NativeLoginInfrastructureDiagnostic("SECURITY.AUTHENTICATION.NATIVE_LOGIN_SCOPE_FAILED",
                                                                  "login_source_absent", database_uuid),
                              "SECURITY.AUTHENTICATION.NATIVE_LOGIN_SCOPE_FAILED", false, {});
  }
  // A transport that says the credential is invalid is believed (fail closed); it can only refuse, never admit.
  const bool transport_invalid = request.credential_invalid_claim;
  std::string password = request.credential_evidence;  // the one copy the login scrubs; the request's own is the caller's
  const EngineRequestContext& caller = request.context;
  const std::string name_text = request.principal_claim;
  const auto attempt = facts->login_source->Login(
      [&caller, &name_text]() { return SelectNativeLoginPrincipal(caller, name_text); }, password);

  if (attempt.scope_error != NativeLoginScopeError::none) {
    return NativeLoginFailure(request,
                              NativeLoginInfrastructureDiagnostic(NativeScopeErrorCode(attempt.scope_error),
                                                                  NativeLoginScopeErrorName(attempt.scope_error), database_uuid),
                              NativeScopeErrorCode(attempt.scope_error), false, {});
  }
  const auto& selection = attempt.selection;
  switch (selection.error) {
    case NativeLoginSelectionError::none: break;
    case NativeLoginSelectionError::name_not_found:
      return generic("SECURITY.AUTHENTICATION.NATIVE_LOGIN_NAME_NOT_FOUND");
    case NativeLoginSelectionError::name_hidden:
      return generic("SECURITY.AUTHENTICATION.NATIVE_LOGIN_NAME_HIDDEN");
    case NativeLoginSelectionError::name_ambiguous:
      return generic("SECURITY.AUTHENTICATION.NATIVE_LOGIN_NAME_AMBIGUOUS");
    case NativeLoginSelectionError::scope_absent:
    case NativeLoginSelectionError::scope_ambiguous: {
      const char* code = selection.error == NativeLoginSelectionError::scope_absent
                             ? "SECURITY.AUTHENTICATION.NATIVE_LOGIN_PRINCIPAL_SCOPE_ABSENT"
                             : "SECURITY.AUTHENTICATION.NATIVE_LOGIN_PRINCIPAL_SCOPE_AMBIGUOUS";
      return NativeLoginFailure(request,
                                NativeLoginInfrastructureDiagnostic(code, NativeLoginSelectionErrorName(selection.error), database_uuid),
                                code, false, {});
    }
    case NativeLoginSelectionError::names_unavailable: {
      const char* code = NativeNamesSnapshotErrorCode(selection.names_error);
      return NativeLoginFailure(request,
                                NativeLoginInfrastructureDiagnostic(code, NativeNamesSnapshotErrorName(selection.names_error), database_uuid),
                                code, false, {});
    }
    case NativeLoginSelectionError::resolver_failed:
      return NativeLoginFailure(request,
                                NativeLoginInfrastructureDiagnostic("SECURITY.AUTHENTICATION.NATIVE_LOGIN_NAME_SOURCE_FAILED",
                                                                    "resolver_failed", database_uuid),
                                "SECURITY.AUTHENTICATION.NATIVE_LOGIN_NAME_SOURCE_FAILED", false, {});
  }
  // The selection resolved to one principal: the credential verdict.
  if (attempt.material_error != NativeLoginMaterialError::none || !attempt.material.has_value()) {
    if (NativeMaterialErrorIsCredentialCause(attempt.material_error))
      return generic(NativeLoginMaterialErrorCode(attempt.material_error), selection.principal_uuid);
    const char* code = NativeLoginMaterialErrorCode(attempt.material_error);
    if (attempt.material_error == NativeLoginMaterialError::none) code = "SECURITY.AUTHENTICATION.NATIVE_LOGIN_SCOPE_FAILED";
    return NativeLoginFailure(request,
                              NativeLoginInfrastructureDiagnostic(code, NativeLoginMaterialErrorName(attempt.material_error), database_uuid),
                              code, false, selection.principal_uuid);
  }
  if (transport_invalid) return generic("SECURITY.AUTHENTICATION.NATIVE_LOGIN_TRANSPORT_FLAG_INVALID", selection.principal_uuid);

  // ---- approved. NO transaction and NO session record is created here (S4e: the user's transaction begins on
  // approval, with the authenticated principal in header +64, once the session ledger exists). The result carries the
  // binary principal, the role / group identities, and the adapter state the server's projection consumes.
  const auto& material = *attempt.material;
  const auto durable_principal = scratchbird::core::uuid::MakeDurableEngineIdentityUuid(
      scratchbird::core::platform::UuidKind::principal, material.principal_uuid);
  if (!durable_principal.ok()) return generic("SECURITY.AUTHENTICATION.NATIVE_LOGIN_PRINCIPAL_IDENTITY_INVALID", selection.principal_uuid);
  ConnectionSecurityContextRecord context;
  context.connection_uuid = request.context.session_uuid;
  if (context.connection_uuid.is_nil()) context.connection_uuid = GenerateCrudEngineUuid("session");
  context.effective_user_uuid = durable_principal.value.value;
  context.authority_uuid = database_uuid;
  context.policy_epoch = request.context.catalog_generation_id == 0 ? 1 : request.context.catalog_generation_id;
  context.security_epoch = request.context.security_epoch == 0 ? 1 : request.context.security_epoch;
  context.active_roles = material.role_uuids;
  context.effective_groups = material.group_uuids;
  context.transaction_start_policy = "deny_when_expired";
  context.disclosure_policy = "hidden_as_missing";
  auto result = SecuritySuccess<EngineAuthenticateResult>(request.context, "security.authenticate");
  result.authenticated = true;
  result.ok = true;
  result.connection_security_context = context;
  result.durable_security_state = NativeLoginLifecycleState(material);  // SB_NATIVE_AUTH_ADAPTER_DELETE_WHEN_NATIVE_AUTHORIZATION
  result.primary_object.uuid = context.effective_user_uuid;
  result.primary_object.object_kind = "principal";
  result.native_login_audit.native = true;
  result.native_login_audit.internal_code = "SECURITY.AUTHENTICATION.NATIVE_LOGIN_ACCEPTED";
  result.native_login_audit.principal_uuid = material.principal_uuid;
  AddSecurityEvidence(&result, "authentication_provider", "local_password");
  AddSecurityEvidence(&result, "durable_principal_uuid", durable_principal.value.value);
  AddSecurityEvidence(&result, "security_state_authority", "native_login_snapshot_point");
  AddSecurityEvidence(&result, "connection_security_context", context.connection_uuid);
  // No claim text in any row: the principal appears only as its binary identity.
  AddSecurityRow(&result, {{"authenticated", "true"},
                           {"provider", "local_password"},
                           {"connection_uuid", context.connection_uuid},
                           {"effective_user_uuid", context.effective_user_uuid}});
  return result;
}

bool IsHexVerifier(std::string_view value) {
  if (value.size() != 64) return false;
  for (const unsigned char ch : value) {
    if (!std::isxdigit(ch)) return false;
  }
  return true;
}

std::map<std::string, std::string> ParseEvidenceFields(std::string_view evidence) {
  std::map<std::string, std::string> fields;
  std::size_t cursor = 0;
  while (cursor < evidence.size()) {
    const std::size_t end = evidence.find(';', cursor);
    const std::string_view part = evidence.substr(cursor, end == std::string_view::npos ? evidence.size() - cursor
                                                                                        : end - cursor);
    const std::size_t eq = part.find('=');
    if (eq != std::string_view::npos && eq != 0 && eq + 1 < part.size()) {
      fields.emplace(std::string(part.substr(0, eq)), std::string(part.substr(eq + 1)));
    }
    if (end == std::string_view::npos) break;
    cursor = end + 1;
  }
  return fields;
}

bool IsDurableSecurityStateAuthority(std::string_view value) {
  return value == "durable_security_catalog" ||
         value == "engine_durable_security_state" ||
         value == "mga_security_principal_lifecycle";
}

std::string FieldOrOption(const EngineAuthenticateRequest& request,
                          const std::map<std::string, std::string>& fields,
                          const std::string& field,
                          const std::string& option_prefix) {
  const auto found = fields.find(field);
  if (found != fields.end()) { return found->second; }
  return SecurityOptionValue(request, option_prefix);
}

bool IsDurablePrincipalUuid(const EngineUuid& value) {
  return scratchbird::core::uuid::MakeDurableEngineIdentityUuid(
      scratchbird::core::platform::UuidKind::principal, value).ok();
}

bool HasTextPrincipalUuid(const EngineAuthenticateRequest& request,
                         const std::map<std::string, std::string>& fields) {
  return fields.contains("principal_uuid") || std::any_of(
      request.option_envelopes.begin(), request.option_envelopes.end(),
      [](const auto& option) { return option.starts_with("durable_principal_uuid:"); });
}

EngineUuid DurablePrincipalUuid(const EngineAuthenticateRequest& request,
                               const std::map<std::string, std::string>& fields) {
  // Text evidence is never a second source of identity, even alongside a native candidate.
  if (HasTextPrincipalUuid(request, fields)) return {};
  return request.durable_principal_uuid;
}

EngineUuid ResolveAuthenticatedPrincipalIdentity(
    const EngineSecurityPrincipalLifecycleState& state,
    const std::string& principal_name, const EngineUuid& candidate) {
  EngineUuid resolved;
  for (const auto& principal : state.principals) {
    if (principal.principal_name != principal_name) continue;
    if (!resolved.is_nil() || principal.deleted || principal.lifecycle_state != "active" ||
        !IsDurablePrincipalUuid(principal.principal_uuid) ||
        (!candidate.is_nil() && candidate != principal.principal_uuid)) return {};
    resolved = principal.principal_uuid;
  }
  return resolved;
}

void AddUniqueAuthorizationTag(std::vector<std::string>* tags, std::string tag) {
  if (tags == nullptr || tag.empty()) { return; }
  if (std::find(tags->begin(), tags->end(), tag) == tags->end()) {
    tags->push_back(std::move(tag));
  }
}

std::vector<std::string> DurableAuthorizationTagsForPrincipal(
    const EngineAuthenticateRequest& request,
    const EngineUuid& principal_uuid,
    bool include_connect_fallback,
    const EngineSecurityPrincipalLifecycleState* retained_state) {
  std::vector<std::string> tags;
  if (include_connect_fallback) {
    AddUniqueAuthorizationTag(&tags, "right:CONNECT");
  }
  if (!IsDurablePrincipalUuid(principal_uuid)) { return tags; }
  EngineLoadSecurityPrincipalLifecycleStateResult loaded;
  const EngineSecurityPrincipalLifecycleState* state = retained_state;
  if (state == nullptr) {
    loaded = LoadSecurityPrincipalLifecycleState(request.context);
    if (!loaded.ok) { return tags; }
    state = &loaded.state;
  }
  for (const auto& grant : state->grants) {
    if (grant.grantee_kind != "principal" ||
        grant.grantee_uuid != principal_uuid ||
        !grant.target_object_uuid.is_nil() ||
        grant.grant_effect == "deny" ||
        !IsKnownSecurityRight(grant.privilege)) {
      continue;
    }
    AddUniqueAuthorizationTag(&tags, "right:" + grant.privilege);
  }
  return tags;
}

bool VerifyLocalPasswordCredentialFingerprint(std::string_view stored,
                                              std::string_view password) {
  // Legacy caller's fixed scratch remains erased; the native startup adapter
  // supplies its separately admitted protected workspace to the shared mechanism.
  std::array<std::byte, 32> derived{};
  return VerifyLocalPasswordCredential(stored, password, derived).verified();
}

std::string TemporaryTokenCredentialFingerprint(std::string_view token_handle,
                                                std::string_view token_digest,
                                                std::string_view state,
                                                std::string_view expires_at_ms) {
  std::string payload;
  payload.reserve(token_digest.size() + state.size() + expires_at_ms.size() + 2);
  payload += token_digest;
  payload += '|';
  payload += state.empty() ? "active" : state;
  payload += '|';
  payload += expires_at_ms.empty() ? "0" : expires_at_ms;
  return "security-temporary-token:v1:hmac-sha256:" +
         SecurityHmacSha256Hex(token_handle, payload);
}

EngineApiDiagnostic DurableCredentialFingerprintForPrincipal(
    const EngineAuthenticateRequest& request,
    const std::string& principal,
    const EngineUuid& durable_principal_uuid,
    std::string* credential_fingerprint,
    const EngineSecurityPrincipalLifecycleState* retained_state) {
  if (credential_fingerprint == nullptr) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "credential_fingerprint_output_required");
  }
  if (request.context.database_path.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTH_SOURCE_UNAVAILABLE",
                                  "durable_security_state_database_path_required");
  }
  EngineLoadSecurityPrincipalLifecycleStateResult loaded;
  const EngineSecurityPrincipalLifecycleState* state = retained_state;
  if (state == nullptr) {
    loaded = LoadSecurityPrincipalLifecycleState(request.context);
    if (!loaded.ok) {
      return MakeSecurityDiagnostic("SECURITY.AUTH_SOURCE_UNAVAILABLE",
                                    loaded.diagnostic.detail.empty()
                                        ? "durable_security_state_unavailable"
                                        : loaded.diagnostic.detail);
    }
    state = &loaded.state;
  }
  for (const auto& record : state->principals) {
    if (record.principal_uuid != durable_principal_uuid) { continue; }
    if (record.principal_name != principal) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "durable_principal_name_mismatch");
    }
    if (record.lifecycle_state != "active") {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "durable_principal_disabled");
    }
    // SEARCH_KEY: SB_AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED (owner Q274,
    // 2026-11-19: agents "can not have a connection outside the database
    // itself ie even if you have the password, you can not run it from any
    // location except inside the program itself"). Every service principal
    // (L5 and the agent principals) is internal; this entry point serves
    // external attaches (server sessions, the SBLR authenticate op), so it
    // refuses them before any password is verified.
    if (const auto binding = CheckExternalAuthenticationBinding(record.principal_kind); binding.error) {
      return binding;
    }
    if (record.credential_fingerprint.empty()) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "durable_credential_fingerprint_required");
    }
    *credential_fingerprint = record.credential_fingerprint;
    return EngineApiDiagnostic{"SB_ENGINE_API_OK", "engine.api.ok", {}, false};
  }
  return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                "durable_principal_credential_missing");
}

EngineApiDiagnostic DurableCredentialFingerprintForPrincipalName(
    const EngineAuthenticateRequest& request,
    const std::string& principal,
    EngineUuid* durable_principal_uuid,
    std::string* credential_fingerprint,
    const EngineSecurityPrincipalLifecycleState* retained_state) {
  if (durable_principal_uuid == nullptr || credential_fingerprint == nullptr) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "credential_fingerprint_output_required");
  }
  if (principal.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID",
                                  "principal_claim_required");
  }
  if (request.context.database_path.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTH_SOURCE_UNAVAILABLE",
                                  "durable_security_state_database_path_required");
  }
  EngineLoadSecurityPrincipalLifecycleStateResult loaded;
  const EngineSecurityPrincipalLifecycleState* state = retained_state;
  if (state == nullptr) {
    loaded = LoadSecurityPrincipalLifecycleState(request.context);
    if (!loaded.ok) {
      return MakeSecurityDiagnostic("SECURITY.AUTH_SOURCE_UNAVAILABLE",
                                    loaded.diagnostic.detail.empty()
                                        ? "durable_security_state_unavailable"
                                        : loaded.diagnostic.detail);
    }
    state = &loaded.state;
  }
  const EngineSecurityPrincipalRecord* matched = nullptr;
  for (const auto& record : state->principals) {
    if (record.principal_name != principal) { continue; }
    if (matched != nullptr) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "durable_principal_name_ambiguous");
    }
    matched = &record;
  }
  if (matched == nullptr) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "durable_principal_credential_missing");
  }
  if (matched->lifecycle_state != "active") {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "durable_principal_disabled");
  }
  // SB_AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED (Q274): see the UUID form above.
  if (const auto binding = CheckExternalAuthenticationBinding(matched->principal_kind); binding.error) {
    return binding;
  }
  if (matched->credential_fingerprint.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "durable_credential_fingerprint_required");
  }
  *durable_principal_uuid = matched->principal_uuid;
  *credential_fingerprint = matched->credential_fingerprint;
  return EngineApiDiagnostic{"SB_ENGINE_API_OK", "engine.api.ok", {}, false};
}

bool LooksLikeStructuredAuthEvidence(std::string_view evidence) {
  return evidence.find("scheme=") != std::string_view::npos ||
         evidence.find(';') != std::string_view::npos;
}

EngineApiDiagnostic VerifyLocalPasswordEvidence(const EngineAuthenticateRequest& request,
                                                const std::string& principal,
                                                EngineUuid* resolved_principal_uuid,
                                                bool* server_derived_connect_right,
                                                const EngineSecurityPrincipalLifecycleState*
                                                    retained_state) {
  if (request.credential_invalid_claim ||
      SecurityOptionPresent(request, "credential:invalid") ||
      SecurityOptionBool(request, "fixture_fail:", false)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED", "credential_evidence_failed");
  }
  const auto fields = ParseEvidenceFields(request.credential_evidence);
  const auto scheme = fields.find("scheme");
  const auto evidence_principal = fields.find("principal");
  const auto verifier = fields.find("verifier");
  const EngineUuid durable_principal_uuid = DurablePrincipalUuid(request, fields);
  const std::string storage_authority =
      FieldOrOption(request, fields, "storage_authority", "security_storage_authority:");
  if (scheme == fields.end() && !LooksLikeStructuredAuthEvidence(request.credential_evidence)) {
    EngineUuid resolved_uuid;
    std::string durable_fingerprint;
    const auto durable = DurableCredentialFingerprintForPrincipalName(
        request, principal, &resolved_uuid, &durable_fingerprint,
        retained_state);
    if (durable.error) { return durable; }
    if (!VerifyLocalPasswordCredentialFingerprint(
            durable_fingerprint, request.credential_evidence)) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "credential_verifier_mismatch");
    }
    if (resolved_principal_uuid != nullptr) { *resolved_principal_uuid = resolved_uuid; }
    if (server_derived_connect_right != nullptr) { *server_derived_connect_right = true; }
    return EngineApiDiagnostic{"SB_ENGINE_API_OK", "engine.api.ok", {}, false};
  }
  if (scheme == fields.end() || scheme->second != "local_password_v1" ||
      evidence_principal == fields.end() || evidence_principal->second != principal ||
      verifier == fields.end() || !IsHexVerifier(verifier->second)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED", "credential_verifier_evidence_required");
  }
  if (!IsDurablePrincipalUuid(durable_principal_uuid)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED", "durable_principal_uuid_required");
  }
  if (!IsDurableSecurityStateAuthority(storage_authority)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "durable_security_state_authority_required");
  }
  std::string durable_fingerprint;
  const auto durable = DurableCredentialFingerprintForPrincipal(
      request, principal, durable_principal_uuid, &durable_fingerprint,
      retained_state);
  if (durable.error) { return durable; }
  return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                "password_secret_required_for_pbkdf2_verification");
}

std::uint64_t CurrentEpochMilliseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

bool TemporaryTokenTextValid(std::string_view token) {
  if (token.empty() || token.size() > 1024) return false;
  for (const unsigned char ch : token) {
    const bool ok = std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ':';
    if (!ok) return false;
  }
  return true;
}

EngineApiDiagnostic VerifySecurityDatabaseTemporaryTokenEvidence(
    const EngineAuthenticateRequest& request,
    const std::string& principal) {
  if (principal.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID", "principal_claim_required");
  }
  if (request.credential_invalid_claim ||
      SecurityOptionPresent(request, "credential:invalid") ||
      SecurityOptionBool(request, "fixture_fail:", false)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED", "credential_evidence_failed");
  }
  const auto fields = ParseEvidenceFields(request.credential_evidence);
  const auto scheme = fields.find("scheme");
  const auto evidence_principal = fields.find("principal");
  const auto token = fields.find("token");
  const EngineUuid durable_principal_uuid = DurablePrincipalUuid(request, fields);
  const std::string storage_authority =
      FieldOrOption(request, fields, "storage_authority", "security_storage_authority:");
  const std::string token_handle =
      FieldOrOption(request, fields, "token_handle", "durable_token_handle:");
  const auto token_digest = fields.find("token_digest");
  if (scheme == fields.end() || scheme->second != "security_database_temporary_token_v1" ||
      evidence_principal == fields.end() || evidence_principal->second != principal ||
      token == fields.end() || !TemporaryTokenTextValid(token->second)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_evidence_required");
  }
  if (!IsDurablePrincipalUuid(durable_principal_uuid)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED", "durable_principal_uuid_required");
  }
  if (!IsDurableSecurityStateAuthority(storage_authority)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "durable_security_state_authority_required");
  }
  if (token_handle.empty()) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_handle_required");
  }
  if (token_digest == fields.end() || !IsHexVerifier(token_digest->second)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_digest_required");
  }
  const std::string computed_digest = SecuritySha256Hex(token->second);
  if (!IsHexVerifier(computed_digest) ||
      !SecurityConstantTimeEqual(token_digest->second, computed_digest)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_digest_mismatch");
  }
  const std::string row_state = FieldOrOption(request, fields, "state", "durable_token_state:");
  if (!row_state.empty() && row_state != "active" && row_state != "valid") {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_revoked");
  }
  const std::string row_expires =
      FieldOrOption(request, fields, "expires_at_ms", "durable_token_expires_at_ms:");
  if (!row_expires.empty()) {
    std::uint64_t expires_at_ms = 0;
    try {
      expires_at_ms = static_cast<std::uint64_t>(std::stoull(row_expires));
    } catch (...) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "security_database_temporary_token_expiry_invalid");
    }
    if (expires_at_ms != 0 && expires_at_ms < CurrentEpochMilliseconds()) {
      return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                    "security_database_temporary_token_expired");
    }
  }
  std::string durable_fingerprint;
  const auto durable = DurableCredentialFingerprintForPrincipal(
      request, principal, durable_principal_uuid, &durable_fingerprint,
      nullptr);
  if (durable.error) { return durable; }
  const std::string expected = TemporaryTokenCredentialFingerprint(
      token_handle,
      token_digest->second,
      row_state.empty() ? "active" : row_state,
      row_expires.empty() ? "0" : row_expires);
  if (expected.empty() || !SecurityConstantTimeEqual(durable_fingerprint, expected)) {
    return MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.FAILED",
                                  "security_database_temporary_token_not_found");
  }
  return EngineApiDiagnostic{"SB_ENGINE_API_OK", "engine.api.ok", {}, false};
}

}  // namespace

// SB_AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED (Q274): see the declaration.
EngineApiDiagnostic CheckExternalAuthenticationBinding(std::string_view principal_kind) {
  if (principal_kind == "service") {
    return MakeSecurityDiagnostic("SECURITY.AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED",
                                  "service_principals_authenticate_only_inside_the_engine");
  }
  return EngineApiDiagnostic{"SB_ENGINE_API_OK", "engine.api.ok", {}, false};
}

DurableAuthorizationMaterializeResult RuntimePrincipalCredentialLease::MaterializeLocalAuthorization(
    std::uint64_t security_epoch, std::uint64_t policy_epoch,
    std::uint64_t catalog_generation) const {
  const auto refuse = [](const char* detail) {
    DurableAuthorizationMaterializeResult result;
    result.diagnostics.push_back(MakeSecurityDiagnostic("SECURITY.CONTEXT.EXPIRED", detail));
    return result;
  };
  if (thread_ != std::this_thread::get_id() || !guard_.owns_lock() ||
      !security_epoch || !policy_epoch || !catalog_generation)
    return refuse("runtime_service_authorization_fence_or_configuration_invalid");
  EngineRequestContext source;
  source.database_uuid = database_;
  source.database_path = path_;
  const auto loaded = LoadSecurityPrincipalLifecycleState(source);
  if (!loaded.ok) {
    DurableAuthorizationMaterializeResult result;
    result.diagnostics.push_back(loaded.diagnostic);
    return result;
  }
  const auto& state = loaded.state;
  if (state.security_context_generation != context_generation_ ||
      state.security_generation != security_generation_ || state.policy_generation != policy_generation_)
    return refuse("runtime_service_authorization_source_changed");
  const auto active = std::find_if(state.principals.begin(), state.principals.end(),
      [&](const auto& principal) { return principal.principal_uuid == principal_; });
  if (active == state.principals.end() || active->deleted || active->lifecycle_state != "active" ||
      active->principal_kind != "service") return refuse("runtime_service_principal_not_active");
  const DurableAuthorizationProjectionBinding binding{
      database_, principal_, security_epoch, policy_epoch, catalog_generation};
  const auto projection = ProjectDurableAuthorizationState(state, binding);
  DurableAuthorizationMaterializeRequest request;
  request.principal_uuid = principal_;
  return MaterializeDurableAuthorizationContext(projection, request);
}

RuntimePrincipalCredentialResult VerifyRuntimePrincipalCredential(
    const RuntimePrincipalCredentialRequest& request) noexcept {
  using Error = RuntimePrincipalCredentialError;
  RuntimePrincipalCredentialResult result;
  try {
    if (request.database_path.empty() || request.database_path.find('\0') != std::string::npos ||
        !core::uuid::IsEngineIdentityUuid(request.database_uuid) ||
        !IsDurablePrincipalUuid(request.principal_uuid) || request.password.empty() ||
        request.password.size() > 1024 || request.password.find('\0') != std::string_view::npos)
      return result;
    auto guard = AcquireTransactionInventoryGuard(request.database_path);
    EngineRequestContext source;
    source.database_path = request.database_path;
    source.database_uuid = request.database_uuid;
    // No transaction/snapshot from the caller: authentication cannot expose its
    // own uncommitted principal or credential changes as committed authority.
    auto loaded = LoadSecurityPrincipalLifecycleState(source);
    if (!loaded.ok) {
      result.error = Error::source_failure;
      result.source_diagnostic = std::move(loaded.diagnostic);
      return result;
    }
    const auto& state = loaded.state;
    if (!state.security_context_generation || !state.security_generation || !state.policy_generation) {
      result.error = Error::source_failure;
      return result;
    }
    const EngineSecurityPrincipalRecord* selected = nullptr;
    result.error = Error::credential_rejected;
    for (const auto& principal : state.principals) {
      if (principal.principal_uuid != request.principal_uuid) continue;
      if (selected || principal.deleted || principal.lifecycle_state != "active" ||
          principal.principal_kind != "service") return result;
      selected = &principal;
    }
    if (!selected || !VerifyLocalPasswordCredentialFingerprint(
                         selected->credential_fingerprint, request.password)) return result;
    result.lease.reset(new RuntimePrincipalCredentialLease(
        std::move(guard), request.database_path, request.database_uuid, selected->principal_uuid,
        state.security_context_generation, state.security_generation, state.policy_generation));
    result.error = Error::none;
  } catch (const std::bad_alloc&) {
    result.error = Error::resource_exhausted;
  } catch (const std::length_error&) {
    result.error = Error::resource_exhausted;
  } catch (const std::system_error&) {
    result.error = Error::lock_failure;
  }
  return result;
}

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_AUTHENTICATION_API_BEHAVIOR
EngineAuthenticateResult EngineAuthenticate(const EngineAuthenticateRequest& request) {
  RecordAuthenticationAttempt(request);
  // SB_NATIVE_LOGIN (S4c, replacing the S3f0 refusal NATIVE_LOGIN_UNAVAILABLE, owner Q312): a database a server serves
  // (the marker) logs in through the loop; a native file nobody serves here has no loop and refuses specifically.
  if (request.context.native_served_facts != nullptr) return NativeLogin(request);
  if (EngineApiDiagnostic native_refusal; NativeDatabaseNotServedRefusal(request.context, &native_refusal))
    return AuthenticationFailureResult(request, std::move(native_refusal));
  const std::string provider = !request.provider_family.empty()
      ? request.provider_family
      : SecurityOptionValue(request, "provider:");
  const std::string principal = !request.principal_claim.empty()
      ? request.principal_claim
      : SecurityOptionValue(request, "principal:");

  EngineApiRequest provider_request = request;
  provider_request.option_envelopes.push_back("auth_flow:login");
  provider_request.option_envelopes.push_back("provider:" + (provider.empty() ? "local_password" : provider));
  if (!principal.empty()) {
    provider_request.option_envelopes.push_back("principal:" + principal);
  }
  const bool credential_present = request.credential_evidence_present ||
                                  !request.credential_evidence.empty() ||
                                  SecurityOptionBool(request, "credential_evidence_present:", false) ||
                                  SecurityOptionPresent(request, "credential:valid");
  const std::string canonical_provider = CanonicalAuthProviderFamily(provider.empty() ? "local_password" : provider);
  auto credential_fields = ParseEvidenceFields(request.credential_evidence);
  // Rejected identity input is not an absent candidate. Otherwise the raw
  // password path could turn a textual or malformed identity into name-only
  // authentication after DurablePrincipalUuid returned nil.
  if (HasTextPrincipalUuid(request, credential_fields) ||
      (!request.durable_principal_uuid.is_nil() &&
       !IsDurablePrincipalUuid(request.durable_principal_uuid))) {
    return AuthenticationFailureResult(request, "SECURITY.AUTHENTICATION.REQUEST_INVALID",
                                       "binary_principal_identity_required");
  }
  EngineUuid authenticated_principal_uuid = DurablePrincipalUuid(request, credential_fields);
  std::vector<std::string> engine_authorization_tags;
  std::shared_ptr<const EngineSecurityPrincipalLifecycleState>
      authentication_durable_security_state;
  if (canonical_provider == "security_database_temporary_token") {
    const auto temporary_token = VerifySecurityDatabaseTemporaryTokenEvidence(request, principal);
    if (temporary_token.error) {
      return AuthenticationFailureResult(request, temporary_token);
    }
    provider_request.option_envelopes.push_back("credential:valid");
    provider_request.option_envelopes.push_back("credential_evidence_present:true");
    provider_request.option_envelopes.push_back("protected_material:available");
  } else if ((provider.empty() || provider == "local_password") && credential_present) {
    auto loaded = LoadSecurityPrincipalLifecycleState(request.context);
    if (!loaded.ok) {
      return AuthenticationFailureResult(request, std::move(loaded.diagnostic));
    }
    authentication_durable_security_state =
        std::make_shared<const EngineSecurityPrincipalLifecycleState>(
            std::move(loaded.state));
    EngineUuid resolved_principal_uuid;
    bool server_derived_connect_right = false;
    const auto local_password = VerifyLocalPasswordEvidence(
        request, principal, &resolved_principal_uuid,
        &server_derived_connect_right,
        authentication_durable_security_state.get());
    if (local_password.error) {
      return AuthenticationFailureResult(request, local_password);
    }
    if (!resolved_principal_uuid.is_nil()) {
      // Preserve the original selection across name/password resolution. The
      // final durable lookup must never compare against a replacement of the
      // candidate the caller actually supplied.
      if (!authenticated_principal_uuid.is_nil() &&
          authenticated_principal_uuid != resolved_principal_uuid) {
        return AuthenticationFailureResult(request, "SECURITY.AUTHENTICATION.FAILED",
                                           "durable_principal_identity_mismatch");
      }
      authenticated_principal_uuid = resolved_principal_uuid;
      credential_fields.emplace("storage_authority", "mga_security_principal_lifecycle");
    }
    engine_authorization_tags = DurableAuthorizationTagsForPrincipal(
        request, resolved_principal_uuid, server_derived_connect_right,
        authentication_durable_security_state.get());
    if (server_derived_connect_right) {
      provider_request.option_envelopes.push_back("credential_password_transport:raw");
    }
    provider_request.option_envelopes.push_back("credential_verifier_match:true");
    provider_request.option_envelopes.push_back("protected_material:available");
  } else if (credential_present) {
    provider_request.option_envelopes.push_back("credential_evidence_present:true");
  }
  const bool mfa_required = SecurityOptionBool(request, "mfa_required:", false);
  const bool mfa_present = request.mfa_evidence_present || SecurityOptionBool(request, "mfa_evidence_present:", false) ||
                           SecurityOptionPresent(request, "mfa:present");
  if (mfa_required) {
    provider_request.option_envelopes.push_back("mfa_required:true");
  }
  if (mfa_present) {
    provider_request.option_envelopes.push_back("mfa_evidence_present:true");
    provider_request.option_envelopes.push_back("mfa:present");
  }

  const auto provider_decision = AuthenticateWithProvider(provider_request);
  if (!provider_decision.ok || !provider_decision.authenticated) {
    return AuthenticationFailureResult(request,
                                       provider_decision.diagnostic.code.empty()
                                           ? "SECURITY.AUTHENTICATION.FAILED"
                                           : provider_decision.diagnostic.code,
                                       provider_decision.diagnostic.detail.empty()
                                           ? "provider_denied"
                                           : provider_decision.diagnostic.detail);
  }

  if (!authentication_durable_security_state) {
    auto loaded = LoadSecurityPrincipalLifecycleState(request.context);
    if (!loaded.ok)
      return AuthenticationFailureResult(request, std::move(loaded.diagnostic));
    authentication_durable_security_state =
        std::make_shared<const EngineSecurityPrincipalLifecycleState>(std::move(loaded.state));
  }
  authenticated_principal_uuid = ResolveAuthenticatedPrincipalIdentity(
      *authentication_durable_security_state, principal, authenticated_principal_uuid);
  // SB_AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED (owner Q274, 2026-11-19): the
  // gate every provider passes - the local-password lookups above refuse a
  // service principal before its password is verified; this one covers the
  // others (e.g. the temporary-token provider), which have no such lookup,
  // once the final principal is known and before any session context is built.
  for (const auto& record : authentication_durable_security_state->principals) {
    if (record.principal_uuid != authenticated_principal_uuid || record.deleted) continue;
    if (const auto binding = CheckExternalAuthenticationBinding(record.principal_kind); binding.error) {
      return AuthenticationFailureResult(request, binding.code, binding.detail);
    }
  }
  EngineApiRequest context_request = request;
  context_request.target_object.uuid = request.target_object.uuid;
  auto context = ConnectionSecurityContextFromRequest(context_request);
  const auto durable_principal =
      scratchbird::core::uuid::MakeDurableEngineIdentityUuid(
          scratchbird::core::platform::UuidKind::principal,
          authenticated_principal_uuid);
  if (durable_principal.ok()) {
    context.effective_user_uuid = durable_principal.value.value;
  } else {
    return AuthenticationFailureResult(request, "SECURITY.AUTHENTICATION.FAILED",
                                       "durable_principal_identity_required");
  }
  if (context.connection_uuid.is_nil()) { context.connection_uuid = GenerateCrudEngineUuid("session"); }
  if (context.authority_uuid.is_nil()) { context.authority_uuid = request.context.database_uuid; }
  context.authorization_trace_tags = std::move(engine_authorization_tags);

  auto result = SecuritySuccess<EngineAuthenticateResult>(request.context, "security.authenticate");
  result.authenticated = true;
  result.connection_security_context = context;
  result.durable_security_state =
      std::move(authentication_durable_security_state);
  result.primary_object.uuid = context.effective_user_uuid;
  result.primary_object.object_kind = "principal";
  (void)scratchbird::core::metrics::PublishIdentitySessionsActive(
      1.0,
      provider_decision.provider_family.empty() ? "local_password" : provider_decision.provider_family,
      "self",
      scratchbird::core::metrics::Labels({{"session_uuid", context.connection_uuid},
                                          {"principal_uuid", context.effective_user_uuid}}));
  (void)scratchbird::core::metrics::PublishIdentityUsersOnline(
      1.0,
      provider_decision.provider_family.empty() ? "local_password" : provider_decision.provider_family,
      scratchbird::core::metrics::Labels({{"principal_uuid", context.effective_user_uuid}}));
  ApplyAuthProviderDecision(&result, provider_decision);
  result.ok = true;
  result.authenticated = true;
  result.operation_id = "security.authenticate";
  AddSecurityEvidence(&result,
                      "authentication_provider",
                      provider_decision.provider_family.empty() ? "local_password" : provider_decision.provider_family);
  if (durable_principal.ok()) {
    AddSecurityEvidence(&result, "durable_principal_uuid", durable_principal.value.value);
  }
  const std::string storage_authority =
      FieldOrOption(request, credential_fields, "storage_authority", "security_storage_authority:");
  if (IsDurableSecurityStateAuthority(storage_authority)) {
    AddSecurityEvidence(&result, "security_state_authority", storage_authority);
  }
  AddSecurityEvidence(&result, "connection_security_context", context.connection_uuid);
  for (const auto& tag : context.authorization_trace_tags) {
    if (tag.rfind("group:", 0) == 0) {
      AddSecurityEvidence(&result, "authorized_group", tag.substr(6));
    }
  }
  AddSecurityRow(&result, {{"authenticated", "true"},
                           {"provider", provider_decision.provider_family.empty()
                                            ? "local_password"
                                            : provider_decision.provider_family},
                           {"principal", provider_decision.principal.empty()
                                             ? principal
                                             : provider_decision.principal},
                           {"connection_uuid", context.connection_uuid},
                           {"effective_user_uuid", context.effective_user_uuid}});
  return result;
}

EngineRefreshSecurityContextResult EngineRefreshSecurityContext(
    const EngineRefreshSecurityContextRequest& request) {
  if (!request.context.security_context_present) {
    return SecurityFailure<EngineRefreshSecurityContextResult>(
        request.context,
        "security.refresh_context",
        MakeSecurityDiagnostic("SECURITY.AUTHENTICATION.REQUEST_INVALID", "security_context_required"));
  }
  if (SecurityContextHasTag(request.context, "security_context:expired") &&
      !SecurityContextHasRight(request.context, "SEC_IDENTITY_ADMIN")) {
    return SecurityFailure<EngineRefreshSecurityContextResult>(
        request.context,
        "security.refresh_context",
        MakeSecurityDiagnostic("SECURITY.CONTEXT.EXPIRED", "refresh_requires_authority"));
  }
  auto result = SecuritySuccess<EngineRefreshSecurityContextResult>(request.context, "security.refresh_context");
  result.refreshed = true;
  AddSecurityEvidence(&result, "security_context", "refreshed");
  AddSecurityRow(&result, {{"refreshed", "true"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
