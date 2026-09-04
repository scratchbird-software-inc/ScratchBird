// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "core/hash/hash_digest.hpp"
#include "core/uuid/uuid.hpp"
#include "database_lifecycle.hpp"
#include "engine/internal_api/dml/bulk_import_stream_execution_api.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_store.hpp"
#include "engine/internal_api/sblr_bulk_import_stream_coordinator.hpp"
#include "engine/sblr/sblr_bulk_import_stream_runtime.hpp"
#include "transaction/transaction_api.hpp"
#include "wire/parser_server_ipc/sbps_bulk_import_stream_codec.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
namespace wire = scratchbird::wire::sbps_bulk_import;

using BulkSha = sblr::BulkImportSha;
using BulkUuid = sblr::BulkImportUuid;

constexpr std::uint64_t kEpochMillis = 1949000000000ull;
constexpr std::string_view kConverterUuid =
    "019d0000-0000-7000-8000-00000000b775";

[[noreturn]] void Fail(std::string_view detail) {
  std::cerr << "bulk_import_stream_execution: " << detail << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool value, std::string_view detail) {
  if (!value) Fail(detail);
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  static std::atomic<std::uint64_t> ordinal{1};
  const auto timestamp = kEpochMillis + ordinal.fetch_add(1);
  const auto generated = [&]() {
    if (kind != platform::UuidKind::session) {
      return uuid::GenerateEngineIdentityV7(kind, timestamp);
    }
    const auto raw = uuid::GenerateCompatibilityUnixTimeV7(timestamp);
    if (!raw.ok()) {
      uuid::TypedUuidResult failed;
      failed.status = raw.status;
      failed.diagnostic = raw.diagnostic;
      return failed;
    }
    return uuid::MakeTypedUuid(kind, raw.value);
  }();
  Require(generated.ok(), "engine UUID generation failed");
  return generated.value;
}

std::string Text(const platform::TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

BulkUuid Bytes(std::string_view text) {
  const auto parsed = uuid::ParseUuid(std::string(text));
  Require(parsed.ok(), "canonical UUID parse failed");
  return parsed.value.bytes;
}

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value));
  out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    out->push_back(static_cast<std::uint8_t>(value >> (index * 8)));
  }
}

void AppendU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    out->push_back(static_cast<std::uint8_t>(value >> (index * 8)));
  }
}

void AppendUuid(std::vector<std::uint8_t>* out, std::string_view text) {
  const auto value = Bytes(text);
  out->insert(out->end(), value.begin(), value.end());
}

void AppendUuid(std::vector<std::uint8_t>* out, const BulkUuid& value) {
  out->insert(out->end(), value.begin(), value.end());
}

void AppendLp16(std::vector<std::uint8_t>* out, std::string_view text) {
  Require(text.size() <= std::numeric_limits<std::uint16_t>::max(),
          "LP16 fixture value overflow");
  AppendU16(out, static_cast<std::uint16_t>(text.size()));
  out->insert(out->end(), text.begin(), text.end());
}

BulkSha Hash(std::span<const std::uint8_t> bytes) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      bytes.data(), bytes.size());
  Require(digest.ok(), "SHA-256 failed");
  return digest.digest;
}

