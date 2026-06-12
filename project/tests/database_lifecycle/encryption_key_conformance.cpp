// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/protected_material_api.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace engine_api = scratchbird::engine::internal_api;

constexpr std::string_view kDatabaseA = "019e18d0-013a-7000-8000-000000000013";
constexpr std::string_view kDatabaseB = "019e18d0-013b-7000-8000-000000000013";
constexpr std::string_view kFilespaceA = "019e18d0-013c-7000-8000-000000000013";
constexpr std::string_view kFilespaceB = "019e18d0-013d-7000-8000-000000000013";
constexpr std::string_view kPlaintextSecret = "CorrectHorseBatteryStaple-DBLC013Z";
constexpr std::string_view kProtectedMaterialUuid = "019e18d0-0140-7000-8000-000000000013";
constexpr std::string_view kProtectedMaterialVersionA = "019e18d0-0141-7000-8000-000000000013";
constexpr std::string_view kProtectedMaterialVersionB = "019e18d0-0142-7000-8000-000000000013";
constexpr std::string_view kPolicyRetention = "019e18d0-0143-7000-8000-000000000013";
constexpr std::string_view kPolicyAccess = "019e18d0-0144-7000-8000-000000000013";
constexpr std::string_view kPolicyRelease = "019e18d0-0145-7000-8000-000000000013";
constexpr std::string_view kPolicyPurge = "019e18d0-0146-7000-8000-000000000013";
constexpr std::string_view kPolicyAudit = "019e18d0-0147-7000-8000-000000000013";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013z_encryption_key.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013Z encryption-key test");
  return std::filesystem::path(made);
}

engine_api::EngineRequestContext Context(const std::filesystem::path& database_path,
                                         std::string_view database_uuid = kDatabaseA,
                                         std::uint64_t epoch = 1000,
                                         std::uint64_t local_transaction_id = 1) {
  engine_api::EngineRequestContext context;
  context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(database_uuid);
  context.security_context_present = true;
  context.trace_tags.push_back("security.bootstrap");
  context.resource_epoch = epoch;
  context.local_transaction_id = local_transaction_id;
  context.snapshot_visible_through_local_transaction_id = local_transaction_id;
  std::ofstream touch(database_path, std::ios::app);
  Require(static_cast<bool>(touch), "DBLC-013Z database fixture create failed");
  return context;
}

engine_api::EngineRequestContext NoAuthorityContext(const std::filesystem::path& database_path) {
  engine_api::EngineRequestContext context;
  context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(kDatabaseA);
  context.resource_epoch = 1000;
  return context;
}

bool HasDiagnostic(const engine_api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
    if (diagnostic.detail == code) { return true; }
  }
  return false;
}

bool HasEvidence(const engine_api::EngineApiResult& result, std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

std::string FlattenResult(const engine_api::EngineApiResult& result) {
  std::ostringstream out;
  out << result.operation_id << '\n';
  for (const auto& diagnostic : result.diagnostics) {
    out << diagnostic.code << '\n' << diagnostic.message_key << '\n' << diagnostic.detail << '\n';
  }
  for (const auto& evidence : result.evidence) {
    out << evidence.evidence_kind << '\n' << evidence.evidence_id << '\n';
  }
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      out << field.first << '=' << field.second.encoded_value << '\n';
    }
  }
  return out.str();
}

void RequireNoLeak(const engine_api::EngineApiResult& result, std::string_view forbidden) {
  const auto flattened = FlattenResult(result);
  Require(flattened.find(forbidden) == std::string::npos,
          "DBLC-013Z protected material leaked into diagnostics/result evidence");
}

void WriteProtectedMaterialPageFixture(const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "DBLC-013Z physical erase fixture open failed");
  const std::string payload = "nonzero-protected-page-material-v2";
  for (int index = 0; index < 128; ++index) {
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
  }
  out.close();
  Require(static_cast<bool>(out), "DBLC-013Z physical erase fixture write failed");
}

