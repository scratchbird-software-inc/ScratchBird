// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/auth_credential_api.hpp"
#include "security/auth_provider_plugin_api.hpp"
#include "security/external_group_api.hpp"
#include "security/protected_material_api.hpp"
#include "security/security_crypto_policy.hpp"
#include "uuid.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1770700000000ull;

bool Expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

api::EngineUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  api::EngineUuid out;
  if (generated.ok()) {
    out.canonical = uuid::UuidToString(generated.value.value);
  }
  return out;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.rfind(prefix, 0) == 0;
}

api::EngineMaterializedAuthorizationContext AuthorizationContext(
    const api::EngineUuid& principal_uuid,
    u64 security_epoch,
    u64 catalog_generation_id) {
  api::EngineMaterializedAuthorizationContext context;
  context.present = true;
  context.authority_uuid = MakeUuid(UuidKind::object, 200);
  context.principal_uuid = principal_uuid;
  context.security_epoch = security_epoch;
  context.policy_epoch = 14;
  context.catalog_generation_id = catalog_generation_id;
  context.effective_subjects.push_back({principal_uuid, "principal"});
  const std::vector<std::string> rights = {
      "AUTH_PROVIDER_ADMIN",
      "CONNECT",
      "KEY_RELEASE_APPROVE",
      "PROTECTED_MATERIAL_RELEASE"};
  u64 grant_offset = 210;
  for (const auto& right : rights) {
    context.grants.push_back({MakeUuid(UuidKind::object, grant_offset++),
                              principal_uuid,
                              "principal",
                              {},
                              right,
                              false,
                              security_epoch});
  }
  context.evidence_tags.push_back("durable_authorization_context");
  return context;
}

bool HasDiagnosticCode(const api::EngineApiResult& result,
                       std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasEvidenceKind(const api::EngineApiResult& result,
                     std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

std::string FlattenResult(const api::EngineApiResult& result) {
  std::string out;
  for (const auto& diagnostic : result.diagnostics) {
    out += diagnostic.code;
    out += '|';
    out += diagnostic.detail;
    out += '\n';
  }
  for (const auto& evidence : result.evidence) {
    out += evidence.evidence_kind;
    out += '=';
    out += evidence.evidence_id;
    out += '\n';
  }
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      out += field.first;
      out += '=';
      out += field.second.encoded_value;
      out += '\n';
    }
  }
  return out;
}

bool ExpectResult(bool condition,
                  std::string_view message,
                  const api::EngineApiResult& result) {
  if (condition) { return true; }
  std::cerr << message << '\n' << FlattenResult(result);
  return false;
}

api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                  u64 tx = 92,
                                  u64 snapshot = 92,
                                  u64 resource_epoch = 1770700001000ull) {
  static const api::EngineUuid kDatabaseUuid = MakeUuid(UuidKind::database, 1);
  static const api::EngineUuid kPrincipalUuid = MakeUuid(UuidKind::principal, 2);
  static const api::EngineUuid kSessionUuid = MakeUuid(UuidKind::object, 3);
  static const api::EngineUuid kTransactionUuid =
      MakeUuid(UuidKind::transaction, 4);
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "pcr092-security-provider-contract";
  context.database_path = database_path.string();
  context.database_uuid = kDatabaseUuid;
  context.principal_uuid = kPrincipalUuid;
  context.session_uuid = kSessionUuid;
  context.transaction_uuid = kTransactionUuid;
  context.local_transaction_id = tx;
  context.snapshot_visible_through_local_transaction_id = snapshot;
  context.security_context_present = true;
  context.catalog_generation_id = 12;
  context.security_epoch = 13;
  context.resource_epoch = resource_epoch;
  context.authorization_context =
      AuthorizationContext(context.principal_uuid,
                           context.security_epoch,
                           context.catalog_generation_id);
  return context;
}

