#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/sb_test_temp_compat.hpp"
#include "../database_lifecycle/database_lifecycle_test_memory.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/protected_material_api.hpp"
#include "security/security_crypto_policy.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1771200330000ull;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

api::EngineUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  api::EngineUuid out;
  if (generated.ok()) { out = generated.value.value; }
  return out;
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
  std::ostringstream out;
  out << result.operation_id << '\n';
  for (const auto& diagnostic : result.diagnostics) {
    out << diagnostic.code << '\n' << diagnostic.detail << '\n';
  }
  for (const auto& evidence : result.evidence) {
    out << evidence.evidence_kind << '\n';
    if (const auto* text = std::get_if<std::string>(&evidence.evidence_id)) {
      out << *text;
    } else {
      const auto& id = std::get<api::EngineUuid>(evidence.evidence_id);
      out.write(reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size());
    }
    out << '\n';
  }
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      out << field.first << '=' << field.second.encoded_value << '\n';
      if (!field.second.binary_value.empty())
        out.write(reinterpret_cast<const char*>(field.second.binary_value.data()),
                  field.second.binary_value.size());
    }
  }
  return out.str();
}

std::vector<unsigned char> ReadBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "failed to open protected material catalog");
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteBytes(const std::filesystem::path& path,
                const std::vector<unsigned char>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "failed to open protected material catalog for write");
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  Require(static_cast<bool>(out), "failed to write protected material catalog");
}

std::string BytesAsText(const std::vector<unsigned char>& bytes) {
  return {bytes.begin(), bytes.end()};
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  api::EngineUuid database = MakeUuid(UuidKind::database, 2);
  api::EngineUuid principal;
  api::EngineRequestContext bootstrap_context;
  api::EngineUuid material = MakeUuid(UuidKind::object, 10);
  api::EngineUuid version_one = MakeUuid(UuidKind::object, 11);
  api::EngineUuid version_two = MakeUuid(UuidKind::object, 12);
  Fixture() {
    auto pattern = (std::filesystem::temp_directory_path() / "sbpmgate_XXXXXX").string();
    Require(mkdtemp(pattern.data()) != nullptr, "private protected-material workspace creation failed");
    root = pattern;
    database_path = root / "eler033.sbdb";
    scratchbird::storage::database::DatabaseCreateConfig create;
    create.path = database_path.string();
    create.database_uuid = {UuidKind::database, database};
    create.filespace_uuid = {UuidKind::filespace, MakeUuid(UuidKind::filespace, 5)};
    create.page_size = 16384;
    create.creation_unix_epoch_millis = kBaseMillis;
    scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
    Require(scratchbird::storage::database::CreateDatabaseFile(create).ok(),
            "protected-material database bootstrap failed");
    bootstrap_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
    principal = bootstrap_context.principal_uuid;
  }
};

api::EngineRequestContext Context(const Fixture& fixture,
                                  u64 tx,
                                  u64 visible_through,
                                  u64 resource_epoch = 1771200335000ull) {
  auto context = fixture.bootstrap_context;
  context.request_id = "eler033-protected-material-durable-catalog";
  context.local_transaction_id = tx;
  context.snapshot_visible_through_local_transaction_id = visible_through;
  context.resource_epoch = resource_epoch;
  return context;
}

api::EngineProtectedMaterialPolicySet Policy(u64 offset,
                                             u64 retention_until = 0) {
  api::EngineProtectedMaterialPolicySet policy;
  policy.retention_policy_uuid = MakeUuid(UuidKind::object, offset);
  policy.access_policy_uuid = MakeUuid(UuidKind::object, offset + 1);
  policy.release_policy_uuid = MakeUuid(UuidKind::object, offset + 2);
  policy.purge_policy_uuid = MakeUuid(UuidKind::object, offset + 3);
  policy.audit_policy_uuid = MakeUuid(UuidKind::object, offset + 4);
  policy.retention_until_epoch_millis = retention_until;
  policy.release_purposes.push_back("security_use");
  return policy;
}

std::filesystem::path CatalogPath(const Fixture& fixture) {
  return fixture.database_path.string() + ".sb.protected_material_catalog";
}

std::filesystem::path CatalogTempPath(const Fixture& fixture) {
  return CatalogPath(fixture).string() + ".tmp";
}

api::EngineCreateProtectedMaterialResult CreateMaterial(const Fixture& fixture) {
  api::EngineCreateProtectedMaterialRequest request;
  request.context = Context(fixture, 101, 101);
  request.protected_material_uuid = fixture.material;
  request.owner_scope_uuid = fixture.principal;
  request.purpose_class = "security_use";
  request.storage_class = "wrapped";
  request.policy = Policy(50);
  request.initial_version_uuid = fixture.version_one;
  request.protected_reference = "wrapped-ref:v1:eler033-initial";
  request.envelope_reference = "envelope-ref:v1:eler033-initial";
  request.payload_hash = "sha256:" + api::SecuritySha256Hex("eler033-initial");
  request.option_envelopes.push_back("key_authority:engine");
  const auto result = api::EngineCreateProtectedMaterial(request);
  Require(result.ok && result.created && result.initial_version_created,
          "protected material create did not persist initial version");
  Require(HasEvidenceKind(result, "protected_material_create"),
          "protected material create evidence missing");
  return result;
}