bool FileAllZero(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) { return false; }
  bool saw_byte = false;
  char buffer[1024];
  while (input) {
    input.read(buffer, sizeof(buffer));
    const auto count = input.gcount();
    for (std::streamsize index = 0; index < count; ++index) {
      saw_byte = true;
      if (buffer[index] != '\0') { return false; }
    }
  }
  return saw_byte && input.eof();
}

engine_api::EngineAdmitEncryptionKeyResult Admit(const engine_api::EngineRequestContext& context,
                                                 std::string_view key_uuid,
                                                 std::string_view filespace_uuid = kFilespaceA,
                                                 std::uint64_t ttl = 300000,
                                                 std::string_view label = "primary database key") {
  engine_api::EngineAdmitEncryptionKeyRequest request;
  request.context = context;
  request.key_uuid = std::string(key_uuid);
  request.key_label = std::string(label);
  request.filespace_uuid = std::string(filespace_uuid);
  request.secret_evidence = "kms-ref:v1:tenant-a-key-proof";
  request.cache_ttl_millis = ttl;
  request.option_envelopes.push_back("key_authority:engine");
  return engine_api::EngineAdmitEncryptionKey(request);
}

engine_api::EngineOpenEncryptedFilespaceResult OpenEncrypted(
    const engine_api::EngineRequestContext& context,
    std::string_view key_handle,
    std::string_view database_uuid = kDatabaseA,
    std::string_view filespace_uuid = kFilespaceA,
    std::string_view key_uuid = {}) {
  engine_api::EngineOpenEncryptedFilespaceRequest request;
  request.context = context;
  request.database_uuid = std::string(database_uuid);
  request.filespace_uuid = std::string(filespace_uuid);
  request.key_uuid = std::string(key_uuid);
  request.key_handle = std::string(key_handle);
  request.option_envelopes.push_back("filespace_open_authority:engine");
  return engine_api::EngineOpenEncryptedFilespace(request);
}

void Purge(const engine_api::EngineRequestContext& context) {
  engine_api::EnginePurgeProtectedMaterialRequest request;
  request.context = context;
  request.purge_reason = "test_reset";
  request.option_envelopes.push_back("key_authority:engine");
  const auto purged = engine_api::EnginePurgeProtectedMaterial(request);
  Require(purged.ok, "DBLC-013Z setup purge failed");
}

void TestAdmissionInspectAndEncryptedOpen(const std::filesystem::path& database_path) {
  auto context = Context(database_path);
  Purge(context);

  const auto admitted = Admit(context, "key-dblc013z-admit", kFilespaceA, 300000,
                              "password=label-must-redact");
  Require(admitted.ok && admitted.key_admitted && admitted.cache_entry_active,
          "DBLC-013Z key admission failed");
  Require(!admitted.plaintext_material_returned,
          "DBLC-013Z key admission returned plaintext material");
  RequireNoLeak(admitted, kPlaintextSecret);

  engine_api::EngineInspectProtectedMaterialCacheRequest inspect;
  inspect.context = context;
  inspect.key_uuid = "key-dblc013z-admit";
  inspect.option_envelopes.push_back("protected_material_authority:engine");
  const auto inspected = engine_api::EngineInspectProtectedMaterialCache(inspect);
  Require(inspected.ok && inspected.protected_material_redacted,
          "DBLC-013Z protected-material inspect failed");
  Require(inspected.active_entry_count == 1,
          "DBLC-013Z inspect did not report one active cache entry");
  Require(!inspected.entries.empty() &&
              inspected.entries.front().key_label == "<protected-material-redacted>",
          "DBLC-013Z inspect did not redact protected key label");
  Require(FlattenResult(inspected).find("label-must-redact") == std::string::npos,
          "DBLC-013Z inspect leaked redacted key label");

  const auto opened = OpenEncrypted(context, admitted.key_handle, kDatabaseA, kFilespaceA,
                                    "key-dblc013z-admit");
  Require(opened.ok && opened.open_admitted && opened.key_cache_hit,
          "DBLC-013Z encrypted filespace open was not admitted with active key");
  Require(!opened.plaintext_material_returned,
          "DBLC-013Z encrypted filespace open returned plaintext material");
}