BulkSha Hash(std::string_view text) {
  return Hash(std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string Hex(std::span<const std::uint8_t> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2);
  for (const auto value : bytes) {
    encoded.push_back(digits[value >> 4]);
    encoded.push_back(digits[value & 0x0f]);
  }
  return encoded;
}

bool DecodeHex(std::string_view encoded, std::vector<std::uint8_t>* bytes) {
  if (bytes == nullptr || encoded.empty() || encoded.size() % 2 != 0) {
    return false;
  }
  const auto nibble = [](char value) -> int {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return 10 + value - 'a';
    if (value >= 'A' && value <= 'F') return 10 + value - 'A';
    return -1;
  };
  bytes->clear();
  bytes->reserve(encoded.size() / 2);
  for (std::size_t index = 0; index < encoded.size(); index += 2) {
    const auto high = nibble(encoded[index]);
    const auto low = nibble(encoded[index + 1]);
    if (high < 0 || low < 0) {
      bytes->clear();
      return false;
    }
    bytes->push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return true;
}

BulkSha EmptySetHash(std::string_view domain) {
  std::vector<std::uint8_t> material(domain.begin(), domain.end());
  AppendU32(&material, 0);
  return Hash(material);
}

BulkSha ColumnDigest(const api::MgaRelationStorageDescriptor& descriptor) {
  std::vector<const api::MgaRelationColumnStorageDescriptor*> columns;
  for (const auto& column : descriptor.columns) columns.push_back(&column);
  std::sort(columns.begin(), columns.end(), [](const auto* left,
                                               const auto* right) {
    return left->ordinal < right->ordinal;
  });
  std::vector<std::uint8_t> material;
  constexpr std::string_view domain =
      "ScratchBird.BulkImportStreamColumnDescriptorSet.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  AppendUuid(&material, descriptor.relation_uuid.canonical);
  AppendU64(&material, descriptor.relation_generation);
  AppendUuid(&material, descriptor.descriptor_uuid.canonical);
  AppendU64(&material, descriptor.descriptor_generation);
  AppendU32(&material, static_cast<std::uint32_t>(columns.size()));
  for (const auto* column : columns) {
    AppendU32(&material, column->ordinal);
    AppendUuid(&material, column->column_uuid.canonical);
    AppendU64(&material, column->column_generation);
    AppendLp16(&material, column->canonical_name_key);
    AppendUuid(&material,
               column->value_descriptor.descriptor_uuid.canonical);
    AppendLp16(&material, column->value_descriptor.descriptor_kind);
    AppendLp16(&material, column->value_descriptor.canonical_type_name);
    const std::vector<std::uint8_t> encoded(
        column->value_descriptor.encoded_descriptor.begin(),
        column->value_descriptor.encoded_descriptor.end());
    const auto encoded_hash = Hash(encoded);
    material.insert(material.end(), encoded_hash.begin(), encoded_hash.end());
    material.push_back(column->nullable ? 1 : 0);
    material.push_back(column->generated ? 1 : 0);
    material.push_back(column->identity_column ? 1 : 0);
    AppendLp16(&material, column->storage_class);
    AppendLp16(&material, column->charset_uuid);
    AppendLp16(&material, column->collation_uuid);
    AppendU32(&material, column->character_length);
    AppendU64(&material, column->max_inline_bytes);
    AppendLp16(&material, column->overflow_policy);
  }
  return Hash(material);
}

BulkSha ConverterEvidence(
    const api::MgaRelationColumnStorageDescriptor& column) {
  std::vector<std::uint8_t> material;
  constexpr std::string_view domain =
      "ScratchBird.BulkImportStreamTextConverter.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  AppendUuid(&material, kConverterUuid);
  AppendU64(&material, 1);
  AppendUuid(&material, column.column_uuid.canonical);
  AppendU64(&material, column.column_generation);
  AppendUuid(&material, column.value_descriptor.descriptor_uuid.canonical);
  AppendLp16(&material, column.value_descriptor.canonical_type_name);
  const std::vector<std::uint8_t> encoded(
      column.value_descriptor.encoded_descriptor.begin(),
      column.value_descriptor.encoded_descriptor.end());
  const auto encoded_hash = Hash(encoded);
  material.insert(material.end(), encoded_hash.begin(), encoded_hash.end());
  return Hash(material);
}

BulkSha PolicyDigest(const api::EngineRequestContext& context,
                     const api::MgaRelationStorageDescriptor& descriptor,
                     const BulkSha& syntax_demand,
                     const BulkSha& columns_digest) {
  std::vector<const api::MgaRelationColumnStorageDescriptor*> columns;
  for (const auto& column : descriptor.columns) columns.push_back(&column);
  std::sort(columns.begin(), columns.end(), [](const auto* left,
                                               const auto* right) {
    return left->ordinal < right->ordinal;
  });
  std::vector<std::uint8_t> material;
  constexpr std::string_view domain =
      "ScratchBird.BulkImportStreamPolicyBundle.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  material.insert(material.end(), syntax_demand.begin(), syntax_demand.end());
  AppendUuid(&material, descriptor.relation_uuid.canonical);
  AppendU64(&material, descriptor.relation_generation);
  AppendUuid(&material, descriptor.descriptor_uuid.canonical);
  AppendU64(&material, descriptor.descriptor_generation);
  material.insert(material.end(), columns_digest.begin(), columns_digest.end());
  AppendUuid(&material, context.catalog_epoch_uuid.canonical);
  AppendU64(&material, context.catalog_generation_id);
  AppendUuid(&material, context.authorization_context.authority_uuid.canonical);
  AppendU64(&material, context.authorization_context.security_epoch);
  AppendUuid(&material, context.resource_admission_uuid.canonical);
  AppendU64(&material, context.resource_epoch);
  for (const auto empty_domain : {
           std::string_view("ScratchBird.BulkImportStreamDefaultDescriptorSet.V1"),
           std::string_view("ScratchBird.BulkImportStreamConstraintSet.V1"),
           std::string_view("ScratchBird.BulkImportStreamTriggerSet.V1"),
           std::string_view("ScratchBird.BulkImportStreamIndexSet.V1")}) {
    const auto empty_hash = EmptySetHash(empty_domain);
    material.insert(material.end(), empty_hash.begin(), empty_hash.end());
  }
  AppendU32(&material, static_cast<std::uint32_t>(columns.size()));
  for (const auto* column : columns) {
    AppendUuid(&material, column->column_uuid.canonical);
    AppendU64(&material, column->column_generation);
    AppendUuid(&material,
               column->value_descriptor.descriptor_uuid.canonical);
    AppendUuid(&material, kConverterUuid);
    AppendU64(&material, 1);
    const auto converter = ConverterEvidence(*column);
    material.insert(material.end(), converter.begin(), converter.end());
  }
  return Hash(material);
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  std::filesystem::path stream_root;
  platform::TypedUuid database = NewUuid(platform::UuidKind::database);
  platform::TypedUuid filespace = NewUuid(platform::UuidKind::filespace);
  platform::TypedUuid schema = NewUuid(platform::UuidKind::schema);
  platform::TypedUuid relation = NewUuid(platform::UuidKind::object);
  platform::TypedUuid principal = NewUuid(platform::UuidKind::principal);
  platform::TypedUuid session = NewUuid(platform::UuidKind::session);
  api::EngineRequestContext context;
  api::MgaRelationStorageDescriptor descriptor;

  Fixture() = default;
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  Fixture(Fixture&&) = default;
  Fixture& operator=(Fixture&&) = default;

  ~Fixture() {
    std::error_code ignored;
    if (!root.empty()) std::filesystem::remove_all(root, ignored);
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = Text(fixture.database);
  context.database_page_size_bytes = 16384;
  context.default_root_uuid.canonical = Text(fixture.filespace);
  context.current_schema_uuid.canonical = Text(fixture.schema);
  context.principal_uuid.canonical = Text(fixture.principal);
  context.session_uuid.canonical = Text(fixture.session);
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(request);
  Require(begun.ok, "transaction begin failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  Require(api::EngineCommitTransaction(request).ok, "transaction commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  Require(api::EngineRollbackTransaction(request).ok,
          "transaction rollback failed");
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.root = std::filesystem::temp_directory_path() /
      ("scratchbird_bulk_import_execution_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()));
  std::filesystem::create_directories(fixture.root);
  fixture.database_path = fixture.root / "execution.sbdb";
  fixture.stream_root = fixture.root / "streams";
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = fixture.database;
  create.filespace_uuid = fixture.filespace;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = kEpochMillis;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  Require(db::CreateDatabaseFile(create).ok(), "database creation failed");

  auto metadata = Begin(fixture, "bulk-import-execution-metadata");
  api::CrudTableRecord table;
  table.creator_tx = metadata.local_transaction_id;
  table.table_uuid = Text(fixture.relation);
  table.default_name = "bulk_import_execution_target";
  table.columns = {
      {"id",
       "type=int32;datatype_descriptor_uuid="
       "019d0000-0000-7000-8000-00000000d716;type_uuid="
       "019d0000-0000-7000-8000-00000000d717;nullable=false"},
      {"payload",
       "type=character;datatype_descriptor_uuid="
       "019d0000-0000-7000-8000-00000000d718;type_uuid="
       "019d0000-0000-7000-8000-00000000d719;codec_uuid="
       "019d0000-0000-7000-8000-00000000d71a;nullable=true"},
  };
  Require(!api::AppendMgaTableMetadata(metadata, table).error,
          "table metadata append failed");
  Require(!api::EnsureMgaRelationStorageDescriptor(
               metadata, table, {}, &fixture.descriptor)
               .error,
          "relation descriptor creation failed");
  Commit(metadata);

  fixture.context = Begin(fixture, "bulk-import-execution");
  fixture.context.statement_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object));
  fixture.context.statement_receipt_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object));
  api::EnginePublishStatementSnapshotRequest publish_snapshot;
  publish_snapshot.context = fixture.context;
  const auto published_snapshot =
      api::EnginePublishStatementSnapshot(publish_snapshot);
  Require(published_snapshot.ok, "statement snapshot publication failed");
  fixture.context.statement_snapshot_uuid =
      published_snapshot.statement_snapshot_uuid;
  fixture.context.snapshot_visible_through_local_transaction_id =
      published_snapshot.snapshot_vector.visible_committed_high_watermark;
  fixture.context.catalog_epoch_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object));
  fixture.context.resource_admission_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object));
  fixture.context.authorization_context.present = true;
  fixture.context.authorization_context.authority_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object));
  fixture.context.authorization_context.security_context_generation = 1;
  fixture.context.authorization_context.principal_uuid =
      fixture.context.principal_uuid;
  fixture.context.authorization_context.security_epoch = 1;
  fixture.context.authorization_context.policy_epoch = 1;
  fixture.context.authorization_context.catalog_generation_id = 1;
  fixture.context.statement_metadata_snapshot_engine_owned = true;
  fixture.context.trace_tags.push_back("private_bulk_import_stream_compiler");
  const auto loaded = api::LoadMgaRelationStorageDescriptor(
      fixture.context, Text(fixture.relation));
  Require(loaded.ok, "live relation descriptor load failed");
  fixture.descriptor = loaded.descriptor;
  Require(fixture.descriptor.columns.size() == 2,
          "fixture relation descriptor column count drifted");
  return fixture;
}

