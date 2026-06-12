// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "domain_support/domain_store.hpp"
#include "memory.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr u64 kBaseMillis = 1771200100000ull;
constexpr scratchbird::core::platform::u32 kPageSize = 16384;

struct DatabaseFixture {
  std::filesystem::path root;
  std::filesystem::path path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
};

struct CleanupDir {
  std::filesystem::path root;
  ~CleanupDir() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

std::string DiagnosticText(const api::EngineApiResult& result) {
  if (result.diagnostics.empty()) { return {}; }
  const auto& diagnostic = result.diagnostics.front();
  return diagnostic.code + ":" + diagnostic.message_key + ":" + diagnostic.detail;
}

std::string DiagnosticText(const api::DomainStoreResult& result) {
  return result.diagnostic.code + ":" + result.diagnostic.message_key + ":" +
         result.diagnostic.detail;
}

void RequireApiOk(const api::EngineApiResult& result, std::string_view message) {
  if (result.ok) { return; }
  std::cerr << message << ": " << DiagnosticText(result) << '\n';
  std::exit(EXIT_FAILURE);
}

void RequireDiagnosticContains(const api::EngineApiDiagnostic& diagnostic,
                               std::string_view needle,
                               std::string_view message) {
  const std::string text =
      diagnostic.code + ":" + diagnostic.message_key + ":" + diagnostic.detail;
  if (text.find(needle) != std::string::npos) { return; }
  std::cerr << message << ": " << text << '\n';
  std::exit(EXIT_FAILURE);
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, kBaseMillis + offset);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string UuidText(TypedUuid typed_uuid) {
  return uuid::UuidToString(typed_uuid.value);
}

std::string UuidText(UuidKind kind, u64 offset) {
  return UuidText(MakeUuid(kind, offset));
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "engine_listener_domain_binary_catalog_conformance";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "engine_listener_domain_binary_catalog_conformance");
  Require(configured.ok(), "memory fixture configuration failed");
  Require(configured.fixture_mode, "memory fixture mode was not active");
}

DatabaseFixture CreateDatabaseFixture(const std::filesystem::path& root) {
  DatabaseFixture fixture;
  fixture.root = root;
  fixture.path = root / "eler031_domain_catalog.sbdb";
  fixture.database_uuid = MakeUuid(UuidKind::database, 310);
  fixture.filespace_uuid = MakeUuid(UuidKind::filespace, 311);
  std::filesystem::create_directories(root);

  db::DatabaseCreateConfig create;
  create.path = fixture.path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.page_size = kPageSize;
  create.creation_unix_epoch_millis = kBaseMillis;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "database fixture creation failed");
  return fixture;
}