api::EngineAddProtectedMaterialVersionResult AddVersionTwo(const Fixture& fixture) {
  {
    std::ofstream stale(CatalogTempPath(fixture), std::ios::binary | std::ios::trunc);
    stale << "stale-temp-residue";
  }
  Require(std::filesystem::exists(CatalogTempPath(fixture)),
          "failed to create stale protected material catalog temp residue");

  api::EngineAddProtectedMaterialVersionRequest request;
  request.context = Context(fixture, 102, 102);
  request.protected_material_uuid = fixture.material;
  request.protected_material_version_uuid = fixture.version_two;
  request.protected_reference = "wrapped-ref:v1:eler033-rotated";
  request.envelope_reference = "envelope-ref:v1:eler033-rotated";
  request.payload_hash = "sha256:" + api::SecuritySha256Hex("eler033-rotated");
  request.storage_class = "wrapped";
  request.rotation_reason = "eler033 rotation proof";
  request.policy_override = Policy(60);
  request.option_envelopes.push_back("key_authority:engine");
  const auto result = api::EngineAddProtectedMaterialVersion(request);
  Require(result.ok && result.version_added && result.active_version_number == 2,
          "protected material version add did not persist version two");
  Require(!std::filesystem::exists(CatalogTempPath(fixture)),
          "stale protected material catalog temp residue survived mutation");
  return result;
}

void ProveDurableCatalogLifecycle(const Fixture& fixture) {
  CreateMaterial(fixture);
  const auto catalog_path = CatalogPath(fixture);
  Require(std::filesystem::exists(catalog_path),
          "protected material binary catalog was not created");
  Require(!std::filesystem::exists(CatalogTempPath(fixture)),
          "protected material catalog temp file survived initial create");

  AddVersionTwo(fixture);

  api::EngineReleaseProtectedMaterialRequest wrong_purpose;
  wrong_purpose.context = Context(fixture, 0, 102);
  wrong_purpose.protected_material_uuid = fixture.material;
  wrong_purpose.purpose = "wrong_purpose";
  wrong_purpose.option_envelopes.push_back("protected_material_authority:engine");
  const auto denied = api::EngineReleaseProtectedMaterial(wrong_purpose);
  Require(!denied.ok && denied.policy_denied &&
              HasDiagnosticCode(denied, "SECURITY.PROTECTED_MATERIAL.POLICY_DENIED"),
          "wrong protected material release purpose did not fail closed");

  api::EngineReleaseProtectedMaterialRequest release;
  release.context = Context(fixture, 0, 102);
  release.protected_material_uuid = fixture.material;
  release.purpose = "security_use";
  release.option_envelopes.push_back("protected_material_authority:engine");
  const auto released = api::EngineReleaseProtectedMaterial(release);
  Require(released.ok && released.released && !released.plaintext_material_returned,
          "protected material release failed or returned plaintext");
  Require(released.release_handle.rfind("protected-material-release:v1:hmac-sha256:", 0) == 0,
          "protected material release did not use HMAC-SHA-256 handle");

  api::EngineInspectProtectedMaterialCatalogRequest inspect;
  inspect.context = Context(fixture, 0, 102);
  inspect.protected_material_uuid = fixture.material;
  inspect.option_envelopes.push_back("protected_material_authority:engine");
  const auto inspected = api::EngineInspectProtectedMaterialCatalog(inspect);
  Require(inspected.ok && inspected.materials.size() == 1 &&
              inspected.versions.size() == 2 &&
              inspected.audit_events.size() >= 4,
          "protected material durable catalog inspect did not reload material versions and audit");
  for (const auto& version : inspected.versions) {
    Require(version.protected_reference == "<protected-material-redacted>",
            "protected material inspect returned an unredacted protected reference");
  }
  const std::string flattened = FlattenResult(inspected);
  Require(flattened.find("wrapped-ref:v1") == std::string::npos,
          "protected material inspect result leaked wrapped reference");

  const auto encoded = ReadBytes(catalog_path);
  Require(encoded.size() >= 80, "protected material binary catalog is too small");
  const std::string encoded_text = BytesAsText(encoded);
  Require(encoded_text.find("plaintext") == std::string::npos &&
              encoded_text.find("password=") == std::string::npos &&
              encoded_text.find("private_key") == std::string::npos &&
              encoded_text.find("CorrectHorse") == std::string::npos,
          "protected material binary catalog contains plaintext secret marker");

  auto tampered = encoded;
  tampered.back() ^= 0x5au;
  WriteBytes(catalog_path, tampered);
  const auto tamper_rejected = api::EngineInspectProtectedMaterialCatalog(inspect);
  Require(!tamper_rejected.ok &&
              HasDiagnosticCode(tamper_rejected,
                                "SECURITY.PROTECTED_MATERIAL.DURABLE_CATALOG_INVALID"),
          "tampered protected material catalog did not fail closed");

  WriteBytes(catalog_path, encoded);
  const auto restored = api::EngineInspectProtectedMaterialCatalog(inspect);
  Require(restored.ok && restored.versions.size() == 2 &&
              restored.audit_events.size() >= inspected.audit_events.size(),
          "restored protected material durable catalog did not reload");
}

void ProveCatalogPathIsRequired(const Fixture& fixture) {
  api::EngineInspectProtectedMaterialCatalogRequest inspect;
  inspect.context = Context(fixture, 0, 102);
  inspect.context.database_path.clear();
  inspect.protected_material_uuid = fixture.material;
  inspect.option_envelopes.push_back("protected_material_authority:engine");
  const auto result = api::EngineInspectProtectedMaterialCatalog(inspect);
  Require(!result.ok &&
              HasDiagnosticCode(result,
                                "SECURITY.PROTECTED_MATERIAL.DURABLE_CATALOG_INVALID"),
          "protected material catalog accepted process memory without database path");
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "protected-material-durable-catalog-conformance");
  const Fixture fixture;

  ProveDurableCatalogLifecycle(fixture);
  ProveCatalogPathIsRequired(fixture);

  std::filesystem::remove_all(fixture.root);
  std::cout << "engine_listener_protected_material_durable_catalog_conformance=passed\n";
  return EXIT_SUCCESS;
}