api::SblrBulkImportStreamAuthorityInputV1 Authority(
    const Fixture& fixture, std::uint64_t structural,
    std::uint32_t occurrence) {
  api::SblrBulkImportStreamAuthorityInputV1 authority;
  authority.authenticated_receipt_uuid =
      Bytes(fixture.context.statement_receipt_uuid.canonical);
  authority.admitted_command_surface_id = "SBSQL-465931ED7427";
  authority.binding_uuid = Bytes(Text(NewUuid(platform::UuidKind::object)));
  authority.binding_generation = 1;
  authority.structural_occurrence = structural;
  authority.import_occurrence = occurrence;
  authority.syntax_demand_sha256 =
      Hash("bulk-import-syntax-" + std::to_string(structural));
  authority.binding_evidence_sha256 =
      Hash("bulk-import-binding-" + std::to_string(structural));
  authority.target_relation_uuid = Bytes(fixture.descriptor.relation_uuid.canonical);
  authority.target_relation_generation = fixture.descriptor.relation_generation;
  authority.owning_transaction_uuid =
      Bytes(fixture.context.transaction_uuid.canonical);
  authority.owning_local_transaction_id = fixture.context.local_transaction_id;
  authority.statement_snapshot_uuid =
      Bytes(fixture.context.statement_snapshot_uuid.canonical);
  authority.catalog_epoch_uuid = Bytes(fixture.context.catalog_epoch_uuid.canonical);
  authority.catalog_generation = fixture.context.catalog_generation_id;
  authority.security_context_uuid =
      Bytes(fixture.context.authorization_context.authority_uuid.canonical);
  authority.security_epoch = fixture.context.authorization_context.security_epoch;
  authority.policy_snapshot_uuid =
      Bytes(Text(NewUuid(platform::UuidKind::object)));
  authority.policy_generation = 1;
  authority.route_snapshot_uuid =
      Bytes(Text(NewUuid(platform::UuidKind::object)));
  authority.route_generation = 1;
  authority.row_shape_uuid = Bytes(fixture.descriptor.descriptor_uuid.canonical);
  authority.row_shape_generation = fixture.descriptor.descriptor_generation;
  authority.column_descriptor_set_sha256 = ColumnDigest(fixture.descriptor);
  authority.import_policy_bundle_sha256 = PolicyDigest(
      fixture.context, fixture.descriptor, authority.syntax_demand_sha256,
      authority.column_descriptor_set_sha256);
  authority.resource_grant_uuid =
      Bytes(fixture.context.resource_admission_uuid.canonical);
  authority.resource_grant_generation = fixture.context.resource_epoch;
  authority.executor_availability_generation = 1;
  authority.effective_maximum_stream_bytes = 1U << 20U;
  authority.effective_maximum_chunk_count = 16;
  authority.effective_maximum_chunk_bytes = 1U << 16U;
  authority.effective_maximum_rows = 64;
  authority.effective_maximum_target_columns = 2;
  return authority;
}

