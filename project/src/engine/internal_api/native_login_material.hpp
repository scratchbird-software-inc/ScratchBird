// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
#pragma once

// SEARCH_KEY: SB_NATIVE_LOGIN_SOURCE (thin-slice S4c, plan 40; owner Q306-Q312, 2026-11-19/22).
// The engine-side vocabulary of a NATIVE login. The types of the S4a credential read (NativeLoginMaterialError,
// NativeLoginGrantEdge, NativeLoginMaterial) were defined in the server's runtime header; EngineAuthenticate lives in
// the engine and must see them, so their definitions MOVED here verbatim (the server header re-exports them with
// using-declarations; no copy exists). Below them: the login SOURCE the database loop implements (as the names
// source), carried in the served facts, and the structured result of one login attempt.

#include "catalog_security_constants.hpp"
#include "uuid.hpp"
#include "native_served_database_facts.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <array>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {


// SEARCH_KEY: SB_NATIVE_LOGIN_MATERIAL (thin-slice S4a, plan 40; owner Q306-Q312). A native credential read with
// NO session: given a principal UUID and a supplied password, read that principal's committed native security rows
// from the served file, verify the password against the stored PBKDF2 verifier in constant time and report the
// facts the later native authorization adapter (Q307) needs, or a specific refusal. Every refusal is an enum value
// with its own stable code (NativeLoginMaterialErrorCode); no refusal carries text, a name, a path or a secret.
// The codes are INTERNAL: the later wire layer decides which of them a client may see (owner question).
enum class NativeLoginMaterialError : std::uint8_t {
  none,
  // The request itself: a nil / non-identity principal UUID, a missing database binding, a protected buffer that is
  // not usable. Never reached for a well-formed request.
  invalid_request,
  // The supplied password cannot be a local password (empty, longer than 1024 bytes, or holds a NUL). Decided from
  // the input alone, before the file is read.
  password_inadmissible,
  // The database is not serving (the loop has no serving database to read). `failed` is refused (Q299 / Q309);
  // `restricted` is NOT refused: login material reads on a restricted database (Q309).
  database_not_serving,
  database_failed,
  // Reading the file failed; no credential verdict exists. These are infrastructure causes, not answers.
  wrong_owner, busy, closed, preparation_failure, custody_failure, crypto_failure, memory_failure,
  security_source_failure, security_binding_failure, protected_workspace_required, verifier_provider_failure,
  // The verdicts, each its own cause.
  principal_absent,
  principal_wrong_family,     // the UUID names a row that is not a principal
  principal_not_active,       // deleted, not active, or its active flag is false
  credential_absent,          // the principal row carries no verifier
  credential_unsupported,     // a scheme or parameters this build does not verify
  password_mismatch,
  principal_generation_invalid,
  external_binding_refused    // an agent / service principal: refused on every external path (Q274)
};
// The stable refusal code (SECURITY.AUTHENTICATION.NATIVE_LOGIN_*; the external-binding refusal carries Q274's own
// SECURITY.AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED, the code CheckExternalAuthenticationBinding returns).

