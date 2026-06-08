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
                                         std::uint64_t epoch = 1000) {
  engine_api::EngineRequestContext context;
  context.trust_mode = engine_api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid.canonical = std::string(database_uuid);
  context.security_context_present = true;
  context.trace_tags.push_back("security.bootstrap");
  context.resource_epoch = epoch;
  context.local_transaction_id = 1;
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

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "dblc013z.sbdb";
  TestAdmissionInspectAndEncryptedOpen(database_path);
  TestScopeAndMissingAuthorityRefusals(database_path);
  TestRotationExpiryPurgeAndShutdown(database_path);
  TestPlaintextRefusalAndNoDiagnosticLeak(database_path);
  return EXIT_SUCCESS;
}