api::EngineRequestContext Context(const DatabaseFixture& fixture,
                                  std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = UuidText(fixture.database_uuid);
  context.node_uuid.canonical = UuidText(UuidKind::object, 312);
  context.principal_uuid.canonical = UuidText(UuidKind::principal, 313);
  context.session_uuid.canonical = UuidText(UuidKind::object, 314);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

void Begin(api::EngineRequestContext* context) {
  api::EngineBeginTransactionRequest request;
  request.context = *context;
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireApiOk(begun, "transaction begin failed");
  context->local_transaction_id = begun.local_transaction_id;
  context->transaction_uuid = begun.transaction_uuid;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;
}

void Commit(api::EngineRequestContext* context) {
  api::EngineCommitTransactionRequest request;
  request.context = *context;
  const auto committed = api::EngineCommitTransaction(request);
  RequireApiOk(committed, "transaction commit failed");
  context->local_transaction_id = 0;
  context->transaction_uuid.canonical.clear();
}

api::DomainRecord Domain(std::uint64_t creator_tx, std::string domain_uuid) {
  api::DomainRecord record;
  record.creator_tx = creator_tx;
  record.domain_uuid = std::move(domain_uuid);
  record.catalog_row_uuid = UuidText(UuidKind::object, 320 + creator_tx);
  record.schema_uuid = UuidText(UuidKind::schema, 330 + creator_tx);
  record.default_name = "eler031_customer_profile";
  record.base_descriptor_uuid = UuidText(UuidKind::object, 340 + creator_tx);
  record.base_descriptor_kind = "scalar";
  record.base_canonical_type_name = "character";
  record.base_encoded_descriptor = "canonical=character;length=4096";
  record.nullable = false;
  record.default_expression_envelope = "literal:path=name=Anonymous;ssn=0000";
  record.check_constraint_envelope = "sblr_predicate:not_empty";
  record.charset_or_collation_ref = "charset:utf8:collation:unicode_ci";
  record.numeric_metadata = "precision=0;scale=0";
  record.cast_policy_envelope = "require_right:DOMAIN_CAST";
  record.mutation_policy_envelope = "require_right:DOMAIN_USE";
  record.masking_policy_envelope = "path:ssn:last4|path:name:fixed:visible";
  record.visibility_policy_envelope = "require_security_context";
  record.encryption_policy_ref = "key_policy:customer_profile_key";
  record.driver_metadata_envelope = "driver:binary-domain-proof";
  record.wire_metadata_envelope = "wire:sbsql-domain-v1";
  record.element_path_envelope = "path:name;path:ssn";
  record.method_binding_envelope = "method:normalize_customer_profile";
  record.localized_names_envelope = "en:Customer Profile|fr:Profil Client";
  record.comment_envelope = "domain catalog binary envelope proof";
  record.reference_alias_envelope = "postgresql:domain:customer_profile";
  record.validation_hook_status = "sblr_builtin";
  return record;
}

std::filesystem::path BinaryCatalogPath(const DatabaseFixture& fixture) {
  return fixture.path.string() + ".sb.domain_catalog";
}

std::filesystem::path LegacyTextPath(const DatabaseFixture& fixture) {
  return fixture.path.string() + ".sb.domain_events";
}

std::vector<char> ReadAllBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "failed to open binary catalog");
  return std::vector<char>((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
}

void WriteAllBytes(const std::filesystem::path& path,
                   const std::vector<char>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "failed to open binary catalog for tamper write");
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  out.close();
  Require(static_cast<bool>(out), "failed to write tampered binary catalog");
}

void RequireSameDomainMetadata(const api::DomainRecord& expected,
                               const api::DomainRecord& actual) {
  Require(actual.creator_tx == expected.creator_tx, "domain creator tx did not round trip");
  Require(actual.domain_uuid == expected.domain_uuid, "domain uuid did not round trip");
  Require(actual.catalog_row_uuid == expected.catalog_row_uuid, "catalog row uuid did not round trip");
  Require(actual.schema_uuid == expected.schema_uuid, "schema uuid did not round trip");
  Require(actual.default_name == expected.default_name, "domain name did not round trip");
  Require(actual.base_descriptor_uuid == expected.base_descriptor_uuid, "base descriptor uuid did not round trip");
  Require(actual.base_descriptor_kind == expected.base_descriptor_kind, "base descriptor kind did not round trip");
  Require(actual.base_canonical_type_name == expected.base_canonical_type_name, "base type did not round trip");
  Require(actual.base_encoded_descriptor == expected.base_encoded_descriptor, "base descriptor did not round trip");
  Require(actual.nullable == expected.nullable, "nullable flag did not round trip");
  Require(actual.default_expression_envelope == expected.default_expression_envelope, "default envelope did not round trip");
  Require(actual.check_constraint_envelope == expected.check_constraint_envelope, "check envelope did not round trip");
  Require(actual.charset_or_collation_ref == expected.charset_or_collation_ref, "charset metadata did not round trip");
  Require(actual.numeric_metadata == expected.numeric_metadata, "numeric metadata did not round trip");
  Require(actual.cast_policy_envelope == expected.cast_policy_envelope, "cast policy did not round trip");
  Require(actual.mutation_policy_envelope == expected.mutation_policy_envelope, "mutation policy did not round trip");
  Require(actual.masking_policy_envelope == expected.masking_policy_envelope, "masking policy did not round trip");
  Require(actual.visibility_policy_envelope == expected.visibility_policy_envelope, "visibility policy did not round trip");
  Require(actual.encryption_policy_ref == expected.encryption_policy_ref, "encryption policy did not round trip");
  Require(actual.driver_metadata_envelope == expected.driver_metadata_envelope, "driver metadata did not round trip");
  Require(actual.wire_metadata_envelope == expected.wire_metadata_envelope, "wire metadata did not round trip");
  Require(actual.element_path_envelope == expected.element_path_envelope, "element path metadata did not round trip");
  Require(actual.method_binding_envelope == expected.method_binding_envelope, "method binding did not round trip");
  Require(actual.localized_names_envelope == expected.localized_names_envelope, "localized names did not round trip");
  Require(actual.comment_envelope == expected.comment_envelope, "comment metadata did not round trip");
  Require(actual.reference_alias_envelope == expected.reference_alias_envelope, "reference aliases did not round trip");
  Require(!actual.dropped, "created domain was loaded as dropped");
}