// One direct edge or privilege row of the authenticated principal, copied (binary identities and constants only).
// NOT resolved: no transitive membership, no deny/allow evaluation, no effective privilege.
struct NativeLoginGrantEdge {
  core::platform::Uuid grant_uuid;
  core::catalog::CatalogSecurityGrantClass grant_class = core::catalog::CatalogSecurityGrantClass::none;
  // privilege: subject_* and right/effect/target; membership: member -> parent.
  core::platform::Uuid subject_uuid, member_uuid, parent_uuid, target_uuid;
  core::catalog::CatalogSecuritySubjectKind subject_kind = core::catalog::CatalogSecuritySubjectKind::none;
  core::catalog::CatalogSecuritySubjectKind member_kind = core::catalog::CatalogSecuritySubjectKind::none;
  core::catalog::CatalogSecuritySubjectKind parent_kind = core::catalog::CatalogSecuritySubjectKind::none;
  std::optional<core::catalog::CatalogSecurityEffect> effect;
  std::optional<core::catalog::CatalogSecurityRight> right;
  core::platform::u64 security_generation = 0;
};
// What an authenticated login read reports. Owning copies only: no verifier, salt or password byte is in it.
struct NativeLoginMaterial {
  core::platform::Uuid database_uuid, principal_uuid, principal_version_uuid;
  core::catalog::CatalogSecurityPrincipalKind principal_kind = core::catalog::CatalogSecurityPrincipalKind::none;
  std::optional<core::catalog::CatalogSecurityAuthorityClass> authority_class;
  bool bootstrap_principal = false;
  core::platform::u64 definition_generation = 0, security_context_authority_version = 0,
      security_context_generation = 0, security_generation = 0, policy_generation = 0;
  // The selected catalog the rows were read from (descriptive; a fence for the later adapter).
  core::platform::u64 catalog_generation = 0, schema_epoch = 0, security_epoch = 0;
  // Direct role / group memberships of the principal (member = the principal), active and not retired, in object
  // order. The rows behind them are in `edges`.
  std::vector<core::platform::Uuid> role_uuids, group_uuids;
  // S4e Part 1 (owner S4c Q1 = extend, 2026-11-21): the EFFECTIVE role / group sets, i.e. the transitive closure of the
  // principal's allow-effect membership edges through ACTIVE role / group rows (a membership whose parent row is absent,
  // retired, deleted or not active is not followed), sorted ascending by UUID bytes, without duplicates. They carry
  // the same set the legacy materializer derives as `effective_subjects`; `role_uuids` / `group_uuids` above stay the
  // principal's DIRECT memberships.
  std::vector<core::platform::Uuid> effective_role_uuids, effective_group_uuids;
  // S4e Part 1: the create-fixed canonical SYSARCH role UUID when the principal is EFFECTIVELY a member of it AND the role row
  // is the engine-owned, immutable, active SYSARCH role (authority class engine_owned_sysarch, as the native runtime
  // projection verifies it for L5); nil otherwise. It is what the legacy path derived from the bootstrap catalog
  // (ResolveEngineOwnedSysarchRoleIdentity): the authorization materializer grants the engine-owned bundle to a member.
  core::platform::Uuid sysarch_role_uuid;
  // Every committed, active grant-family row that names the principal as subject or member (privileges,
  // memberships, agent bindings) PLUS (S4e Part 1) the membership rows whose member is a role or group of the
  // effective sets and the privilege rows (allow and deny) whose subject is the principal's effective role or
  // group: the adapter's input. Retired / deleted / not-active rows are never edges.
  std::vector<NativeLoginGrantEdge> edges;
};

// SEARCH_KEY: SB_NATIVE_ROLE_SET_DIGEST (S4e Part 1). The role-set digest the session ledger's SESSION record stores
// at +80 (the spec's role_set_hash). DEFINITION: SHA-256 over the concatenation of the 16-byte binary UUIDs of the
// role set (the effective roles), sorted ascending by their bytes, duplicates and nil UUIDs removed, with no prefix,
// separator or count. The empty set digests to SHA-256 of the empty input (a fixed non-zero value), so "computed, no
// roles" is told apart from the ledger's all-zero "none stated". The same function serves the group set (not stored:
// see the ledger record's layout note). Returns false only when the hash provider fails.
inline bool NativeUuidSetDigest(std::vector<core::platform::Uuid> uuids, std::array<core::platform::byte, 32>& out) {
  std::sort(uuids.begin(), uuids.end(), [](const auto& a, const auto& b) { return a.bytes < b.bytes; });
  uuids.erase(std::unique(uuids.begin(), uuids.end(), [](const auto& a, const auto& b) { return a.bytes == b.bytes; }),
              uuids.end());
  std::vector<core::platform::byte> bytes;
  for (const auto& id : uuids) {
    if (id.is_nil()) continue;
    bytes.insert(bytes.end(), id.bytes.begin(), id.bytes.end());
  }
  const auto digest = core::hash::ComputeSha256Digest(bytes);
  if (!digest.ok() || digest.digest.size() != out.size()) return false;
  std::copy(digest.digest.begin(), digest.digest.end(), out.begin());
  return true;
}