void TestScopeAndMissingAuthorityRefusals(const std::filesystem::path& database_path) {
  auto context = Context(database_path);
  Purge(context);
  const auto admitted = Admit(context, "key-dblc013z-scope");
  Require(admitted.ok, "DBLC-013Z scope setup admission failed");

  const auto wrong_database = OpenEncrypted(Context(database_path, kDatabaseB),
                                            admitted.key_handle,
                                            kDatabaseB,
                                            kFilespaceA,
                                            "key-dblc013z-scope");
  Require(!wrong_database.ok && HasDiagnostic(wrong_database, "SECURITY.KEY.SCOPE_MISMATCH"),
          "DBLC-013Z wrong database scope was not refused");

  const auto wrong_filespace = OpenEncrypted(context,
                                             admitted.key_handle,
                                             kDatabaseA,
                                             kFilespaceB,
                                             "key-dblc013z-scope");
  Require(!wrong_filespace.ok && HasDiagnostic(wrong_filespace, "SECURITY.KEY.SCOPE_MISMATCH"),
          "DBLC-013Z wrong filespace scope was not refused");

  engine_api::EngineAdmitEncryptionKeyRequest no_auth;
  no_auth.context = NoAuthorityContext(database_path);
  no_auth.key_uuid = "key-dblc013z-no-auth";
  no_auth.filespace_uuid = std::string(kFilespaceA);
  no_auth.secret_evidence = "kms-ref:v1:no-authority";
  const auto refused = engine_api::EngineAdmitEncryptionKey(no_auth);
  Require(!refused.ok && HasDiagnostic(refused, "SECURITY.PROTECTED_MATERIAL.AUTHORITY_DENIED"),
          "DBLC-013Z missing key authority was not refused");

  engine_api::EngineAdmitEncryptionKeyRequest parser_auth;
  parser_auth.context = context;
  parser_auth.key_uuid = "key-dblc013z-parser";
  parser_auth.filespace_uuid = std::string(kFilespaceA);
  parser_auth.secret_evidence = "kms-ref:v1:parser";
  parser_auth.option_envelopes.push_back("key_authority:parser");
  const auto parser_refused = engine_api::EngineAdmitEncryptionKey(parser_auth);
  Require(!parser_refused.ok &&
              HasDiagnostic(parser_refused,
                            "SECURITY.PROTECTED_MATERIAL.AUTHORITY_BYPASS_REFUSED"),
          "DBLC-013Z parser-owned key authority was not refused");
}