api::EngineProtectedMaterialPolicySet PolicySet(u64 offset,
                                                u64 retention_until = 0,
                                                bool legal_hold = false) {
  api::EngineProtectedMaterialPolicySet policy;
  policy.retention_policy_uuid = MakeUuid(UuidKind::object, offset).canonical;
  policy.access_policy_uuid = MakeUuid(UuidKind::object, offset + 1).canonical;
  policy.release_policy_uuid = MakeUuid(UuidKind::object, offset + 2).canonical;
  policy.purge_policy_uuid = MakeUuid(UuidKind::object, offset + 3).canonical;
  policy.audit_policy_uuid = MakeUuid(UuidKind::object, offset + 4).canonical;
  policy.retention_until_epoch_millis = retention_until;
  policy.legal_hold = legal_hold;
  policy.release_purposes.push_back("security_use");
  return policy;
}

api::EngineAuthenticateProviderRequest ProviderRequest(
    const std::filesystem::path& database_path,
    std::string provider) {
  api::EngineAuthenticateProviderRequest request;
  request.context = Context(database_path);
  request.option_envelopes.push_back("provider:" + std::move(provider));
  request.option_envelopes.push_back("provider_enabled:true");
  request.option_envelopes.push_back("provider_lifecycle_state:healthy");
  request.option_envelopes.push_back("policy_generation_current:12");
  request.option_envelopes.push_back("security_epoch_current:13");
  request.option_envelopes.push_back("provider_generation_current:5");
  return request;
}

bool TestProviderContracts(const std::filesystem::path& database_path) {
  bool ok = true;

  auto stale = ProviderRequest(database_path, "token_api_key");
  stale.option_envelopes.push_back("provider_generation_observed:4");
  const auto stale_result = api::EngineAuthenticateProvider(stale);
  ok = Expect(!stale_result.ok &&
                  HasDiagnosticCode(stale_result, "SECURITY.POLICY.STALE"),
              "stale provider generation should fail closed") && ok;

  auto ldap_missing_dependency = ProviderRequest(database_path, "ldap_ad");
  ldap_missing_dependency.option_envelopes.push_back("principal:alice");
  ldap_missing_dependency.option_envelopes.push_back("fixture:success");
  const auto missing_dependency =
      api::EngineAuthenticateProvider(ldap_missing_dependency);
  ok = Expect(!missing_dependency.ok &&
                  HasDiagnosticCode(missing_dependency,
                                    "SECURITY.AUTH_SOURCE_UNAVAILABLE"),
              "external LDAP provider should require explicit dependency") && ok;

  auto scram_no_binding = ProviderRequest(database_path, "scram_sha256");
  scram_no_binding.option_envelopes.push_back("principal:alice");
  scram_no_binding.option_envelopes.push_back("fixture:success");
  const auto no_binding = api::EngineAuthenticateProvider(scram_no_binding);
  ok = Expect(!no_binding.ok &&
                  HasDiagnosticCode(no_binding,
                                    "SECURITY.CHANNEL_BINDING_REQUIRED"),
              "SCRAM provider should require verified channel binding") && ok;

  auto scram_ok = scram_no_binding;
  scram_ok.option_envelopes.push_back("channel_binding_verified:true");
  const auto binding_ok = api::EngineAuthenticateProvider(scram_ok);
  ok = Expect(binding_ok.ok && binding_ok.authenticated,
              "SCRAM provider with channel binding and verifier evidence should authenticate") && ok;

  auto parser_mismatch = ProviderRequest(database_path, "oidc_jwt");
  parser_mismatch.option_envelopes.push_back("parser_attach_provider:true");
  parser_mismatch.option_envelopes.push_back("parser_policy_requires_provider:ldap_ad");
  parser_mismatch.option_envelopes.push_back("client_dependency:oidc_jwt_client:available");
  parser_mismatch.option_envelopes.push_back(
      "provider_payload:iss=idp;aud=scratchbird;sub=alice;alg=rs256;exp=9999999999999;sig=" +
      api::SecuritySha256Hex("oidc-sig") +
      ";groups=reader;validator=jwks");
  const auto parser_denied = api::EngineAuthenticateProvider(parser_mismatch);
  ok = Expect(!parser_denied.ok &&
                  HasDiagnosticCode(parser_denied,
                                    "SECURITY.AUTHENTICATION.REQUEST_INVALID"),
              "parser provider mismatch should fail before provider authentication") && ok;

  auto replay = ProviderRequest(database_path, "oidc_jwt");
  replay.option_envelopes.push_back("client_dependency:oidc_jwt_client:available");
  replay.option_envelopes.push_back(
      "provider_payload:iss=idp;aud=scratchbird;sub=alice;alg=rs256;exp=9999999999999;sig=" +
      api::SecuritySha256Hex("oidc-sig") +
      ";groups=reader;validator=jwks;replay=true");
  const auto replay_denied = api::EngineAuthenticateProvider(replay);
  ok = Expect(!replay_denied.ok &&
                  HasDiagnosticCode(replay_denied,
                                    "SECURITY.AUTHENTICATION.FAILED"),
              "replayed provider assertion should fail closed") && ok;

  auto alg_none = ProviderRequest(database_path, "oidc_jwt");
  alg_none.option_envelopes.push_back("client_dependency:oidc_jwt_client:available");
  alg_none.option_envelopes.push_back(
      "provider_payload:iss=idp;aud=scratchbird;sub=alice;alg=none;exp=9999999999999;sig=" +
      api::SecuritySha256Hex("oidc-sig") +
      ";groups=reader;validator=jwks");
  const auto alg_denied = api::EngineAuthenticateProvider(alg_none);
  ok = Expect(!alg_denied.ok &&
                  HasDiagnosticCode(alg_denied,
                                    "SECURITY.AUTHENTICATION.FAILED"),
              "provider algorithm downgrade should fail closed") && ok;

  auto ldap_ok = ProviderRequest(database_path, "ldap_ad");
  ldap_ok.option_envelopes.push_back("client_mode:external");
  ldap_ok.option_envelopes.push_back("client_dependency:ldap_client:available");
  ldap_ok.option_envelopes.push_back(
      "provider_payload:user=alice;endpoint=ldap.example;starttls=true;bind=allow;password=protected_ref;groups=reader;path=alice,reader;client_mode=external;client_result=success");
  const auto authenticated = api::EngineAuthenticateProvider(ldap_ok);
  ok = ExpectResult(authenticated.ok && authenticated.authenticated,
                    "LDAP provider with live external evidence should authenticate",
                    authenticated) && ok;
  ok = ExpectResult(HasEvidenceKind(authenticated,
                                    "auth_provider_real_external_client"),
                    "LDAP provider should preserve real external client evidence",
                    authenticated) && ok;

  return ok;
}