// SEARCH_KEY: SB_NATIVE_LOGIN_MATERIAL (thin-slice S4a). See the declaration and NativeLoginMaterialError.
inline const char* NativeLoginMaterialErrorName(NativeLoginMaterialError e) noexcept {
  using E=NativeLoginMaterialError;
  switch(e){
    case E::none: return "none";
    case E::invalid_request: return "invalid_request";
    case E::password_inadmissible: return "password_inadmissible";
    case E::database_not_serving: return "database_not_serving";
    case E::database_failed: return "database_failed";
    case E::wrong_owner: return "wrong_owner";
    case E::busy: return "busy";
    case E::closed: return "closed";
    case E::preparation_failure: return "preparation_failure";
    case E::custody_failure: return "custody_failure";
    case E::crypto_failure: return "crypto_failure";
    case E::memory_failure: return "memory_failure";
    case E::security_source_failure: return "security_source_failure";
    case E::security_binding_failure: return "security_binding_failure";
    case E::protected_workspace_required: return "protected_workspace_required";
    case E::verifier_provider_failure: return "verifier_provider_failure";
    case E::principal_absent: return "principal_absent";
    case E::principal_wrong_family: return "principal_wrong_family";
    case E::principal_not_active: return "principal_not_active";
    case E::credential_absent: return "credential_absent";
    case E::credential_unsupported: return "credential_unsupported";
    case E::password_mismatch: return "password_mismatch";
    case E::principal_generation_invalid: return "principal_generation_invalid";
    case E::external_binding_refused: return "external_binding_refused";
  }
  return "unknown";
}

inline const char* NativeLoginMaterialErrorCode(NativeLoginMaterialError e) noexcept {
  using E=NativeLoginMaterialError;
  switch(e){
    case E::none: return "";
    case E::invalid_request: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_REQUEST_INVALID";
    case E::password_inadmissible: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_PASSWORD_INADMISSIBLE";
    case E::database_not_serving: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_DATABASE_NOT_SERVING";
    case E::database_failed: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_DATABASE_FAILED";
    case E::wrong_owner: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_READ_WRONG_OWNER";
    case E::busy: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_READ_BUSY";
    case E::closed: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_READ_CLOSED";
    case E::preparation_failure: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_READ_PREPARATION_FAILED";
    case E::custody_failure: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_READ_CUSTODY_FAILED";
    case E::crypto_failure: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_READ_CRYPTO_SCOPE_FAILED";
    case E::memory_failure: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_READ_MEMORY_FAILED";
    case E::security_source_failure: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_SECURITY_SOURCE_FAILED";
    case E::security_binding_failure: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_SECURITY_BINDING_FAILED";
    case E::protected_workspace_required: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_PROTECTED_WORKSPACE_REQUIRED";
    case E::verifier_provider_failure: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_VERIFIER_PROVIDER_FAILED";
    case E::principal_absent: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_PRINCIPAL_ABSENT";
    case E::principal_wrong_family: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_PRINCIPAL_WRONG_FAMILY";
    case E::principal_not_active: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_PRINCIPAL_NOT_ACTIVE";
    case E::credential_absent: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_CREDENTIAL_ABSENT";
    case E::credential_unsupported: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_CREDENTIAL_UNSUPPORTED";
    case E::password_mismatch: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_PASSWORD_MISMATCH";
    case E::principal_generation_invalid: return "SECURITY.AUTHENTICATION.NATIVE_LOGIN_PRINCIPAL_GENERATION_INVALID";
    // Q274: the SAME code CheckExternalAuthenticationBinding returns for a service principal.
    case E::external_binding_refused: return "SECURITY.AGENT_PRINCIPAL_EXTERNAL_BINDING_REFUSED";
  }
  return "";
}