BulkSha ContentHash(std::span<const std::uint8_t> payload) {
  std::vector<std::uint8_t> material;
  constexpr std::string_view domain =
      "ScratchBird.BulkImportStreamContent.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  AppendU64(&material, payload.size());
  material.insert(material.end(), payload.begin(), payload.end());
  return Hash(material);
}

struct PreparedStream {
  api::BulkImportStreamAllocation allocation;
  std::vector<std::uint8_t> biro;
  api::BulkImportChunk chunk;
  api::BulkImportSeal seal;
};

PreparedStream CoordinateStream(Fixture& fixture,
                                api::SblrBulkImportStreamRegistry& registry,
                                std::uint64_t structural,
                                std::string_view csv) {
  const auto coordinated = api::CoordinateDurableSblrBulkImportStreamDescriptorV1(
      fixture.context, registry, Authority(fixture, structural, 1));
  Require(coordinated.ok && !coordinated.replayed,
          "durable descriptor coordination failed");
  const auto biro =
      sblr::EncodeSblrBulkImportStreamDescriptorV1(coordinated.descriptor, true);
  Require(biro.size() == sblr::BulkImportWireLayout::descriptor_size,
          "canonical BIRO encoding failed");
  const std::vector<std::uint8_t> payload(csv.begin(), csv.end());
  api::BulkImportChunk chunk;
  chunk.authenticated_receipt_uuid =
      coordinated.allocation.authenticated_receipt_uuid;
  chunk.stream_uuid = coordinated.allocation.stream_uuid;
  chunk.stream_generation = coordinated.allocation.stream_generation;
  chunk.structural_occurrence = coordinated.allocation.structural_occurrence;
  chunk.import_occurrence = coordinated.allocation.import_occurrence;
  chunk.descriptor_evidence = coordinated.allocation.descriptor_evidence;
  chunk.sequence = 1;
  chunk.byte_offset = 0;
  chunk.previous_chain_sha = wire::ChainStart(
      chunk.stream_uuid, chunk.stream_generation, chunk.descriptor_evidence);
  chunk.payload = payload;
  chunk.payload_sha = wire::PayloadSha256(chunk.payload);
  chunk.chain_sha = wire::ChainStep(
      chunk.previous_chain_sha, chunk.sequence, chunk.byte_offset,
      chunk.payload_sha, static_cast<std::uint32_t>(chunk.payload.size()));
  api::BulkImportSeal seal;
  seal.authenticated_receipt_uuid =
      coordinated.allocation.authenticated_receipt_uuid;
  seal.stream_uuid = coordinated.allocation.stream_uuid;
  seal.stream_generation = coordinated.allocation.stream_generation;
  seal.descriptor_evidence = coordinated.allocation.descriptor_evidence;
  seal.final_chunk_count = 1;
  seal.total_stream_bytes = payload.size();
  seal.final_chain_sha = chunk.chain_sha;
  seal.content_sha = ContentHash(payload);
  wire::Seal wire_seal;
  wire_seal.authenticated_receipt_uuid = seal.authenticated_receipt_uuid;
  wire_seal.stream_uuid = seal.stream_uuid;
  wire_seal.stream_generation = seal.stream_generation;
  wire_seal.descriptor_evidence = seal.descriptor_evidence;
  wire_seal.final_chunk_count = seal.final_chunk_count;
  wire_seal.total_stream_bytes = seal.total_stream_bytes;
  wire_seal.final_chain_sha256 = seal.final_chain_sha;
  wire_seal.content_sha256 = seal.content_sha;
  seal.seal_request_evidence = wire::SealRequestEvidence(wire_seal);
  return {coordinated.allocation, biro, std::move(chunk), std::move(seal)};
}

void AppendPreparedStream(api::SblrBulkImportStreamRegistry& registry,
                          const PreparedStream& stream) {
  const auto appended = registry.Append(stream.chunk);
  Require(appended.ok && appended.state == api::BulkImportStreamState::receiving,
          "durable chunk append failed");
}

void SealPreparedStream(api::SblrBulkImportStreamRegistry& registry,
                        const PreparedStream& stream) {
  const auto sealed = registry.Seal(stream.seal);
  Require(sealed.ok && sealed.state == api::BulkImportStreamState::sealed,
          "durable stream seal failed");
}

PreparedStream CoordinateAndSeal(Fixture& fixture,
                                 api::SblrBulkImportStreamRegistry& registry,
                                 std::uint64_t structural,
                                 std::string_view csv) {
  auto stream = CoordinateStream(fixture, registry, structural, csv);
  AppendPreparedStream(registry, stream);
  SealPreparedStream(registry, stream);
  return stream;
}

std::vector<std::uint8_t> EncodeChunkForWorker(
    const api::BulkImportChunk& chunk) {
  wire::Chunk encoded_chunk;
  encoded_chunk.authenticated_receipt_uuid =
      chunk.authenticated_receipt_uuid;
  encoded_chunk.stream_uuid = chunk.stream_uuid;
  encoded_chunk.stream_generation = chunk.stream_generation;
  encoded_chunk.structural_occurrence = chunk.structural_occurrence;
  encoded_chunk.import_occurrence = chunk.import_occurrence;
  encoded_chunk.descriptor_evidence = chunk.descriptor_evidence;
  encoded_chunk.chunk_sequence = chunk.sequence;
  encoded_chunk.byte_offset = chunk.byte_offset;
  encoded_chunk.previous_chain_sha256 = chunk.previous_chain_sha;
  encoded_chunk.payload_sha256 = chunk.payload_sha;
  encoded_chunk.chunk_payload = chunk.payload;
  encoded_chunk.chunk_chain_sha256 = chunk.chain_sha;
  std::vector<std::uint8_t> encoded;
  Require(wire::EncodeChunk(encoded_chunk, &encoded),
          "crash-worker chunk encoding failed");
  return encoded;
}