bool TestExternalGroupContracts(const std::filesystem::path& database_path) {
  bool ok = true;

  api::EngineSyncExternalGroupsRequest sync;
  sync.context = Context(database_path);
  sync.provider_family = "directory.ldap";
  sync.option_envelopes.push_back("external_group:cn=reader");
  sync.option_envelopes.push_back("internal_group_uuid:" +
                                  MakeUuid(UuidKind::object, 40).canonical);
  const auto synced = api::EngineSyncExternalGroups(sync);
  ok = Expect(synced.ok && synced.materialized,
              "canonical LDAP external group should materialize") && ok;
  ok = Expect(FlattenResult(synced).find("provider_family=ldap_ad") !=
                  std::string::npos,
              "external group sync should report canonical provider family") && ok;
  ok = Expect(FlattenResult(synced).find("ordinary_authz_live_lookup=false") !=
                  std::string::npos,
              "external group sync should record no live authorization lookup") && ok;

  api::EngineExplainMembershipRequest explain;
  explain.context = Context(database_path);
  explain.provider_family = "directory.ldap_ad_starttls";
  const auto explained = api::EngineExplainMembership(explain);
  ok = Expect(explained.ok && explained.explainable,
              "canonical LDAP membership path should be explainable") && ok;

  api::EngineSyncExternalGroupsRequest cluster = sync;
  cluster.provider_family = "cluster.security_provider";
  cluster.context.cluster_authority_available = false;
  const auto cluster_denied = api::EngineSyncExternalGroups(cluster);
  ok = Expect(!cluster_denied.ok && cluster_denied.cluster_authority_required &&
                  HasDiagnosticCode(cluster_denied, "PROCESS.CLUSTER_PATH_ABSENT"),
              "cluster group materialization should require external cluster authority") && ok;

  api::EngineSyncExternalGroupsRequest unknown = sync;
  unknown.provider_family = "made_up_provider";
  const auto unknown_denied = api::EngineSyncExternalGroups(unknown);
  ok = Expect(!unknown_denied.ok &&
                  HasDiagnosticCode(unknown_denied,
                                    "SECURITY.AUTHORITY.INVALID"),
              "unknown external group provider should fail closed") && ok;

  return ok;
}