void DomainBinaryCatalogProof() {
  ConfigureMemoryFixture();
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  CleanupDir cleanup{std::filesystem::temp_directory_path() /
                     ("sb_eler031_" + std::to_string(unique))};
  const auto fixture = CreateDatabaseFixture(cleanup.root);

  auto writer = Context(fixture, "eler031-domain-writer");
  Begin(&writer);
  const auto domain_uuid = UuidText(UuidKind::object, 350);
  const auto domain = Domain(writer.local_transaction_id, domain_uuid);

  auto bad_domain = domain;
  bad_domain.creator_tx = writer.local_transaction_id + 10;
  const auto bad_append =
      api::AppendDomainEvent(writer, api::MakeDomainCreateEvent(bad_domain));
  Require(bad_append.error, "domain append accepted mismatched creator tx");
  RequireDiagnosticContains(bad_append,
                            "domain_creator_tx_mismatch",
                            "creator transaction mismatch diagnostic drifted");

  const auto appended =
      api::AppendDomainEvent(writer, api::MakeDomainCreateEvent(domain));
  Require(!appended.error, "domain create event append failed");
  Require(std::filesystem::exists(BinaryCatalogPath(fixture)),
          "binary domain catalog was not created");
  Require(!std::filesystem::exists(LegacyTextPath(fixture)),
          "new domain append wrote legacy text event sidecar");

  const auto same_tx_visible =
      api::FindVisibleDomain(writer, domain_uuid, writer.local_transaction_id);
  Require(same_tx_visible.has_value(), "active transaction could not see its domain");

  auto reader_before_commit = Context(fixture, "eler031-reader-before-commit");
  Begin(&reader_before_commit);
  const auto other_tx_visible = api::FindVisibleDomain(
      reader_before_commit, domain_uuid, reader_before_commit.local_transaction_id);
  Require(!other_tx_visible.has_value(), "uncommitted domain was visible to another transaction");
  Commit(&reader_before_commit);

  std::ofstream stale_temp(BinaryCatalogPath(fixture).string() + ".tmp",
                           std::ios::binary | std::ios::trunc);
  stale_temp << "partial-crash-record";
  stale_temp.close();
  Require(static_cast<bool>(stale_temp), "failed to create stale catalog temp file");
  const auto stale_temp_load = api::LoadDomainState(writer);
  Require(stale_temp_load.ok, "stale temp file blocked committed catalog load");
  Require(stale_temp_load.domains.size() == 1, "stale temp file changed catalog contents");

  Commit(&writer);

  auto reader = Context(fixture, "eler031-domain-reader");
  Begin(&reader);
  const auto loaded = api::LoadDomainState(reader);
  Require(loaded.ok, DiagnosticText(loaded));
  Require(loaded.domains.size() == 1, "binary domain catalog did not load exactly one record");
  RequireSameDomainMetadata(domain, loaded.domains.front());

  const auto committed_visible =
      api::FindVisibleDomain(reader, domain_uuid, reader.local_transaction_id);
  Require(committed_visible.has_value(), "committed domain was not visible after reload");
  RequireSameDomainMetadata(domain, *committed_visible);
  Commit(&reader);

  auto encoded = ReadAllBytes(BinaryCatalogPath(fixture));
  Require(encoded.size() > 128, "binary domain catalog too small to prove payload");
  Require(std::string(encoded.data(), encoded.data() + 8) == "SBDOMC01",
          "binary domain catalog magic mismatch");
  encoded[encoded.size() - 1] = static_cast<char>(encoded.back() ^ 0x01);
  WriteAllBytes(BinaryCatalogPath(fixture), encoded);

  const auto tampered = api::LoadDomainState(Context(fixture, "eler031-tamper-reader"));
  Require(!tampered.ok, "tampered domain catalog did not fail closed");
  Require(DiagnosticText(tampered).find("digest_mismatch") != std::string::npos,
          "tampered domain catalog diagnostic did not identify digest mismatch");
}

}  // namespace

int main() {
  DomainBinaryCatalogProof();
  std::cout << "engine_listener_domain_binary_catalog_conformance=passed\n";
  return EXIT_SUCCESS;
}