void TestRotationExpiryPurgeAndShutdown(const std::filesystem::path& database_path) {
  auto context = Context(database_path);
  Purge(context);
  const auto admitted = Admit(context, "key-dblc013z-rotate-old");
  Require(admitted.ok, "DBLC-013Z rotation setup admission failed");

  engine_api::EngineRotateEncryptionKeyRequest rotate;
  rotate.context = context;
  rotate.key_uuid = "key-dblc013z-rotate-old";
  rotate.replacement_key_uuid = "key-dblc013z-rotate-new";
  rotate.replacement_secret_evidence = "kms-ref:v1:rotated-proof";
  rotate.rotation_reason = "password=rotation-secret";
  rotate.option_envelopes.push_back("key_authority:engine");
  const auto rotated = engine_api::EngineRotateEncryptionKey(rotate);
  Require(rotated.ok && rotated.rotated && rotated.rotation_metadata_persisted,
          "DBLC-013Z key rotation failed");
  Require(!rotated.plaintext_material_persisted,
          "DBLC-013Z key rotation persisted plaintext material");
  Require(FlattenResult(rotated).find("rotation-secret") == std::string::npos,
          "DBLC-013Z rotation diagnostic/result leaked protected reason");

  const auto old_open = OpenEncrypted(context, admitted.key_handle, kDatabaseA, kFilespaceA,
                                      "key-dblc013z-rotate-old");
  Require(!old_open.ok && HasDiagnostic(old_open, "SECURITY.KEY.UNAVAILABLE"),
          "DBLC-013Z rotated-out key handle remained active");
  const auto new_open = OpenEncrypted(context, rotated.active_key_handle, kDatabaseA, kFilespaceA,
                                      "key-dblc013z-rotate-new");
  Require(new_open.ok && new_open.open_admitted,
          "DBLC-013Z rotated-in key handle was not admitted");

  auto short_context = Context(database_path, kDatabaseA, 2000);
  const auto short_lived = Admit(short_context, "key-dblc013z-expire", kFilespaceA, 2);
  Require(short_lived.ok, "DBLC-013Z short-lived admission failed");
  auto expired_context = Context(database_path, kDatabaseA, 2003);
  const auto expired_open = OpenEncrypted(expired_context,
                                          short_lived.key_handle,
                                          kDatabaseA,
                                          kFilespaceA,
                                          "key-dblc013z-expire");
  Require(!expired_open.ok && expired_open.key_expired &&
              HasDiagnostic(expired_open, "SECURITY.KEY.EXPIRED"),
          "DBLC-013Z expired key cache entry was not refused");

  Purge(context);
  const auto purged_open = OpenEncrypted(context, rotated.active_key_handle, kDatabaseA, kFilespaceA,
                                         "key-dblc013z-rotate-new");
  Require(!purged_open.ok && HasDiagnostic(purged_open, "SECURITY.KEY.UNAVAILABLE"),
          "DBLC-013Z purged key remained usable");

  const auto shutdown_key = Admit(context, "key-dblc013z-shutdown");
  Require(shutdown_key.ok, "DBLC-013Z shutdown setup admission failed");
  engine_api::EngineShutdownProtectedMaterialRequest shutdown;
  shutdown.context = context;
  shutdown.shutdown_reason = "shutdown password=must-redact";
  shutdown.option_envelopes.push_back("shutdown_authority:engine");
  const auto shut_down = engine_api::EngineShutdownProtectedMaterial(shutdown);
  Require(shut_down.ok && shut_down.shutdown_purge_complete,
          "DBLC-013Z shutdown purge failed");
  Require(FlattenResult(shut_down).find("must-redact") == std::string::npos,
          "DBLC-013Z shutdown purge leaked protected reason");
  const auto after_shutdown = OpenEncrypted(context,
                                            shutdown_key.key_handle,
                                            kDatabaseA,
                                            kFilespaceA,
                                            "key-dblc013z-shutdown");
  Require(!after_shutdown.ok && HasDiagnostic(after_shutdown, "SECURITY.KEY.UNAVAILABLE"),
          "DBLC-013Z shutdown did not purge protected-material cache");
}

void TestPlaintextRefusalAndNoDiagnosticLeak(const std::filesystem::path& database_path) {
  auto context = Context(database_path);
  Purge(context);
  engine_api::EngineAdmitEncryptionKeyRequest request;
  request.context = context;
  request.key_uuid = "key-dblc013z-plaintext";
  request.filespace_uuid = std::string(kFilespaceA);
  request.secret_evidence = std::string("password=") + std::string(kPlaintextSecret);
  request.option_envelopes.push_back("key_authority:engine");
  const auto refused = engine_api::EngineAdmitEncryptionKey(request);
  Require(!refused.ok && HasDiagnostic(refused, "SECURITY.KEY.PLAINTEXT_REFUSED"),
          "DBLC-013Z plaintext key evidence was not refused");
  RequireNoLeak(refused, kPlaintextSecret);
}

engine_api::EngineProtectedMaterialPolicySet MaterialPolicy(std::uint64_t retain_until = 0) {
  engine_api::EngineProtectedMaterialPolicySet policy;
  policy.retention_policy_uuid = std::string(kPolicyRetention);
  policy.access_policy_uuid = std::string(kPolicyAccess);
  policy.release_policy_uuid = std::string(kPolicyRelease);
  policy.purge_policy_uuid = std::string(kPolicyPurge);
  policy.audit_policy_uuid = std::string(kPolicyAudit);
  policy.retention_until_epoch_millis = retain_until;
  policy.release_purposes = {"filespace.open"};
  return policy;
}