std::vector<std::uint8_t> EncodeSealForWorker(
    const api::BulkImportSeal& seal) {
  wire::Seal encoded_seal;
  encoded_seal.authenticated_receipt_uuid =
      seal.authenticated_receipt_uuid;
  encoded_seal.stream_uuid = seal.stream_uuid;
  encoded_seal.stream_generation = seal.stream_generation;
  encoded_seal.descriptor_evidence = seal.descriptor_evidence;
  encoded_seal.final_chunk_count = seal.final_chunk_count;
  encoded_seal.total_stream_bytes = seal.total_stream_bytes;
  encoded_seal.final_chain_sha256 = seal.final_chain_sha;
  encoded_seal.content_sha256 = seal.content_sha;
  encoded_seal.seal_request_evidence_sha256 = seal.seal_request_evidence;
  std::vector<std::uint8_t> encoded;
  Require(wire::EncodeSeal(encoded_seal, &encoded),
          "crash-worker seal encoding failed");
  return encoded;
}

void SpawnWorker(std::vector<std::string> arguments, int expected_code,
                 std::string_view detail) {
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (auto& argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);
  pid_t child = -1;
  const auto spawned = ::posix_spawn(
      &child, "/proc/self/exe", nullptr, nullptr, argv.data(), environ);
  Require(spawned == 0 && child > 0, "crash worker spawn failed");
  int status = 0;
  Require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == expected_code,
          detail);
}

void SpawnAppendWorker(const Fixture& fixture, const PreparedStream& stream,
                       int exit_code, std::string_view detail) {
  const auto encoded = EncodeChunkForWorker(stream.chunk);
  SpawnWorker({"bulk_import_stream_crash_worker", "--crash-append",
               fixture.stream_root.string(), std::to_string(exit_code),
               Hex(encoded)},
              exit_code, detail);
}

void SpawnSealWorker(const Fixture& fixture, const PreparedStream& stream,
                     int exit_code, std::string_view detail) {
  const auto encoded = EncodeSealForWorker(stream.seal);
  SpawnWorker({"bulk_import_stream_crash_worker", "--crash-seal",
               fixture.stream_root.string(), std::to_string(exit_code),
               Hex(encoded)},
              exit_code, detail);
}

void SpawnExecutionWorker(
    const Fixture& fixture, const PreparedStream& stream,
    api::BulkImportStreamExecutionCheckpointV1 checkpoint, int exit_code,
    std::string_view detail) {
  const auto& context = fixture.context;
  SpawnWorker(
      {"bulk_import_stream_crash_worker",
       "--crash-execute",
       fixture.stream_root.string(),
       context.database_path,
       context.database_uuid.canonical,
       context.default_root_uuid.canonical,
       context.current_schema_uuid.canonical,
       context.principal_uuid.canonical,
       context.session_uuid.canonical,
       context.request_id,
       std::to_string(context.local_transaction_id),
       context.transaction_uuid.canonical,
       std::to_string(
           context.snapshot_visible_through_local_transaction_id),
       context.transaction_isolation_level,
       context.statement_uuid.canonical,
       context.statement_receipt_uuid.canonical,
       context.statement_snapshot_uuid.canonical,
       context.catalog_epoch_uuid.canonical,
       context.resource_admission_uuid.canonical,
       context.authorization_context.authority_uuid.canonical,
       std::to_string(context.catalog_generation_id),
       std::to_string(context.authorization_context.security_epoch),
       std::to_string(context.resource_epoch),
       std::to_string(context.name_resolution_epoch),
       std::to_string(static_cast<unsigned>(checkpoint)),
       std::to_string(exit_code),
       Hex(stream.biro)},
      exit_code, detail);
}