// ---- the login source and one attempt's result (S4c) ------------------------------------------------------------
// SEARCH_KEY: SB_NATIVE_LOGIN_SOURCE. One native login = ONE snapshot-point scope on the database loop thread (owner
// 2026-11-21: login reads run at an in-memory snapshot point, no durable transaction, nothing written, no number
// burned; the user's transaction begins only on approval, S4e). Inside it, in sequence (never nested: a nested runtime
// read reports busy):
//   1. `select` (a callback the ENGINE supplies: name -> principal UUID through the existing resolver, hidden-name
//      policy) runs ON THE LOOP THREAD,
//   2. the S4a credential read for that principal (PBKDF2 included) with the password the caller handed over.
// The password is the only secret: the source copies it into protected memory on the loop thread and scrubs the
// caller's bytes. Everything that reaches a client is decided by the caller (EngineAuthenticate); the specific causes
// here are INTERNAL.
enum class NativeLoginSelectionError : std::uint8_t {
  none,
  // The database's principal name scope (the authorization policy body's user schema) could not be read from the
  // names snapshot: absent, or more than one distinct value (a configuration fault, not a credential failure).
  scope_absent, scope_ambiguous,
  // The names read itself failed (infrastructure): `names_error` says how.
  names_unavailable,
  // The resolver answered, and the name is not a usable login: unknown, hidden (S4b Q1: a hidden-class name does not
  // authenticate), or two different objects match.
  name_not_found, name_hidden, name_ambiguous,
  // The resolver refused for another reason (its code is in `resolver_code`).
  resolver_failed
};
inline const char* NativeLoginSelectionErrorName(NativeLoginSelectionError e) noexcept {
  switch (e) {
    case NativeLoginSelectionError::none: return "none";
    case NativeLoginSelectionError::scope_absent: return "scope_absent";
    case NativeLoginSelectionError::scope_ambiguous: return "scope_ambiguous";
    case NativeLoginSelectionError::names_unavailable: return "names_unavailable";
    case NativeLoginSelectionError::name_not_found: return "name_not_found";
    case NativeLoginSelectionError::name_hidden: return "name_hidden";
    case NativeLoginSelectionError::name_ambiguous: return "name_ambiguous";
    case NativeLoginSelectionError::resolver_failed: return "resolver_failed";
  }
  return "unknown";
}
struct NativeLoginSelection {
  NativeLoginSelectionError error = NativeLoginSelectionError::name_not_found;
  NativeNamesSnapshotError names_error = NativeNamesSnapshotError::none;  // names_unavailable
  std::string resolver_code;                                              // resolver_failed: a diagnostic code token
  core::platform::Uuid principal_uuid;                                    // none: the one principal the name resolved to
  bool ok() const noexcept { return error == NativeLoginSelectionError::none; }
};

// Where the scope itself refused or failed (the work did not complete): infrastructure, never a credential verdict.
enum class NativeLoginScopeError : std::uint8_t { none, database_not_serving, database_failed, busy, scope_refused, work_failed };
inline const char* NativeLoginScopeErrorName(NativeLoginScopeError e) noexcept {
  switch (e) {
    case NativeLoginScopeError::none: return "none";
    case NativeLoginScopeError::database_not_serving: return "database_not_serving";
    case NativeLoginScopeError::database_failed: return "database_failed";
    case NativeLoginScopeError::busy: return "busy";
    case NativeLoginScopeError::scope_refused: return "scope_refused";
    case NativeLoginScopeError::work_failed: return "work_failed";
  }
  return "unknown";
}

struct NativeLoginAttemptResult {
  NativeLoginScopeError scope_error = NativeLoginScopeError::scope_refused;
  NativeLoginSelection selection;
  // Meaningful when the scope ran and the selection is ok, OR when a burn read ran for a failed selection (the
  // timing equaliser: see the loop). `material_error` is what the credential read said; `material` only on success.
  NativeLoginMaterialError material_error = NativeLoginMaterialError::invalid_request;
  std::optional<NativeLoginMaterial> material;
  // Structured detail of the credential read's infrastructure refusal (enum numbers, never text), for the audit line.
  std::uint32_t security_error = 0, source_error = 0, custody_error = 0;
  bool ok() const noexcept {
    return scope_error == NativeLoginScopeError::none && selection.ok() &&
           material_error == NativeLoginMaterialError::none && material.has_value();
  }
};

class NativeLoginSource {
 public:
  virtual ~NativeLoginSource() = default;
  // Callable from any thread (a request on the loop's queue; from the loop thread it runs directly). `select` runs on
  // the loop thread. `password` (the first bytes of the caller's string) is consumed: copied into protected memory
  // on the loop thread and the caller's bytes are scrubbed whatever the result. Never throws.
  virtual NativeLoginAttemptResult Login(const std::function<NativeLoginSelection()>& select,
                                         std::string& password) const noexcept = 0;
};

}  // namespace scratchbird::engine::internal_api