void TestProtectedMaterialCatalogLifecycle(const std::filesystem::path& database_path) {
  const auto create_context = Context(database_path, kDatabaseA, 3000, 10);

  engine_api::EngineCreateProtectedMaterialRequest create;
  create.context = create_context;
  create.protected_material_uuid = std::string(kProtectedMaterialUuid);
  create.object_class = "filespace_encryption_key";
  create.owner_scope_uuid = std::string(kFilespaceA);
  create.purpose_class = "encryption_use";
  create.storage_class = "wrapped";
  create.policy = MaterialPolicy();
  create.initial_version_uuid = std::string(kProtectedMaterialVersionA);
  create.protected_reference = "kms-ref:v1:wrapped-material-a";
  create.envelope_reference = "kms-envelope:v1:wrapped-material-a";
  create.payload_hash = "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const auto created = engine_api::EngineCreateProtectedMaterial(create);
  Require(created.ok && created.created && created.initial_version_created,
          "DBLC-013Z protected material create failed");
  Require(!created.plaintext_material_stored && created.protected_material_redacted,
          "DBLC-013Z protected material create stored plaintext or skipped redaction");
  Require(created.active_version_number == 1,
          "DBLC-013Z protected material initial version number mismatch");
  RequireNoLeak(created, "wrapped-material-a");

  engine_api::EngineAddProtectedMaterialVersionRequest add;
  add.context = Context(database_path, kDatabaseA, 3500, 20);
  add.protected_material_uuid = std::string(kProtectedMaterialUuid);
  add.protected_material_version_uuid = std::string(kProtectedMaterialVersionB);
  add.protected_reference = "kms-ref:v1:wrapped-material-b";
  add.envelope_reference = "kms-envelope:v1:wrapped-material-b";
  add.payload_hash = "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  add.storage_class = "wrapped";
  add.rotation_reason = "password=rotation-reason-must-redact";
  add.policy_override = MaterialPolicy(5000);
  const auto added = engine_api::EngineAddProtectedMaterialVersion(add);
  Require(added.ok && added.version_added && added.active_version_changed,
          "DBLC-013Z protected material add-version failed");
  Require(!added.plaintext_material_stored && added.protected_material_redacted,
          "DBLC-013Z protected material add-version stored plaintext or skipped redaction");
  Require(added.active_version_uuid == kProtectedMaterialVersionB &&
              added.active_version_number == 2,
          "DBLC-013Z protected material active version did not rotate");
  RequireNoLeak(added, "rotation-reason-must-redact");
  RequireNoLeak(added, "wrapped-material-b");

  engine_api::EngineResolveProtectedMaterialRequest resolve;
  resolve.context = Context(database_path, kDatabaseA, 3600, 20);
  resolve.protected_material_uuid = std::string(kProtectedMaterialUuid);
  resolve.purpose = "filespace.open";
  const auto resolved = engine_api::EngineResolveProtectedMaterial(resolve);
  Require(resolved.ok && resolved.resolved && resolved.active_version_visible,
          "DBLC-013Z protected material resolve failed");
  Require(resolved.protected_material_version_uuid == kProtectedMaterialVersionB,
          "DBLC-013Z protected material resolve did not choose active version");
  Require(!resolved.protected_material_ref.empty(),
          "DBLC-013Z protected material resolve did not return opaque reference");
  RequireNoLeak(resolved, "wrapped-material-b");

  engine_api::EngineReleaseProtectedMaterialRequest release;
  release.context = Context(database_path, kDatabaseA, 3700, 20);
  release.protected_material_uuid = std::string(kProtectedMaterialUuid);
  release.purpose = "filespace.open";
  const auto released = engine_api::EngineReleaseProtectedMaterial(release);
  Require(released.ok && released.released && !released.policy_denied,
          "DBLC-013Z protected material release failed");
  Require(!released.plaintext_material_returned && !released.release_handle.empty(),
          "DBLC-013Z protected material release returned plaintext or no handle");
  RequireNoLeak(released, "wrapped-material-b");

  engine_api::EngineReleaseProtectedMaterialRequest denied_release = release;
  denied_release.purpose = "support.bundle.export";
  const auto denied = engine_api::EngineReleaseProtectedMaterial(denied_release);
  Require(!denied.ok && denied.policy_denied &&
              HasDiagnostic(denied, "SECURITY.PROTECTED_MATERIAL.POLICY_DENIED"),
          "DBLC-013Z protected material release policy denial failed");

  engine_api::EngineExportProtectedMaterialPackageRequest export_package;
  export_package.context = Context(database_path, kDatabaseA, 3800, 21);
  export_package.protected_material_uuid = std::string(kProtectedMaterialUuid);
  export_package.include_versions = true;
  export_package.include_audit = true;
  export_package.export_reason = "reference-package-transfer";
  const auto exported_package =
      engine_api::EngineExportProtectedMaterialPackage(export_package);
  Require(exported_package.ok && exported_package.exported &&
              exported_package.material_count == 1 &&
              exported_package.version_count >= 2 &&
              !exported_package.encoded_package.empty() &&
              !exported_package.package_digest.empty(),
          "DBLC-013Z protected material package export failed");
  Require(exported_package.protected_material_redacted &&
              !exported_package.plaintext_material_returned,
          "DBLC-013Z protected material package export redaction failed");
  Require(HasEvidence(exported_package, "protected_material_package_export"),
          "DBLC-013Z protected material package export evidence missing");
  RequireNoLeak(exported_package, "wrapped-material-b");

  const auto import_database_path =
      database_path.parent_path() / "dblc013z-import-target.sbdb";
  engine_api::EngineImportProtectedMaterialPackageRequest import_denied;
  import_denied.context = Context(import_database_path, kDatabaseB, 3900, 22);
  import_denied.encoded_package = exported_package.encoded_package;
  import_denied.expected_package_digest = exported_package.package_digest;
  const auto denied_import =
      engine_api::EngineImportProtectedMaterialPackage(import_denied);
  Require(!denied_import.ok &&
              HasDiagnostic(denied_import,
                            "SECURITY.PROTECTED_MATERIAL.PACKAGE_IMPORT_AUTHORITY_REQUIRED"),
          "DBLC-013Z protected material package import authority refusal failed");

  engine_api::EngineImportProtectedMaterialPackageRequest import_package = import_denied;
  import_package.import_authorized = true;
  import_package.import_reason = "authorized-reference-package-transfer";
  const auto imported_package =
      engine_api::EngineImportProtectedMaterialPackage(import_package);
  Require(imported_package.ok && imported_package.imported &&
              imported_package.material_count == 1 &&
              imported_package.version_count >= 2 &&
              imported_package.package_digest == exported_package.package_digest,
          "DBLC-013Z protected material package import failed");
  Require(imported_package.protected_material_redacted &&
              !imported_package.plaintext_material_returned,
          "DBLC-013Z protected material package import redaction failed");
  Require(HasEvidence(imported_package, "protected_material_package_import"),
          "DBLC-013Z protected material package import evidence missing");
  RequireNoLeak(imported_package, "wrapped-material-b");

  engine_api::EngineInspectProtectedMaterialCatalogRequest inspect_import;
  inspect_import.context = Context(import_database_path, kDatabaseB, 4000, 22);
  inspect_import.protected_material_uuid = std::string(kProtectedMaterialUuid);
  inspect_import.include_versions = true;
  inspect_import.include_audit = true;
  const auto imported_catalog =
      engine_api::EngineInspectProtectedMaterialCatalog(inspect_import);
  Require(imported_catalog.ok && imported_catalog.materials.size() == 1 &&
              imported_catalog.versions.size() >= 2 &&
              imported_catalog.materials.front().database_uuid == kDatabaseB,
          "DBLC-013Z imported protected material catalog visibility failed");
  RequireNoLeak(imported_catalog, "wrapped-material-b");

  engine_api::EnginePurgeProtectedMaterialVersionRequest purge_retained;
  purge_retained.context = Context(database_path, kDatabaseA, 4000, 30);
  purge_retained.protected_material_uuid = std::string(kProtectedMaterialUuid);
  purge_retained.protected_material_version_uuid = std::string(kProtectedMaterialVersionB);
  purge_retained.purge_reason = "retention-check";
  const auto retained = engine_api::EnginePurgeProtectedMaterialVersion(purge_retained);
  Require(!retained.ok && retained.refused_by_retention && retained.audit_preserved,
          "DBLC-013Z protected material retention refusal failed");
  Require(retained.protected_reference_reachable,
          "DBLC-013Z retained protected material lost reference");

  const auto physical_page_path =
      database_path.parent_path() / "dblc013z-protected-material-version-b.page";
  WriteProtectedMaterialPageFixture(physical_page_path);

  engine_api::EnginePurgeProtectedMaterialVersionRequest physical_denied = purge_retained;
  physical_denied.context = Context(database_path, kDatabaseA, 6000, 39);
  physical_denied.purge_reason = "physical-erase-authority-check";
  physical_denied.physical_erase_requested = true;
  physical_denied.physical_erase_path = physical_page_path.string();
  const auto denied_physical =
      engine_api::EnginePurgeProtectedMaterialVersion(physical_denied);
  Require(!denied_physical.ok && denied_physical.audit_preserved &&
              denied_physical.protected_reference_reachable &&
              HasDiagnostic(denied_physical,
                            "SECURITY.PROTECTED_MATERIAL.PHYSICAL_ERASE_AUTHORITY_REQUIRED"),
          "DBLC-013Z physical erase authority refusal failed");
  Require(!FileAllZero(physical_page_path),
          "DBLC-013Z physical erase authority refusal changed protected page bytes");

  engine_api::EnginePurgeProtectedMaterialVersionRequest purge_allowed = purge_retained;
  purge_allowed.context = Context(database_path, kDatabaseA, 6000, 40);
  purge_allowed.purge_reason = "retention-expired";
  purge_allowed.physical_erase_requested = true;
  purge_allowed.physical_erase_authorized = true;
  purge_allowed.physical_erase_retention_satisfied = true;
  purge_allowed.physical_erase_legal_hold_clear = true;
  purge_allowed.physical_erase_path = physical_page_path.string();
  const auto purged = engine_api::EnginePurgeProtectedMaterialVersion(purge_allowed);
  Require(purged.ok && purged.purged && purged.audit_preserved,
          "DBLC-013Z protected material purge after retention failed");
  Require(!purged.protected_reference_reachable,
          "DBLC-013Z purged protected material reference remained reachable");
  Require(purged.physical_erase_executed && purged.physical_erase_verified &&
              purged.physical_erase_bytes > 0,
          "DBLC-013Z physical cryptographic erase was not executed and verified");
  Require(FileAllZero(physical_page_path),
          "DBLC-013Z physical cryptographic erase did not clear protected page bytes");
  Require(HasEvidence(purged, "protected_material_physical_erase") &&
              HasEvidence(purged, "protected_material_physical_erase_verified"),
          "DBLC-013Z physical erase evidence missing");

  engine_api::EngineInspectProtectedMaterialCatalogRequest inspect;
  inspect.context = Context(database_path, kDatabaseA, 6100, 40);
  inspect.protected_material_uuid = std::string(kProtectedMaterialUuid);
  inspect.include_versions = true;
  inspect.include_audit = true;
  const auto inspected = engine_api::EngineInspectProtectedMaterialCatalog(inspect);
  Require(inspected.ok && inspected.protected_material_redacted,
          "DBLC-013Z protected material catalog inspect failed");
  Require(!inspected.materials.empty(), "DBLC-013Z protected material catalog material missing");
  Require(inspected.versions.size() >= 2,
          "DBLC-013Z protected material catalog versions missing");
  Require(inspected.audit_events.size() >= 7,
          "DBLC-013Z protected material audit history incomplete");
  RequireNoLeak(inspected, "wrapped-material-a");
  RequireNoLeak(inspected, "wrapped-material-b");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "dblc013z.sbdb";
  TestAdmissionInspectAndEncryptedOpen(database_path);
  TestScopeAndMissingAuthorityRefusals(database_path);
  TestRotationExpiryPurgeAndShutdown(database_path);
  TestPlaintextRefusalAndNoDiagnosticLeak(database_path);
  TestProtectedMaterialCatalogLifecycle(database_path);
  return EXIT_SUCCESS;
}