int RunCrashWorker(int argc, char** argv) {
  try {
    if (argc == 5 && std::string_view(argv[1]) == "--crash-append") {
      std::vector<std::uint8_t> encoded;
      wire::Chunk decoded;
      if (!DecodeHex(argv[4], &encoded) ||
          !wire::DecodeChunk(encoded.data(), encoded.size(), &decoded)) {
        return 99;
      }
      api::BulkImportChunk chunk;
      chunk.authenticated_receipt_uuid = decoded.authenticated_receipt_uuid;
      chunk.stream_uuid = decoded.stream_uuid;
      chunk.stream_generation = decoded.stream_generation;
      chunk.structural_occurrence = decoded.structural_occurrence;
      chunk.import_occurrence = decoded.import_occurrence;
      chunk.descriptor_evidence = decoded.descriptor_evidence;
      chunk.sequence = decoded.chunk_sequence;
      chunk.byte_offset = decoded.byte_offset;
      chunk.previous_chain_sha = decoded.previous_chain_sha256;
      chunk.payload_sha = decoded.payload_sha256;
      chunk.payload = std::move(decoded.chunk_payload);
      chunk.chain_sha = decoded.chunk_chain_sha256;
      api::SblrBulkImportStreamRegistry registry(argv[2]);
      const auto result = registry.Append(chunk);
      if (!registry.healthy() || !result.ok) return 98;
      ::_exit(static_cast<int>(std::stoul(argv[3])));
    }
    if (argc == 5 && std::string_view(argv[1]) == "--crash-seal") {
      std::vector<std::uint8_t> encoded;
      wire::Seal decoded;
      if (!DecodeHex(argv[4], &encoded) ||
          !wire::DecodeSeal(encoded.data(), encoded.size(), &decoded)) {
        return 99;
      }
      api::BulkImportSeal seal;
      seal.authenticated_receipt_uuid = decoded.authenticated_receipt_uuid;
      seal.stream_uuid = decoded.stream_uuid;
      seal.stream_generation = decoded.stream_generation;
      seal.descriptor_evidence = decoded.descriptor_evidence;
      seal.final_chunk_count = decoded.final_chunk_count;
      seal.total_stream_bytes = decoded.total_stream_bytes;
      seal.final_chain_sha = decoded.final_chain_sha256;
      seal.content_sha = decoded.content_sha256;
      seal.seal_request_evidence = decoded.seal_request_evidence_sha256;
      api::SblrBulkImportStreamRegistry registry(argv[2]);
      const auto result = registry.Seal(seal);
      if (!registry.healthy() || !result.ok) return 98;
      ::_exit(static_cast<int>(std::stoul(argv[3])));
    }
    if (argc == 27 && std::string_view(argv[1]) == "--crash-execute") {
      api::EngineRequestContext context;
      context.trust_mode = api::EngineTrustMode::server_isolated;
      context.database_path = argv[3];
      context.database_uuid.canonical = argv[4];
      context.default_root_uuid.canonical = argv[5];
      context.current_schema_uuid.canonical = argv[6];
      context.principal_uuid.canonical = argv[7];
      context.session_uuid.canonical = argv[8];
      context.request_id = argv[9];
      context.local_transaction_id = std::stoull(argv[10]);
      context.transaction_uuid.canonical = argv[11];
      context.snapshot_visible_through_local_transaction_id =
          std::stoull(argv[12]);
      context.transaction_isolation_level = argv[13];
      context.statement_uuid.canonical = argv[14];
      context.statement_receipt_uuid.canonical = argv[15];
      context.statement_snapshot_uuid.canonical = argv[16];
      context.catalog_epoch_uuid.canonical = argv[17];
      context.resource_admission_uuid.canonical = argv[18];
      context.authorization_context.authority_uuid.canonical = argv[19];
      context.catalog_generation_id = std::stoull(argv[20]);
      context.security_epoch = std::stoull(argv[21]);
      context.authorization_context.security_epoch = context.security_epoch;
      context.resource_epoch = std::stoull(argv[22]);
      context.name_resolution_epoch = std::stoull(argv[23]);
      context.database_page_size_bytes = 16384;
      context.identifier_profile_uuid = "sbsql_v3";
      context.language_context.language_tag = "en";
      context.language_context.default_language_tag = "en";
      context.security_context_present = true;
      context.authorization_context.present = true;
      context.authorization_context.security_context_generation = 1;
      context.authorization_context.principal_uuid = context.principal_uuid;
      context.authorization_context.policy_epoch = 1;
      context.authorization_context.catalog_generation_id =
          context.catalog_generation_id;
      context.statement_metadata_snapshot_engine_owned = true;
      context.trace_tags.push_back("private_bulk_import_stream_compiler");
      const auto checkpoint =
          static_cast<api::BulkImportStreamExecutionCheckpointV1>(
              std::stoul(argv[24]));
      const auto exit_code = static_cast<int>(std::stoul(argv[25]));
      std::vector<std::uint8_t> biro;
      if (!DecodeHex(argv[26], &biro)) return 99;
      api::SblrBulkImportStreamRegistry registry(argv[2]);
      if (!registry.healthy()) return 98;
      api::EngineExecuteBulkImportStreamRequestV1 request;
      request.context = std::move(context);
      request.registry = &registry;
      request.canonical_biro = std::move(biro);
      request.conformance_checkpoint_observer =
          [checkpoint, exit_code](auto observed) {
            if (observed == checkpoint) ::_exit(exit_code);
          };
      (void)api::ExecuteBulkImportStreamV1(request);
      return 97;
    }
  } catch (...) {
    return 96;
  }
  return 95;
}

std::vector<api::CrudRowVersionRecord> VisibleRows(const Fixture& fixture) {
  api::MgaVisibleHeapRelationReadRequest request;
  request.relation_uuid = Text(fixture.relation);
  request.maximum_scanned_row_versions = 128;
  request.maximum_decoded_bytes = 1U << 20U;
  request.maximum_output_rows = 64;
  request.maximum_memory_bytes = 1U << 20U;
  request.cancellation_requested = [] { return false; };
  const auto read = api::ReadVisibleMgaHeapRelation(fixture.context, request);
  if (!read.ok) {
    std::cerr << read.diagnostic.code << ':' << read.diagnostic.message_key
              << ':' << read.diagnostic.detail << '\n';
  }
  Require(read.ok, "bounded visible-row read failed");
  return read.visible_rows;
}