bool TestCredentialRotationContracts(const std::filesystem::path& database_path) {
  bool ok = true;

  api::EngineRotateCredentialRequest rotate;
  rotate.context = Context(database_path);
  rotate.option_envelopes.push_back("provider:local_password");
  rotate.option_envelopes.push_back("protected_material:available");
  rotate.option_envelopes.push_back("protected_material_generation_current:8");
  const auto rotated = api::EngineRotateCredential(rotate);
  ok = Expect(rotated.ok && rotated.rotated,
              "local password credential rotation should admit protected material") && ok;
  ok = Expect(HasEvidenceKind(rotated, "credential_rotation_audit"),
              "credential rotation should preserve audit evidence") && ok;
  ok = Expect(FlattenResult(rotated).find("plaintext_persisted=false") !=
                  std::string::npos,
              "credential rotation should not persist plaintext") && ok;

  auto stale = rotate;
  stale.option_envelopes.push_back("protected_material_generation_observed:7");
  const auto stale_rotation = api::EngineRotateCredential(stale);
  ok = Expect(!stale_rotation.ok &&
                  HasDiagnosticCode(stale_rotation, "SECURITY.POLICY.STALE"),
              "stale protected-material generation should block rotation") && ok;

  auto replayed = rotate;
  replayed.option_envelopes.push_back("replayed:true");
  const auto replay_rotation = api::EngineRotateCredential(replayed);
  ok = Expect(!replay_rotation.ok &&
                  HasDiagnosticCode(replay_rotation,
                                    "SECURITY.AUTHENTICATION.FAILED"),
              "replayed rotation evidence should fail closed") && ok;

  api::EngineRotateCredentialRequest unsupported;
  unsupported.context = Context(database_path);
  unsupported.option_envelopes.push_back("provider:kerberos_pac");
  unsupported.option_envelopes.push_back("protected_material:available");
  unsupported.option_envelopes.push_back("protected_material_generation_current:8");
  const auto unsupported_rotation = api::EngineRotateCredential(unsupported);
  ok = Expect(!unsupported_rotation.ok &&
                  HasDiagnosticCode(unsupported_rotation,
                                    "SECURITY.AUTH_PROVIDER_UNSUPPORTED"),
              "provider without rotation capability should fail closed") && ok;

  auto failed = rotate;
  failed.option_envelopes.push_back("rotation_provider_failed:true");
  const auto provider_failed = api::EngineRotateCredential(failed);
  ok = Expect(!provider_failed.ok &&
                  HasDiagnosticCode(provider_failed, "SECURITY.KEY.UNAVAILABLE"),
              "provider-side rotation failure should fail closed") && ok;

  return ok;
}