std::map<std::string, std::string> Values(
    const api::CrudRowVersionRecord& row) {
  return {row.values.begin(), row.values.end()};
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) return RunCrashWorker(argc, argv);
  auto fixture = MakeFixture();
  auto registry = std::make_unique<api::SblrBulkImportStreamRegistry>(
      fixture.stream_root);
  Require(registry->healthy(), "durable stream registry failed to open");

  const auto success = CoordinateAndSeal(
      fixture, *registry, 11,
      "1,plain\n2,\"comma,value\"\n3,\"\\N\"\n4,\\N\n");
  api::EngineExecuteBulkImportStreamRequestV1 execute;
  execute.context = fixture.context;
  execute.registry = registry.get();
  execute.canonical_biro = success.biro;
  const auto executed = api::ExecuteBulkImportStreamV1(execute);
  if (!executed.ok) {
    std::cerr << executed.diagnostic.code << ':'
              << executed.diagnostic.message_key << ':'
              << executed.diagnostic.detail << '\n';
  }
  Require(executed.ok && !executed.replayed && executed.affected_rows == 4 &&
              executed.rejected_rows == 0 &&
              executed.canonical_birs.size() ==
                  sblr::BulkImportWireLayout::result_size,
          "strict CSV terminal execution failed");
  auto rows = VisibleRows(fixture);
  Require(rows.size() == 4, "strict CSV did not publish four rows");
  std::map<std::string, std::string> payload_by_id;
  for (const auto& row : rows) {
    const auto values = Values(row);
    Require(values.contains("id") && values.contains("payload"),
            "published row shape drifted");
    payload_by_id.emplace(values.at("id"), values.at("payload"));
  }
  Require(payload_by_id["1"] == "plain" &&
              payload_by_id["2"] == "comma,value" &&
              payload_by_id["3"] == "\\N" &&
              payload_by_id["4"] == "<NULL>",
          "strict CSV quote/null conversion drifted");

  const auto malformed = CoordinateAndSeal(
      fixture, *registry, 12, "5,accepted_before_error\n6\n");
  execute.canonical_biro = malformed.biro;
  const auto refused = api::ExecuteBulkImportStreamV1(execute);
  Require(!refused.ok && refused.diagnostic.code == "BULK.IMPORT.ABORTED" &&
              refused.canonical_birs.empty() && VisibleRows(fixture).size() == 4,
          "malformed stream was not refused atomically");
  api::BulkImportStreamEntry malformed_entry;
  Require(registry->Recover(malformed.allocation.stream_uuid,
                            &malformed_entry)
                  .ok &&
              malformed_entry.state == api::BulkImportStreamState::aborted,
          "malformed stream did not reach durable aborted state");

  const auto authority_refusal_rows = VisibleRows(fixture).size();
  const auto require_prelookup_refusal =
      [&](const PreparedStream& stream,
          const api::EngineRequestContext& context,
          std::string_view expected_code,
          std::string_view failure_detail) {
        execute.context = context;
        execute.registry = registry.get();
        execute.canonical_biro = stream.biro;
        execute.conformance_checkpoint_observer = {};
        const auto refused_by_authority =
            api::ExecuteBulkImportStreamV1(execute);
        api::BulkImportStreamEntry entry;
        Require(!refused_by_authority.ok &&
                    refused_by_authority.diagnostic.code == expected_code &&
                    refused_by_authority.canonical_birs.empty() &&
                    registry->Recover(stream.allocation.stream_uuid, &entry)
                        .ok &&
                    entry.state == api::BulkImportStreamState::sealed &&
                    VisibleRows(fixture).size() == authority_refusal_rows,
                failure_detail);
      };

  const auto security_refusal = CoordinateAndSeal(
      fixture, *registry, 13, "13,security_refused\n");
  auto invalid_context = fixture.context;
  invalid_context.security_context_present = false;
  require_prelookup_refusal(security_refusal, invalid_context,
                            "SECURITY.ACCESS_DENIED",
                            "security refusal disclosed or mutated stream state");

  const auto transaction_refusal = CoordinateAndSeal(
      fixture, *registry, 14, "14,transaction_refused\n");
  invalid_context = fixture.context;
  invalid_context.local_transaction_id = 0;
  require_prelookup_refusal(transaction_refusal, invalid_context,
                            "MGA.TRANSACTION_INVALID",
                            "invalid MGA identity crossed the stream lookup boundary");

  const auto authority_mismatch = CoordinateAndSeal(
      fixture, *registry, 15, "15,authority_mismatch\n");
  invalid_context = fixture.context;
  ++invalid_context.catalog_generation_id;
  require_prelookup_refusal(authority_mismatch, invalid_context,
                            "MGA.AUTHORITY_MISMATCH",
                            "stale catalog authority crossed the stream lookup boundary");
  execute.context = fixture.context;

  const auto first_birs = executed.canonical_birs;
  registry.reset();
  registry = std::make_unique<api::SblrBulkImportStreamRegistry>(
      fixture.stream_root);
  Require(registry->healthy(), "durable registry reopen failed");
  execute.registry = registry.get();
  execute.canonical_biro = success.biro;
  const auto replayed = api::ExecuteBulkImportStreamV1(execute);
  Require(replayed.ok && replayed.replayed &&
              replayed.canonical_birs == first_birs &&
              VisibleRows(fixture).size() == 4,
          "exact terminal replay was not byte-identical and mutation-free");
  sblr::SblrBulkImportStreamResultV1 decoded;
  Require(sblr::DecodeSblrBulkImportStreamResultV1(
              replayed.canonical_birs.data(), replayed.canonical_birs.size(),
              &decoded, nullptr) &&
              decoded.availability_generation ==
                  success.allocation.executor_availability_generation,
          "recorded BIRS failed canonical decoding");

  using Checkpoint = api::BulkImportStreamExecutionCheckpointV1;
  const auto prepublication_rows = VisibleRows(fixture).size();
  for (const auto [checkpoint, structural, row_id] : {
           std::tuple{Checkpoint::before_descriptor_lookup, 21ULL, 21},
           std::tuple{Checkpoint::before_target_generation_revalidation,
                      22ULL, 22},
           std::tuple{Checkpoint::before_stream_or_publication_lock, 23ULL,
                      23},
           std::tuple{Checkpoint::immediately_before_durable_publication,
                      24ULL, 24}}) {
    const auto stream = CoordinateAndSeal(
        fixture, *registry, structural,
        std::to_string(row_id) + ",cancelled\n");
    bool cancellation_requested = false;
    bool checkpoint_observed = false;
    execute.context = fixture.context;
    execute.context.query_cancellation_requested =
        [&] { return cancellation_requested; };
    execute.registry = registry.get();
    execute.canonical_biro = stream.biro;
    execute.conformance_checkpoint_observer = [&](Checkpoint observed) {
      if (observed == checkpoint) {
        checkpoint_observed = true;
        cancellation_requested = true;
      }
    };
    const auto cancelled = api::ExecuteBulkImportStreamV1(execute);
    Require(checkpoint_observed && !cancelled.ok &&
                cancelled.diagnostic.code == "PROCESS.CANCELLED" &&
                cancelled.canonical_birs.empty() &&
                VisibleRows(fixture).size() == prepublication_rows,
            "one pre-publication cancellation checkpoint was not atomic");
    api::BulkImportStreamEntry cancelled_entry;
    Require(registry->Recover(stream.allocation.stream_uuid,
                              &cancelled_entry)
                .ok &&
                cancelled_entry.state !=
                    api::BulkImportStreamState::published &&
                cancelled_entry.state !=
                    api::BulkImportStreamState::evidenced &&
                cancelled_entry.state !=
                    api::BulkImportStreamState::result_recorded,
            "pre-publication cancellation crossed the durable barrier");
  }

  const auto postpublication = CoordinateAndSeal(
      fixture, *registry, 25, "25,published_despite_late_cancel\n");
  bool late_cancellation = false;
  bool postpublication_observed = false;
  execute.context = fixture.context;
  execute.context.query_cancellation_requested =
      [&] { return late_cancellation; };
  execute.canonical_biro = postpublication.biro;
  execute.conformance_checkpoint_observer = [&](Checkpoint observed) {
    if (observed == Checkpoint::after_durable_publication) {
      postpublication_observed = true;
      late_cancellation = true;
    }
  };
  const auto completed_after_late_cancel =
      api::ExecuteBulkImportStreamV1(execute);
  Require(postpublication_observed && completed_after_late_cancel.ok &&
              !completed_after_late_cancel.canonical_birs.empty() &&
              VisibleRows(fixture).size() == prepublication_rows + 1,
          "post-publication cancellation did not complete exact finality");

  execute.conformance_checkpoint_observer = {};
  execute.context = fixture.context;

  const auto reopen_registry = [&] {
    registry.reset();
    registry = std::make_unique<api::SblrBulkImportStreamRegistry>(
        fixture.stream_root);
    Require(registry->healthy(), "durable registry crash reopen failed");
    execute.registry = registry.get();
  };
  const auto recover_execute = [&](const PreparedStream& stream,
                                   std::size_t expected_rows,
                                   std::string_view detail) {
    execute.context = fixture.context;
    execute.registry = registry.get();
    execute.canonical_biro = stream.biro;
    execute.conformance_checkpoint_observer = {};
    const auto recovered = api::ExecuteBulkImportStreamV1(execute);
    Require(recovered.ok && !recovered.canonical_birs.empty() &&
                VisibleRows(fixture).size() == expected_rows,
            detail);
    return recovered.canonical_birs;
  };

  std::size_t durable_rows = prepublication_rows + 1;
  auto crash_after_chunk = CoordinateStream(
      fixture, *registry, 31, "31,recovered_after_chunk\n");
  registry.reset();
  SpawnAppendWorker(fixture, crash_after_chunk, 81,
                    "worker did not crash after durable chunk");
  reopen_registry();
  api::BulkImportStreamEntry crash_entry;
  Require(registry->Recover(crash_after_chunk.allocation.stream_uuid,
                            &crash_entry)
              .ok &&
              crash_entry.state == api::BulkImportStreamState::receiving,
          "durable chunk was not recoverable after process loss");
  SealPreparedStream(*registry, crash_after_chunk);
  recover_execute(crash_after_chunk, ++durable_rows,
                  "execution did not recover after durable-chunk crash");

  auto crash_after_seal = CoordinateStream(
      fixture, *registry, 32, "32,recovered_after_seal\n");
  AppendPreparedStream(*registry, crash_after_seal);
  registry.reset();
  SpawnSealWorker(fixture, crash_after_seal, 82,
                  "worker did not crash after durable seal");
  reopen_registry();
  Require(registry->Recover(crash_after_seal.allocation.stream_uuid,
                            &crash_entry)
              .ok &&
              crash_entry.state == api::BulkImportStreamState::sealed,
          "durable seal was not recoverable after process loss");
  recover_execute(crash_after_seal, ++durable_rows,
                  "execution did not recover after durable-seal crash");

  const auto crash_execute = [&](std::uint64_t structural,
                                 std::string_view csv,
                                 Checkpoint checkpoint, int exit_code,
                                 std::string_view crash_detail,
                                 std::string_view recovery_detail) {
    auto stream = CoordinateAndSeal(fixture, *registry, structural, csv);
    registry.reset();
    SpawnExecutionWorker(fixture, stream, checkpoint, exit_code,
                         crash_detail);
    reopen_registry();
    recover_execute(stream, ++durable_rows, recovery_detail);
  };

  crash_execute(33, "33,recovered_prepublication\n",
                Checkpoint::immediately_before_durable_publication, 83,
                "child did not stop immediately before publication",
                "pre-publication execution crash did not recover exactly once");
  crash_execute(34, "34,recovered_postpublication\n",
                Checkpoint::after_durable_publication, 84,
                "child did not stop immediately after publication",
                "post-publication crash did not recover without duplicate rows");
  crash_execute(35, "35,recovered_after_evidence\n",
                Checkpoint::after_executor_evidence_before_result, 85,
                "child did not stop after executor evidence",
                "evidenced stream did not recover its exact terminal result");

  const auto fresh_descriptor = CoordinateAndSeal(
      fixture, *registry, 36, "35,recovered_after_evidence\n");
  execute.context = fixture.context;
  execute.registry = registry.get();
  execute.canonical_biro = fresh_descriptor.biro;
  execute.conformance_checkpoint_observer = {};
  const auto fresh_result = api::ExecuteBulkImportStreamV1(execute);
  Require(fresh_result.ok && !fresh_result.replayed &&
              fresh_descriptor.allocation.stream_uuid !=
                  success.allocation.stream_uuid &&
              fresh_result.canonical_birs != first_birs &&
              VisibleRows(fixture).size() == durable_rows + 1,
          "a fresh descriptor incorrectly recovered an earlier operation");

  Rollback(fixture.context);
  return EXIT_SUCCESS;
}