bool TestProtectedMaterialLifecycle(const std::filesystem::path& database_path) {
  bool ok = true;

  api::EnginePurgeProtectedMaterialRequest purge_cache;
  purge_cache.context = Context(database_path);
  purge_cache.option_envelopes.push_back("key_authority:engine");
  (void)api::EnginePurgeProtectedMaterial(purge_cache);

  api::EngineAdmitEncryptionKeyRequest admit;
  admit.context = Context(database_path);
  admit.key_uuid = "pcr092-key-old";
  admit.key_label = "redacted-key";
  admit.filespace_uuid = MakeUuid(UuidKind::object, 50).canonical;
  admit.secret_evidence = "kms-ref:v1:pcr092-proof";
  admit.option_envelopes.push_back("key_authority:engine");
  const auto admitted = api::EngineAdmitEncryptionKey(admit);
  ok = Expect(admitted.ok && admitted.key_admitted,
              "protected material key should admit") && ok;
  ok = Expect(StartsWith(admitted.key_fingerprint,
                         "fingerprint:v1:hmac-sha256:"),
              "protected material fingerprint should use HMAC-SHA-256") && ok;
  ok = Expect(StartsWith(admitted.key_handle,
                         "protected-material-handle:v1:hmac-sha256:"),
              "protected material handle should use HMAC-SHA-256") && ok;
  ok = Expect(admitted.key_fingerprint.find("fnv") == std::string::npos &&
                  admitted.key_handle.find("fnv") == std::string::npos,
              "protected material key outputs should not expose legacy FNV markers") && ok;

  api::EngineRotateEncryptionKeyRequest rotate;
  rotate.context = Context(database_path);
  rotate.key_uuid = "pcr092-key-old";
  rotate.replacement_key_uuid = "pcr092-key-new";
  rotate.replacement_secret_evidence = "kms-ref:v1:pcr092-rotated-proof";
  rotate.rotation_reason = "scheduled public release key rotation";
  rotate.option_envelopes.push_back("key_authority:engine");
  const auto rotated = api::EngineRotateEncryptionKey(rotate);
  ok = ExpectResult(rotated.ok && rotated.rotated &&
                        StartsWith(rotated.active_key_handle,
                                   "protected-material-handle:v1:hmac-sha256:"),
                    "rotated protected material key should use HMAC-SHA-256 handle",
                    rotated) && ok;

  api::EngineOpenEncryptedFilespaceRequest old_open;
  old_open.context = Context(database_path);
  old_open.database_uuid = old_open.context.database_uuid.canonical;
  old_open.filespace_uuid = admit.filespace_uuid;
  old_open.key_uuid = "pcr092-key-old";
  old_open.key_handle = admitted.key_handle;
  old_open.option_envelopes.push_back("filespace_open_authority:engine");
  const auto old_denied = api::EngineOpenEncryptedFilespace(old_open);
  ok = ExpectResult(!old_denied.ok &&
                        HasDiagnosticCode(old_denied,
                                          "SECURITY.KEY.UNAVAILABLE"),
                    "rotated-out protected material key should not remain active",
                    old_denied) && ok;

  api::EngineCreateProtectedMaterialRequest create;
  create.context = Context(database_path, 100, 100);
  create.protected_material_uuid = MakeUuid(UuidKind::object, 60).canonical;
  create.owner_scope_uuid = create.context.principal_uuid.canonical;
  create.purpose_class = "security_use";
  create.storage_class = "wrapped";
  create.policy = PolicySet(70);
  create.initial_version_uuid = MakeUuid(UuidKind::object, 80).canonical;
  create.protected_reference = "wrapped-ref:v1:pcr092-initial";
  create.envelope_reference = "envelope-ref:v1:pcr092-initial";
  create.payload_hash = "sha256:" + api::SecuritySha256Hex("pcr092-initial");
  create.option_envelopes.push_back("key_authority:engine");
  const auto created = api::EngineCreateProtectedMaterial(create);
  ok = Expect(created.ok && created.created && created.initial_version_created,
              "protected material catalog create should persist initial version") && ok;

  api::EngineAddProtectedMaterialVersionRequest add;
  add.context = Context(database_path, 101, 101);
  add.protected_material_uuid = create.protected_material_uuid;
  add.protected_material_version_uuid = MakeUuid(UuidKind::object, 81).canonical;
  add.protected_reference = "wrapped-ref:v1:pcr092-rotated";
  add.envelope_reference = "envelope-ref:v1:pcr092-rotated";
  add.payload_hash = "sha256:" + api::SecuritySha256Hex("pcr092-rotated");
  add.storage_class = "wrapped";
  add.rotation_reason = "pcr092 protected material version rotation";
  add.policy_override = PolicySet(90, 1770709999000ull, true);
  add.option_envelopes.push_back("key_authority:engine");
  const auto added = api::EngineAddProtectedMaterialVersion(add);
  ok = ExpectResult(added.ok && added.version_added &&
                        added.active_version_number == 2,
                    "protected material version rotation should create version two",
                    added) && ok;

  api::EngineResolveProtectedMaterialRequest old_resolve;
  old_resolve.context = Context(database_path, 0, 100);
  old_resolve.protected_material_uuid = create.protected_material_uuid;
  old_resolve.purpose = "security_use";
  old_resolve.option_envelopes.push_back("protected_material_authority:engine");
  const auto old_visible = api::EngineResolveProtectedMaterial(old_resolve);
  ok = ExpectResult(old_visible.ok && old_visible.version_number == 1 &&
                        StartsWith(old_visible.protected_material_ref,
                                   "protected-material-ref:v1:sha256:"),
                    "old snapshot should resolve version one with SHA-256 ref",
                    old_visible) && ok;

  api::EngineResolveProtectedMaterialRequest new_resolve = old_resolve;
  new_resolve.context = Context(database_path, 0, 101);
  const auto new_visible = api::EngineResolveProtectedMaterial(new_resolve);
  ok = ExpectResult(new_visible.ok && new_visible.version_number == 2 &&
                        new_visible.protected_material_version_uuid ==
                            add.protected_material_version_uuid,
                    "new snapshot should resolve rotated protected material version",
                    new_visible) && ok;

  api::EngineReleaseProtectedMaterialRequest release;
  release.context = Context(database_path, 0, 101);
  release.protected_material_uuid = create.protected_material_uuid;
  release.purpose = "security_use";
  release.option_envelopes.push_back("protected_material_authority:engine");
  const auto released = api::EngineReleaseProtectedMaterial(release);
  ok = ExpectResult(released.ok && released.released &&
                        StartsWith(released.release_handle,
                                   "protected-material-release:v1:hmac-sha256:"),
                    "protected material release should use HMAC-SHA-256 handle",
                    released) && ok;
  ok = ExpectResult(StartsWith(released.audit_event_uuid,
                               "protected-material-audit:v1:sha256:"),
                    "protected material release audit event should use SHA-256 id",
                    released) && ok;
  ok = Expect(!released.plaintext_material_returned &&
                  released.redaction_applied,
              "protected material release should stay redacted") && ok;

  api::EnginePurgeProtectedMaterialVersionRequest purge_version;
  purge_version.context = Context(database_path, 102, 102);
  purge_version.protected_material_uuid = create.protected_material_uuid;
  purge_version.protected_material_version_uuid =
      add.protected_material_version_uuid;
  purge_version.purge_reason = "retention check";
  purge_version.option_envelopes.push_back("key_authority:engine");
  const auto purge_denied =
      api::EnginePurgeProtectedMaterialVersion(purge_version);
  ok = ExpectResult(!purge_denied.ok && purge_denied.refused_by_retention &&
                        purge_denied.audit_preserved &&
                        HasDiagnosticCode(purge_denied,
                                          "SECURITY.PROTECTED_MATERIAL.RETENTION_REQUIRED"),
                    "retention and legal hold should block protected material purge",
                    purge_denied) && ok;
  ok = ExpectResult(StartsWith(purge_denied.audit_event_uuid,
                               "protected-material-audit:v1:sha256:"),
                    "protected material purge refusal audit should use SHA-256 id",
                    purge_denied) && ok;

  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path work_dir =
      argc > 1 ? std::filesystem::path(argv[1])
               : std::filesystem::temp_directory_path() /
                     "scratchbird_pcr092_security_provider_contract";
  std::error_code ignored;
  std::filesystem::remove_all(work_dir, ignored);
  std::filesystem::create_directories(work_dir);
  const auto database_path = work_dir / "pcr092.sbdb";

  bool ok = true;
  ok = TestProviderContracts(database_path) && ok;
  ok = TestExternalGroupContracts(database_path) && ok;
  ok = TestCredentialRotationContracts(database_path) && ok;
  ok = TestProtectedMaterialLifecycle(database_path) && ok;

  std::filesystem::remove_all(work_dir, ignored);
  return ok ? 0 : 1;
}
