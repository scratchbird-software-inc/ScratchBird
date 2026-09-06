// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/relation_projection_view.hpp"
#include "ddl/create_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

bool HasDiagnostic(const api::EngineApiResult& result,
                   std::string_view detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail.find(detail) != std::string::npos) return true;
  }
  return false;
}

std::string EvidenceValue(const api::EngineApiResult& result,
                          std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) return evidence.evidence_id;
  }
  return {};
}

std::uint64_t NowMillis() {
  static std::uint64_t sequence = 0;
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) +
         (++sequence * 1000);
}

platform::TypedUuid NewTypedUuid(platform::UuidKind kind,
                                std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, salt);
  Require(generated.ok(), "relation projection UUID generation failed");
  return generated.value;
}

std::string NewUuid(platform::UuidKind kind, std::uint64_t salt) {
  return uuid::UuidToString(NewTypedUuid(kind, salt).value);
}

bool CanonicalObjectUuid(std::string_view value) {
  const auto parsed = uuid::ParseTypedUuid(
      platform::UuidKind::object, std::string(value));
  return parsed.ok() && uuid::UuidToString(parsed.value.value) == value;
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string principal_uuid;
  std::string schema_uuid;
  std::string singleton_table_uuid;
  std::string multi_table_uuid;
  std::string updatable_table_uuid;
  api::MgaRelationStorageDescriptor singleton_descriptor;
  api::MgaRelationStorageDescriptor multi_descriptor;
  api::MgaRelationStorageDescriptor updatable_descriptor;
  std::uint64_t salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

Fixture CreateFixture() {
  Fixture fixture;
  fixture.salt = NowMillis();
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_relation_projection_view_" +
                       std::to_string(fixture.salt));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "relation_projection.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid =
      NewTypedUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid =
      NewTypedUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = fixture.salt + 3;
  create.page_size = 8192;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "relation projection database creation failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.principal_uuid =
      NewUuid(platform::UuidKind::principal, fixture.salt + 4);
  fixture.schema_uuid =
      NewUuid(platform::UuidKind::schema, fixture.salt + 5);
  fixture.singleton_table_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 6);
  fixture.multi_table_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 7);
  fixture.updatable_table_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 8);
  return fixture;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::uint64_t ordinal,
                                std::string isolation = "read_committed") {
  api::EngineBeginTransactionRequest begin;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id =
      "relation-projection-begin-" + std::to_string(ordinal);
  begin.context.database_path = fixture.database_path.string();
  begin.context.database_uuid.canonical = fixture.database_uuid;
  begin.context.principal_uuid.canonical = fixture.principal_uuid;
  begin.context.session_uuid.canonical =
      NewUuid(platform::UuidKind::object, fixture.salt + 100 + ordinal);
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch = 1;
  begin.context.name_resolution_epoch = 1;
  begin.isolation_level = std::move(isolation);
  const auto begun = api::EngineBeginTransaction(begin);
  RequireOk(begun, "relation projection transaction begin failed");

  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request),
            "relation projection transaction commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request),
            "relation projection transaction rollback failed");
}

api::EngineLocalizedName Name(std::string value) {
  api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = value;
  name.raw_name_text = value;
  name.display_name = value;
  name.default_name = true;
  return name;
}

api::CrudTableRecord Int32Table(const api::EngineRequestContext& context,
                                std::string table_uuid,
                                std::string name,
                                bool persisted_int_alias = false) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = std::move(table_uuid);
  table.default_name = std::move(name);
  table.columns.push_back({
      "ID",
      persisted_int_alias
          ? "type=int;nullable=true"
          : "canonical=int32;precision=32;scale=0;nullable=true"});
  return table;
}

void PersistTable(const api::EngineRequestContext& context,
                  const api::CrudTableRecord& table,
                  api::MgaRelationStorageDescriptor* descriptor) {
  Require(!api::AppendMgaTableMetadata(context, table).error,
          "relation projection table metadata append failed");
  Require(!api::EnsureMgaRelationStorageDescriptor(
               context, table, {}, descriptor)
               .error,
          "relation projection descriptor persistence failed");
  Require(descriptor != nullptr && descriptor->columns.size() == 1 &&
              descriptor->columns[0].canonical_name_key == "ID" &&
              !descriptor->columns[0]
                   .value_descriptor.descriptor_uuid.canonical.empty(),
          "relation projection source descriptor drifted");
}

std::string ReadDescriptorStoreBytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(input.good(), "descriptor cache fixture store open failed");
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  Require(size >= 0, "descriptor cache fixture store size failed");
  input.seekg(0, std::ios::beg);
  std::string bytes(static_cast<std::size_t>(size), '\0');
  if (!bytes.empty()) {
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  Require(input.good() || input.eof(),
          "descriptor cache fixture store read failed");
  return bytes;
}

void RewriteDescriptorStoreBytes(const std::filesystem::path& path,
                                 std::string_view bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  Require(output.good(), "descriptor cache fixture store rewrite open failed");
  if (!bytes.empty()) {
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  output.close();
  Require(output.good(), "descriptor cache fixture store rewrite failed");
  std::error_code size_error;
  Require(std::filesystem::file_size(path, size_error) == bytes.size() &&
              !size_error,
          "descriptor cache fixture store rewrite changed file size");
}

void TestExactDescriptorCacheFalseNegativeRecovery() {
  auto fixture = CreateFixture();
  auto metadata = Begin(fixture, 1000);
  api::MgaRelationStorageDescriptor expected;
  PersistTable(metadata,
               Int32Table(metadata,
                          fixture.singleton_table_uuid,
                          "descriptor_cache_source"),
               &expected);
  Commit(metadata);

  auto reader = Begin(fixture, 1001);
  const std::filesystem::path descriptor_path(
      fixture.database_path.string() + ".sb.mga_relation_descriptors");
  const std::string valid_bytes = ReadDescriptorStoreBytes(descriptor_path);
  Require(valid_bytes.rfind("SBMGADESC1\tRELATION\t", 0) == 0,
          "descriptor cache fixture durable magic missing");

  std::error_code time_error;
  const auto valid_mtime =
      std::filesystem::last_write_time(descriptor_path, time_error);
  Require(!time_error, "descriptor cache fixture mtime read failed");

  // Move the valid store to a distinct cache identity first.  This prevents
  // the descriptor created above from satisfying the corruption probe from
  // the positive cache populated during EnsureMgaRelationStorageDescriptor.
  std::filesystem::last_write_time(
      descriptor_path, valid_mtime + std::chrono::hours(1), time_error);
  Require(!time_error, "descriptor cache fixture shifted mtime write failed");
  const auto shifted_mtime =
      std::filesystem::last_write_time(descriptor_path, time_error);
  Require(!time_error && shifted_mtime != valid_mtime,
          "descriptor cache fixture could not shift file identity");
  const auto shifted_valid = api::LoadMgaRelationStorageDescriptor(
      reader, fixture.singleton_table_uuid);
  Require(shifted_valid.ok &&
              shifted_valid.descriptor.descriptor_uuid.canonical ==
                  expected.descriptor_uuid.canonical,
          "descriptor cache fixture valid warm load failed");

  std::string invalid_bytes = valid_bytes;
  invalid_bytes.front() = invalid_bytes.front() == 'X' ? 'Y' : 'X';
  RewriteDescriptorStoreBytes(descriptor_path, invalid_bytes);
  time_error.clear();
  std::filesystem::last_write_time(
      descriptor_path, valid_mtime, time_error);
  Require(!time_error &&
              std::filesystem::last_write_time(descriptor_path) == valid_mtime,
          "descriptor cache fixture invalid store mtime restore failed");

  const auto negative = api::LoadMgaRelationStorageDescriptor(
      reader, fixture.singleton_table_uuid);
  Require(!negative.ok &&
              negative.diagnostic.detail.find("persisted_descriptor_required") !=
                  std::string::npos,
          "invalid descriptor magic did not prime an exact negative cache");

  RewriteDescriptorStoreBytes(descriptor_path, valid_bytes);
  time_error.clear();
  std::filesystem::last_write_time(
      descriptor_path, valid_mtime, time_error);
  Require(!time_error &&
              std::filesystem::last_write_time(descriptor_path) == valid_mtime,
          "descriptor cache fixture valid store mtime restore failed");

  const auto recovered = api::LoadMgaRelationStorageDescriptor(
      reader, fixture.singleton_table_uuid);
  Require(recovered.ok &&
              recovered.descriptor.relation_uuid.canonical ==
                  expected.relation_uuid.canonical &&
              recovered.descriptor.descriptor_uuid.canonical ==
                  expected.descriptor_uuid.canonical &&
              recovered.descriptor.columns.size() == expected.columns.size() &&
              recovered.descriptor.columns[0]
                      .value_descriptor.descriptor_uuid.canonical ==
                  expected.columns[0]
                      .value_descriptor.descriptor_uuid.canonical,
          "exact descriptor load did not recover from an identity-matching "
          "negative cache");
  Rollback(reader);
}

api::EngineTypedValue Int32Value(
    const api::EngineDescriptor& descriptor,
    std::int32_t value) {
  api::EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.encoded_value = std::to_string(value);
  typed.setState(api::EngineValueState::value);
  return typed;
}

api::EngineRowValue Row(const api::EngineDescriptor& descriptor,
                        std::int32_t value,
                        std::uint64_t ordinal) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical =
      NewUuid(platform::UuidKind::row, NowMillis() + ordinal);
  row.fields.push_back({"ID", Int32Value(descriptor, value)});
  return row;
}

api::EngineRowValue NullRow(const api::EngineDescriptor& descriptor,
                            std::uint64_t ordinal) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical =
      NewUuid(platform::UuidKind::row, NowMillis() + ordinal);
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.setState(api::EngineValueState::sql_null);
  row.fields.push_back({"ID", std::move(value)});
  return row;
}

void Insert(const api::EngineRequestContext& context,
            const std::string& table_uuid,
            const api::EngineDescriptor& descriptor,
            std::vector<std::int32_t> values) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.context.request_id = "relation-projection-insert";
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  std::uint64_t ordinal = 0;
  for (const auto value : values) {
    request.input_rows.push_back(Row(descriptor, value, ++ordinal));
  }
  request.estimated_row_count = request.input_rows.size();
  const auto inserted = api::EngineInsertRows(request);
  RequireOk(inserted, "relation projection insert failed");
  Require(inserted.inserted_count == request.estimated_row_count,
          "relation projection insert count drifted");
}

void InsertDelete03Rows(const api::EngineRequestContext& context,
                        const std::string& table_uuid,
                        const api::EngineDescriptor& descriptor) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.context.request_id = "relation-projection-v2-insert";
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows = {
      Row(descriptor, 10, 101),
      Row(descriptor, 10, 102),
      NullRow(descriptor, 103)};
  request.estimated_row_count = request.input_rows.size();
  const auto inserted = api::EngineInsertRows(request);
  RequireOk(inserted, "updatable relation projection insert failed");
  Require(inserted.inserted_count == 3,
          "updatable relation projection insert count drifted");
}

struct Delete03VisibleRows {
  std::size_t row_count = 0;
  std::size_t ten_count = 0;
  std::size_t null_count = 0;
  std::size_t other_count = 0;
};

Delete03VisibleRows ReadDelete03VisibleRows(
    const api::EngineRequestContext& context,
    const std::string& table_uuid) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "updatable relation projection MGA state load failed");
  const auto state = api::BuildMgaRelationReadView(loaded.state);
  const auto rows =
      api::VisibleCrudRowsForContext(state, table_uuid, context);
  Delete03VisibleRows counts;
  counts.row_count = rows.size();
  for (const auto& row : rows) {
    bool found = false;
    for (const auto& [name, value] : row.values) {
      if (name != "ID") continue;
      found = true;
      if (value == "10") {
        ++counts.ten_count;
      } else if (value == "<NULL>") {
        ++counts.null_count;
      } else {
        ++counts.other_count;
      }
    }
    if (!found) ++counts.other_count;
  }
  return counts;
}

void RequireDelete03VisibleRows(
    const api::EngineRequestContext& context,
    const std::string& table_uuid,
    std::size_t expected_tens,
    std::size_t expected_nulls,
    std::string_view message) {
  const auto counts = ReadDelete03VisibleRows(context, table_uuid);
  Require(counts.row_count == expected_tens + expected_nulls &&
              counts.ten_count == expected_tens &&
              counts.null_count == expected_nulls &&
              counts.other_count == 0,
          message);
}

std::size_t VisibleRowCount(const api::EngineRequestContext& context,
                            const std::string& table_uuid) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "relation projection MGA row-count load failed");
  const auto state = api::BuildMgaRelationReadView(loaded.state);
  return api::VisibleCrudRowsForContext(state, table_uuid, context).size();
}

api::EngineCreateViewRequest ViewRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& source,
    std::string view_name,
    std::string literal_output_name) {
  Require(source.columns.size() == 1,
          "relation projection source width drifted");
  api::EngineCreateViewRequest request;
  request.context = context;
  request.context.request_id = "relation-projection-create";
  request.operation_id = "ddl.create_view";
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_schema.object_kind = "schema";
  request.target_object.object_kind = "view";
  request.localized_names.push_back(Name(std::move(view_name)));
  request.related_objects.push_back({source.relation_uuid, "table"});

  api::EngineColumnDefinition source_output;
  source_output.requested_column_uuid = source.columns[0].column_uuid;
  source_output.names.push_back(Name("ID"));
  source_output.descriptor = source.columns[0].value_descriptor;
  source_output.ordinal = 0;
  source_output.nullable = source.columns[0].nullable;
  request.columns.push_back(std::move(source_output));

  api::EngineColumnDefinition literal_output;
  literal_output.names.push_back(Name(std::move(literal_output_name)));
  literal_output.descriptor =
      api::EngineRelationProjectionInt32LiteralInputDescriptor();
  literal_output.ordinal = 1;
  literal_output.nullable = false;
  request.columns.push_back(std::move(literal_output));

  request.assignments.push_back(
      {"projection_1_literal",
       Int32Value(api::EngineRelationProjectionInt32LiteralInputDescriptor(),
                  5)});
  request.projection.canonical_projection_envelopes = {
      api::kEngineRelationProjectionSourceColumnV1,
      api::kEngineRelationProjectionTypedInt32LiteralV1};
  request.option_envelopes = {
      std::string("view_query_shape:") +
          api::kEngineRelationProjectionViewMarkerV1,
      "source_relation_descriptor_uuid:" +
          source.descriptor_uuid.canonical,
      "source_relation_descriptor_generation:" +
          std::to_string(source.descriptor_generation),
      "source_resource_epoch:" +
          std::to_string(context.resource_epoch)};
  return request;
}

api::EngineCreateViewRequest UpdatableViewRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& source,
    std::string view_name = "TEST_DELETE_03") {
  Require(source.columns.size() == 1,
          "updatable relation projection source width drifted");
  api::EngineCreateViewRequest request;
  request.context = context;
  request.context.request_id = "relation-projection-v2-create";
  request.operation_id = "ddl.create_view";
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_schema.object_kind = "schema";
  request.target_object.object_kind = "view";
  request.localized_names.push_back(Name(std::move(view_name)));
  request.related_objects.push_back({source.relation_uuid, "table"});

  api::EngineColumnDefinition output;
  output.requested_column_uuid = source.columns[0].column_uuid;
  output.names.push_back(Name("ID"));
  output.descriptor = source.columns[0].value_descriptor;
  output.ordinal = 0;
  output.nullable = source.columns[0].nullable;
  request.columns.push_back(std::move(output));

  request.projection.canonical_projection_envelopes = {
      api::kEngineRelationProjectionSourceColumnV1};
  request.option_envelopes = {
      std::string("view_query_shape:") +
          api::kEngineRelationProjectionViewMarkerV2,
      "source_relation_descriptor_uuid:" +
          source.descriptor_uuid.canonical,
      "source_relation_descriptor_generation:" +
          std::to_string(source.descriptor_generation),
      "source_resource_epoch:" +
          std::to_string(context.resource_epoch)};
  return request;
}

api::EngineResolveNameRequest ResolveRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string name) {
  api::EngineResolveNameRequest request;
  request.context = context;
  request.context.request_id = "relation-projection-resolve";
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_schema.object_kind = "schema";
  request.target_object.object_kind = "view";
  request.localized_names.push_back(Name(name));
  request.sql_object_reference.expected_object_type = "view";
  request.sql_object_reference.object_name.raw_text = std::move(name);
  request.sql_object_reference.object_name.identifier_profile_uuid =
      context.identifier_profile_uuid;
  return request;
}

api::EngineSelectRowsRequest SelectRequest(
    const api::EngineRequestContext& context,
    const api::EngineRelationProjectionViewDescriptor& descriptor) {
  Require(descriptor.present && descriptor.outputs.size() == 2,
          "relation projection durable descriptor required");
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.context.request_id = "relation-projection-select";
  request.source_object.uuid = descriptor.view_uuid;
  request.source_object.object_kind = "view";
  request.relation_projection_view.present = true;
  request.relation_projection_view.marker =
      api::kEngineRelationProjectionViewMarkerV1;
  request.relation_projection_view.view_uuid = descriptor.view_uuid;
  request.relation_projection_view.view_descriptor_uuid =
      descriptor.view_descriptor_uuid;
  request.relation_projection_view.view_descriptor_generation =
      descriptor.view_descriptor_generation;
  request.relation_projection_view.outputs =
      api::EngineRelationProjectionViewSemanticOutputs(descriptor);
  return request;
}

api::EngineSelectRowsRequest SelectRequest(
    const api::EngineRequestContext& context,
    const api::EngineResolveNameResult& resolved) {
  Require(resolved.ok && resolved.semantic_projection.present &&
              resolved.semantic_projection.marker ==
                  api::kEngineRelationProjectionViewMarkerV1 &&
              resolved.semantic_projection.ordered_outputs.size() == 2,
          "relation projection semantic descriptor required");
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.context.request_id = "relation-projection-select";
  request.source_object.uuid = resolved.bound_object_identity.object_uuid;
  request.source_object.object_kind = "view";
  request.relation_projection_view.present = true;
  request.relation_projection_view.marker =
      resolved.semantic_projection.marker;
  request.relation_projection_view.view_uuid =
      resolved.bound_object_identity.object_uuid;
  request.relation_projection_view.view_descriptor_uuid =
      resolved.semantic_projection.projection_descriptor.descriptor_uuid;
  request.relation_projection_view.view_descriptor_generation =
      resolved.semantic_projection.descriptor_generation;
  request.relation_projection_view.outputs =
      resolved.semantic_projection.ordered_outputs;
  return request;
}

void ReplaceOnce(std::string* text,
                 const std::string& expected,
                 const std::string& replacement) {
  Require(text != nullptr, "relation projection replacement text required");
  const auto offset = text->find(expected);
  Require(offset != std::string::npos &&
              text->find(expected, offset + expected.size()) ==
                  std::string::npos,
          "relation projection persisted field was not unique");
  text->replace(offset, expected.size(), replacement);
}

std::string Hex(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2u);
  for (const unsigned char byte : value) {
    encoded.push_back(kHex[(byte >> 4u) & 0x0fu]);
    encoded.push_back(kHex[byte & 0x0fu]);
  }
  return encoded;
}

std::string PackedRelationProjectionViewCreate(
    const api::MgaRelationStorageDescriptor& relation,
    std::uint32_t ordinal,
    std::string_view output_name) {
  Require(relation.columns.size() == 1 && ordinal < 2,
          "rpvc1 source relation shape drifted");
  const bool source = ordinal == 0;
  const auto& source_column = relation.columns.front();
  const api::EngineDescriptor type =
      source ? source_column.value_descriptor
             : api::EngineRelationProjectionInt32LiteralInputDescriptor();
  return "rpvc1|0|" + std::to_string(ordinal) + "|" +
         Hex(relation.relation_uuid.canonical) + "|" +
         Hex(relation.descriptor_uuid.canonical) + "|" +
         std::to_string(relation.descriptor_generation) + "|1|" +
         Hex(output_name) + "|" +
         Hex(source ? api::kEngineRelationProjectionSourceColumnV1
                    : api::kEngineRelationProjectionTypedInt32LiteralV1) +
         "|" +
         Hex(source ? source_column.column_uuid.canonical : std::string{}) +
         "|" +
         Hex(source
                 ? source_column.value_descriptor.descriptor_uuid.canonical
                 : std::string{}) +
         "|" + Hex(type.descriptor_kind) + "|" +
         Hex(type.canonical_type_name) + "|" +
         Hex(type.encoded_descriptor) + "|" +
         (source && source_column.nullable ? "1" : "0") + "|" +
         Hex(source ? std::string_view{} : std::string_view{"5"});
}

std::string PackedUpdatableRelationProjectionViewCreate(
    const api::MgaRelationStorageDescriptor& relation,
    std::string_view output_name = "ID",
    std::uint64_t generation_override = 0,
    std::uint64_t resource_epoch_override = 0) {
  Require(relation.columns.size() == 1,
          "rpvc2 source relation shape drifted");
  const auto& source = relation.columns.front();
  const auto generation = generation_override == 0
                              ? relation.descriptor_generation
                              : generation_override;
  const auto resource_epoch =
      resource_epoch_override == 0 ? 1 : resource_epoch_override;
  return "rpvc2|0|0|" + Hex(relation.relation_uuid.canonical) + "|" +
         Hex(relation.descriptor_uuid.canonical) + "|" +
         std::to_string(generation) + "|" +
         std::to_string(resource_epoch) + "|" +
         Hex(output_name) + "|" + Hex(source.canonical_name_key) + "|" +
         Hex(source.column_uuid.canonical) + "|" +
         Hex(source.value_descriptor.descriptor_uuid.canonical) + "|" +
         Hex(source.value_descriptor.descriptor_kind) + "|" +
         Hex(source.value_descriptor.canonical_type_name) + "|" +
         Hex(source.value_descriptor.encoded_descriptor) + "|" +
         (source.nullable ? "1" : "0") + "|" + Hex("");
}

sblr::SblrDispatchResult DispatchRelationProjectionViewCreate(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& relation,
    std::string view_name,
    std::string projection_0,
    std::string projection_1,
    std::string marker = api::kEngineRelationProjectionViewMarkerV1,
    std::string projection_count = "2",
    std::vector<sblr::SblrOperand> extra_operands = {}) {
  auto envelope = sblr::MakeSblrEnvelope(
      "ddl.create_view", "SBLR_DDL_CREATE_VIEW",
      "SBLR-RELATION-PROJECTION-VIEW-CREATE-CONFORMANCE");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands = {
      {"text", "target_object_kind", "view"},
      {"text", "view_name", view_name},
      {"text", "name", std::move(view_name)},
      {"text", "target_schema_uuid", fixture.schema_uuid},
      {"text", "view_projection_count", std::move(projection_count)},
      {"text", "view_query_shape", std::move(marker)},
      {"text", "view_source_uuid", relation.relation_uuid.canonical},
      {"text", "view_projection_0", std::move(projection_0)},
      {"text", "view_projection_1", std::move(projection_1)}};
  for (auto& operand : extra_operands) {
    envelope.operands.push_back(std::move(operand));
  }
  sblr::SblrDispatchRequest request;
  request.context = context;
  request.envelope = std::move(envelope);
  return sblr::DispatchSblrOperation(std::move(request));
}

sblr::SblrDispatchResult DispatchUpdatableRelationProjectionViewCreate(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& relation,
    std::string view_name,
    std::string packed,
    std::string marker = api::kEngineRelationProjectionViewMarkerV2,
    std::string projection_count = "1",
    std::vector<sblr::SblrOperand> extra_operands = {}) {
  auto envelope = sblr::MakeSblrEnvelope(
      "ddl.create_view", "SBLR_DDL_CREATE_VIEW",
      "SBLR-UPDATABLE-RELATION-PROJECTION-VIEW-CREATE-CONFORMANCE");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands = {
      {"text", "target_object_kind", "view"},
      {"text", "view_name", view_name},
      {"text", "name", std::move(view_name)},
      {"text", "target_schema_uuid", fixture.schema_uuid},
      {"text", "view_projection_count", std::move(projection_count)},
      {"text", "view_query_shape", std::move(marker)},
      {"text", "view_source_uuid", relation.relation_uuid.canonical},
      {"text", "view_projection_0", std::move(packed)}};
  for (auto& operand : extra_operands) {
    envelope.operands.push_back(std::move(operand));
  }
  sblr::SblrDispatchRequest request;
  request.context = context;
  request.envelope = std::move(envelope);
  return sblr::DispatchSblrOperation(std::move(request));
}

std::string PackedRelationProjectionViewSelect(
    const api::EngineResolveNameResult& resolved,
    std::uint64_t generation_override = 0) {
  Require(resolved.ok && resolved.semantic_projection.present &&
              resolved.semantic_projection.marker ==
                  api::kEngineRelationProjectionViewMarkerV1 &&
              resolved.semantic_projection.ordered_outputs.size() == 2,
          "rpvs1 semantic projection required");
  const auto& semantic = resolved.semantic_projection;
  const auto generation = generation_override == 0
                              ? semantic.descriptor_generation
                              : generation_override;
  std::string packed =
      "rpvs1|" + Hex(semantic.marker) + "|" +
      Hex(semantic.projection_descriptor.descriptor_uuid.canonical) + "|" +
      std::to_string(generation) + "|2";
  for (const auto& output : semantic.ordered_outputs) {
    packed += "|" + std::to_string(output.ordinal) + "|" +
              Hex(output.output_name) + "|" +
              Hex(output.output_column_uuid.canonical) + "|" +
              Hex(output.output_type.type_descriptor_uuid.canonical) + "|" +
              Hex(output.output_type.descriptor_kind) + "|" +
              Hex(output.output_type.canonical_type_name) + "|" +
              Hex(output.output_type.encoded_descriptor) + "|" +
              Hex(output.nullable ? "1" : "0");
  }
  return packed;
}

std::string PackedUpdatableRelationProjectionViewDelete(
    const api::EngineRelationProjectionViewDescriptor& descriptor,
    std::uint64_t generation_override = 0,
    std::string_view output_name_override = {}) {
  Require(descriptor.present &&
              descriptor.marker ==
                  api::kEngineRelationProjectionViewMarkerV2 &&
              descriptor.outputs.size() == 1,
          "rpvd2 durable descriptor required");
  const auto& output = descriptor.outputs.front();
  const auto generation = generation_override == 0
                              ? descriptor.view_descriptor_generation
                              : generation_override;
  const std::string_view output_name = output_name_override.empty()
                                           ? output.output_name
                                           : output_name_override;
  return "rpvd2|" +
         Hex(api::kEngineRelationProjectionViewMarkerV2) + "|" +
         Hex(descriptor.view_descriptor_uuid.canonical) + "|" +
         std::to_string(generation) + "|1|0|" + Hex(output_name) + "|" +
         Hex(output.output_column_uuid.canonical) + "|" +
         Hex(output.output_type.type_descriptor_uuid.canonical) + "|" +
         Hex(output.output_type.descriptor_kind) + "|" +
         Hex(output.output_type.canonical_type_name) + "|" +
         Hex(output.output_type.encoded_descriptor) + "|" +
         Hex(output.nullable ? "1" : "0");
}

api::EngineDeleteRowsRequest UpdatableDeleteRequest(
    const api::EngineRequestContext& context,
    const api::EngineRelationProjectionViewDescriptor& descriptor,
    std::int32_t predicate_value = 10) {
  Require(descriptor.present &&
              descriptor.marker ==
                  api::kEngineRelationProjectionViewMarkerV2 &&
              descriptor.outputs.size() == 1,
          "updatable view delete descriptor required");
  const auto& output = descriptor.outputs.front();
  api::EngineDeleteRowsRequest request;
  request.context = context;
  request.context.request_id = "relation-projection-v2-delete";
  request.target_table.uuid = descriptor.view_uuid;
  request.target_table.object_kind = "view";
  request.delete_surface_variant =
      api::kEngineRelationProjectionViewMarkerV2;
  request.delete_predicate.predicate_kind = "column_equals";
  request.delete_predicate.canonical_predicate_envelope =
      output.output_name;
  api::EngineDescriptor predicate_descriptor;
  predicate_descriptor.descriptor_kind = "scalar";
  predicate_descriptor.canonical_type_name = "int32";
  predicate_descriptor.encoded_descriptor = "type=int32";
  request.delete_predicate.bound_values.push_back(
      Int32Value(predicate_descriptor, predicate_value));
  request.relation_projection_view.present = true;
  request.relation_projection_view.marker =
      api::kEngineRelationProjectionViewMarkerV2;
  request.relation_projection_view.view_descriptor_uuid =
      descriptor.view_descriptor_uuid;
  request.relation_projection_view.view_descriptor_generation =
      descriptor.view_descriptor_generation;
  request.relation_projection_view.outputs =
      api::EngineRelationProjectionViewSemanticOutputs(descriptor);
  return request;
}

sblr::SblrDispatchResult DispatchRelationProjectionViewSelect(
    const api::EngineRequestContext& context,
    std::string view_uuid,
    std::string packed,
    std::string marker = api::kEngineRelationProjectionViewMarkerV1,
    std::string projection_count = "1",
    std::vector<sblr::SblrOperand> extra_operands = {}) {
  auto envelope = sblr::MakeSblrEnvelope(
      "dml.select_rows", "SBLR_DML_SELECT_ROWS",
      "SBLR-RELATION-PROJECTION-VIEW-SELECT-CONFORMANCE");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands = {
      {"text", "target_object_uuid", view_uuid},
      {"text", "target_object_kind", "view"},
      {"text", "source_uuid", std::move(view_uuid)},
      {"text", "source_kind", "view"},
      {"text", "result_projection", std::move(marker)},
      {"text", "projection_count", std::move(projection_count)},
      {"text", "projection_0", std::move(packed)}};
  for (auto& operand : extra_operands) {
    envelope.operands.push_back(std::move(operand));
  }
  sblr::SblrDispatchRequest request;
  request.context = context;
  request.envelope = std::move(envelope);
  return sblr::DispatchSblrOperation(std::move(request));
}

sblr::SblrDispatchResult DispatchUpdatableRelationProjectionViewDelete(
    const api::EngineRequestContext& context,
    std::string view_uuid,
    std::string predicate_column,
    std::string predicate_value,
    std::string packed,
    std::string marker = api::kEngineRelationProjectionViewMarkerV2,
    std::string projection_count = "1",
    std::string predicate_kind = "column_equals",
    std::string predicate_value_type = "int32",
    std::vector<sblr::SblrOperand> extra_operands = {}) {
  auto envelope = sblr::MakeSblrEnvelope(
      "dml.delete_rows", "SBLR_DML_DELETE_ROWS",
      "SBLR-UPDATABLE-RELATION-PROJECTION-VIEW-DELETE-CONFORMANCE");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.operands = {
      {"text", "target_object_uuid", view_uuid},
      {"text", "target_object_kind", "view"},
      {"text", "dml_surface_variant", std::move(marker)},
      {"text", "projection_count", std::move(projection_count)},
      {"text", "projection_0", std::move(packed)},
      {"text", "predicate_kind", std::move(predicate_kind)},
      {"text", "predicate_column", std::move(predicate_column)},
      {"text", "predicate_value", std::move(predicate_value)},
      {"text", "predicate_value_type", std::move(predicate_value_type)}};
  for (auto& operand : extra_operands) {
    envelope.operands.push_back(std::move(operand));
  }
  sblr::SblrDispatchRequest request;
  request.context = context;
  request.envelope = std::move(envelope);
  return sblr::DispatchSblrOperation(std::move(request));
}

void RequireRelationProjectionDispatchRejectedBeforeScan(
    const sblr::SblrDispatchResult& result,
    std::string_view message) {
  const bool failed_closed =
      !result.envelope_validated || !result.accepted ||
      (result.dispatched_to_api && !result.api_result.ok);
  Require(failed_closed && !result.api_result.ok &&
              result.api_result.result_shape.rows.empty() &&
              EvidenceValue(result.api_result,
                            "relation_projection_relation_scan")
                  .empty(),
          message);
}

void RequireUpdatableCreateDispatchRejectedBeforeMutation(
    const sblr::SblrDispatchResult& result,
    std::string_view message) {
  const bool failed_closed =
      !result.envelope_validated || !result.accepted ||
      (result.dispatched_to_api && !result.api_result.ok);
  Require(failed_closed && !result.api_result.ok &&
              result.api_result.primary_object.uuid.canonical.empty() &&
              EvidenceValue(result.api_result,
                            "relation_projection_view_marker")
                  .empty(),
          message);
}

void RequireUpdatableDeleteDispatchRejectedBeforeMutation(
    const sblr::SblrDispatchResult& result,
    std::string_view message) {
  const bool failed_closed =
      !result.envelope_validated || !result.accepted ||
      (result.dispatched_to_api && !result.api_result.ok);
  Require(failed_closed && !result.api_result.ok &&
              result.api_result.result_shape.rows.empty() &&
              EvidenceValue(result.api_result,
                            "relation_projection_view_delete_mga_authority")
                  .empty(),
          message);
}

void RequireRows(const api::EngineSelectRowsResult& result,
                 std::string_view second_name,
                 const std::vector<std::int32_t>& ids,
                 std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
  Require(result.result_shape.result_kind == "query_rowset" &&
              result.result_shape.columns.size() == 2 &&
              result.result_shape.rows.size() == ids.size() &&
              result.visible_count == ids.size(),
          message);
  Require(CanonicalObjectUuid(
              result.result_shape.columns[0].descriptor_uuid.canonical) &&
              CanonicalObjectUuid(
                  result.result_shape.columns[1].descriptor_uuid.canonical) &&
              result.result_shape.columns[0].descriptor_uuid.canonical !=
                  result.result_shape.columns[1].descriptor_uuid.canonical,
          "relation projection result type identities drifted");
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto& fields = result.result_shape.rows[i].fields;
    Require(fields.size() == 2 && fields[0].first == "ID" &&
                fields[1].first == second_name &&
                fields[0].second.encoded_value ==
                    std::to_string(ids[i]) &&
                fields[1].second.encoded_value == "5" &&
                !fields[0].second.isSqlNull() &&
                !fields[1].second.isSqlNull(),
            message);
  }
  Require(EvidenceValue(result, "relation_projection_relation_scan") ==
              "one_mga_visible_scan" &&
              EvidenceValue(result, "relation_projection_output_count") ==
                  "2" &&
              EvidenceValue(result, "relation_projection_row_storage") ==
                  "none" &&
              EvidenceValue(result, "relation_projection_view_parser_sql") ==
                  "false",
          "relation projection execution evidence drifted");
}

void RequireIdentityAuthority(
    const api::EngineRelationProjectionViewDescriptor& descriptor) {
  Require(descriptor.present && descriptor.outputs.size() == 2 &&
              descriptor.view_descriptor_generation == 1 &&
              descriptor.source_resource_epoch == 1,
          "relation projection durable descriptor missing");
  const std::set<std::string> identities = {
      descriptor.view_uuid.canonical,
      descriptor.view_descriptor_uuid.canonical,
      descriptor.source_relation_uuid.canonical,
      descriptor.source_relation_descriptor_uuid.canonical,
      descriptor.outputs[0].output_column_uuid.canonical,
      descriptor.outputs[0].expression_uuid.canonical,
      descriptor.outputs[0].output_type.type_descriptor_uuid.canonical,
      descriptor.outputs[0].source_column_uuid.canonical,
      descriptor.outputs[1].output_column_uuid.canonical,
      descriptor.outputs[1].expression_uuid.canonical,
      descriptor.outputs[1].output_type.type_descriptor_uuid.canonical};
  const auto& source_type_descriptor_uuid =
      descriptor.outputs[0].source_column_type_descriptor_uuid.canonical;
  Require(CanonicalObjectUuid(source_type_descriptor_uuid) &&
              (source_type_descriptor_uuid ==
                   descriptor.outputs[0].source_column_uuid.canonical ||
               !identities.contains(source_type_descriptor_uuid)) &&
              identities.size() == 11,
          "relation projection catalog identities collided");
  for (const auto& identity : identities) {
    Require(CanonicalObjectUuid(identity),
            "relation projection identity is not a canonical object UUID");
  }
  Require(descriptor.outputs[0].expression_kind ==
              api::EngineRelationProjectionExpressionKind::source_column &&
              descriptor.outputs[1].expression_kind ==
                  api::EngineRelationProjectionExpressionKind::
                      typed_int32_literal &&
              descriptor.outputs[1].literal_int32 == 5,
          "relation projection expression authority drifted");
}

bool SameTypeDescriptor(
    const api::EngineRelationProjectionTypeDescriptor& left,
    const api::EngineRelationProjectionTypeDescriptor& right) {
  return left.type_descriptor_uuid.canonical ==
             right.type_descriptor_uuid.canonical &&
         left.descriptor_kind == right.descriptor_kind &&
         left.canonical_type_name == right.canonical_type_name &&
         left.encoded_descriptor == right.encoded_descriptor;
}

bool SameOutput(
    const api::EngineRelationProjectionViewOutput& left,
    const api::EngineRelationProjectionViewOutput& right) {
  return left.ordinal == right.ordinal &&
         left.output_column_uuid.canonical ==
             right.output_column_uuid.canonical &&
         left.expression_uuid.canonical == right.expression_uuid.canonical &&
         left.output_name == right.output_name &&
         SameTypeDescriptor(left.output_type, right.output_type) &&
         left.nullable == right.nullable &&
         left.expression_kind == right.expression_kind &&
         left.source_column_uuid.canonical ==
             right.source_column_uuid.canonical &&
         left.source_column_type_descriptor_uuid.canonical ==
             right.source_column_type_descriptor_uuid.canonical &&
         left.literal_int32 == right.literal_int32;
}

bool SameAuthoritativeDescriptor(
    const api::EngineRelationProjectionViewDescriptor& left,
    const api::EngineRelationProjectionViewDescriptor& right) {
  if (left.present != right.present || left.marker != right.marker ||
      left.view_uuid.canonical != right.view_uuid.canonical ||
      left.view_descriptor_uuid.canonical !=
          right.view_descriptor_uuid.canonical ||
      left.view_descriptor_generation != right.view_descriptor_generation ||
      left.source_relation_uuid.canonical !=
          right.source_relation_uuid.canonical ||
      left.source_relation_descriptor_uuid.canonical !=
          right.source_relation_descriptor_uuid.canonical ||
      left.source_relation_descriptor_generation !=
          right.source_relation_descriptor_generation ||
      left.source_resource_epoch != right.source_resource_epoch ||
      left.outputs.size() != right.outputs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.outputs.size(); ++i) {
    if (!SameOutput(left.outputs[i], right.outputs[i])) return false;
  }
  return true;
}

void RequireUpdatableIdentityAuthority(
    const api::EngineRelationProjectionViewDescriptor& descriptor) {
  Require(descriptor.present &&
              descriptor.marker ==
                  api::kEngineRelationProjectionViewMarkerV2 &&
              descriptor.outputs.size() == 1 &&
              descriptor.view_descriptor_generation == 1 &&
              descriptor.source_resource_epoch == 1,
          "updatable relation projection durable descriptor missing");
  const auto& output = descriptor.outputs.front();
  const std::set<std::string> identities = {
      descriptor.view_uuid.canonical,
      descriptor.view_descriptor_uuid.canonical,
      descriptor.source_relation_uuid.canonical,
      descriptor.source_relation_descriptor_uuid.canonical,
      output.output_column_uuid.canonical,
      output.expression_uuid.canonical,
      output.output_type.type_descriptor_uuid.canonical,
      output.source_column_uuid.canonical};
  Require(CanonicalObjectUuid(
              output.source_column_type_descriptor_uuid.canonical) &&
              (output.source_column_type_descriptor_uuid.canonical ==
                   output.source_column_uuid.canonical ||
               !identities.contains(
                   output.source_column_type_descriptor_uuid.canonical)) &&
              identities.size() == 8,
          "updatable relation projection catalog identities collided");
  for (const auto& identity : identities) {
    Require(CanonicalObjectUuid(identity),
            "updatable relation projection identity is not canonical");
  }
  Require(output.ordinal == 0 && output.output_name == "ID" &&
              output.nullable &&
              output.expression_kind ==
                  api::EngineRelationProjectionExpressionKind::source_column,
          "updatable relation projection output authority drifted");
}

void SetOption(api::EngineApiRequest* request,
               std::string_view prefix,
               std::string value) {
  Require(request != nullptr, "relation projection option request required");
  for (auto& option : request->option_envelopes) {
    if (option.rfind(prefix, 0) == 0) {
      option = std::string(prefix) + std::move(value);
      return;
    }
  }
  Fail("relation projection option prefix not found");
}

void TestRelationProjectionView(Fixture& fixture) {
  auto metadata = Begin(fixture, 1);
  api::EngineCreateSchemaRequest schema;
  schema.context = metadata;
  schema.target_object.uuid.canonical = fixture.schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("view_schema"));
  RequireOk(api::EngineCreateSchema(schema),
            "relation projection schema create failed");
  PersistTable(metadata,
               Int32Table(metadata,
                          fixture.singleton_table_uuid,
                          "singleton_source"),
               &fixture.singleton_descriptor);
  PersistTable(metadata,
               // Firebird CREATE TABLE ... INT persists the exact descriptor
               // spelling `type=int`.  Exercise that compatibility-facing alias
               // through create, reload, and one-MGA-scan selection.
               Int32Table(metadata, fixture.multi_table_uuid, "tb", true),
               &fixture.multi_descriptor);
  Commit(metadata);

  auto initial_rows = Begin(fixture, 2);
  Insert(initial_rows,
         fixture.singleton_table_uuid,
         fixture.singleton_descriptor.columns[0].value_descriptor,
         {1});
  Insert(initial_rows,
         fixture.multi_table_uuid,
         fixture.multi_descriptor.columns[0].value_descriptor,
         {3, 10});
  Commit(initial_rows);

  auto create = Begin(fixture, 3);
  const auto singleton_created = api::EngineCreateView(
      ViewRequest(fixture,
                  create,
                  fixture.singleton_descriptor,
                  "V_TEST",
                  "X"));
  RequireOk(singleton_created,
            "exact test_02 relation projection view create failed");
  const auto multi_created = api::EngineCreateView(
      ViewRequest(fixture,
                  create,
                  fixture.multi_descriptor,
                  "TEST",
                  "NUM"));
  RequireOk(multi_created,
            "exact test_05 relation projection view create failed");
  Require(EvidenceValue(singleton_created,
                        "relation_projection_view_marker") ==
              api::kEngineRelationProjectionViewMarkerV1 &&
              EvidenceValue(singleton_created,
                            "relation_projection_view_parser_sql") ==
                  "false" &&
              EvidenceValue(singleton_created,
                            "relation_projection_view_row_storage") ==
                  "none_mga_source_scan_only",
          "relation projection create evidence drifted");

  const auto singleton_own_descriptor =
      api::DescribeEngineRelationProjectionView(
          create, singleton_created.primary_object.uuid.canonical);
  const auto multi_own_descriptor =
      api::DescribeEngineRelationProjectionView(
          create, multi_created.primary_object.uuid.canonical);
  Require(!singleton_own_descriptor.diagnostic.error &&
              !multi_own_descriptor.diagnostic.error,
          "creating transaction could not describe its views");
  RequireIdentityAuthority(singleton_own_descriptor);
  RequireIdentityAuthority(multi_own_descriptor);

  const auto singleton_own = api::EngineResolveName(
      ResolveRequest(fixture, create, "V_TEST"));
  const auto multi_own = api::EngineResolveName(
      ResolveRequest(fixture, create, "TEST"));
  RequireOk(singleton_own,
            "creating transaction could not resolve exact test_02 view");
  RequireOk(multi_own,
            "creating transaction could not resolve exact test_05 view");
  RequireRows(api::EngineSelectRows(SelectRequest(create, singleton_own)),
              "X",
              {1},
              "exact test_02 own-transaction SELECT failed");
  RequireRows(api::EngineSelectRows(SelectRequest(create, multi_own)),
              "NUM",
              {3, 10},
              "exact test_05 own-transaction SELECT failed");

  auto precommit_reader = Begin(fixture, 4);
  const auto precommit = api::EngineResolveName(
      ResolveRequest(fixture, precommit_reader, "TEST"));
  Require(!precommit.ok && !precommit.diagnostics.empty() &&
              precommit.diagnostics.front().code == "CATALOG.NAME.NOT_FOUND",
          "uncommitted relation projection view escaped MGA visibility");
  Rollback(precommit_reader);
  Commit(create);

  auto rollback_create = Begin(fixture, 5);
  const auto rollback_created = api::EngineCreateView(
      ViewRequest(fixture,
                  rollback_create,
                  fixture.multi_descriptor,
                  "V_ROLLBACK",
                  "NUM"));
  RequireOk(rollback_created,
            "relation projection rollback probe create failed");
  const auto rollback_own = api::DescribeEngineRelationProjectionView(
      rollback_create, rollback_created.primary_object.uuid.canonical);
  Require(!rollback_own.diagnostic.error && rollback_own.present,
          "rollback probe lacked own-transaction visibility");
  Rollback(rollback_create);
  auto rollback_reader = Begin(fixture, 6);
  const auto rollback_resolved = api::EngineResolveName(
      ResolveRequest(fixture, rollback_reader, "V_ROLLBACK"));
  Require(!rollback_resolved.ok &&
              !rollback_resolved.diagnostics.empty() &&
              rollback_resolved.diagnostics.front().code ==
                  "CATALOG.NAME.NOT_FOUND",
          "rolled-back relation projection view remained name-visible");
  const auto rollback_descriptor =
      api::DescribeEngineRelationProjectionView(
          rollback_reader, rollback_created.primary_object.uuid.canonical);
  Require(!rollback_descriptor.diagnostic.error &&
              !rollback_descriptor.present,
          "rolled-back relation projection descriptor remained visible");
  Rollback(rollback_reader);

  auto fresh = Begin(fixture, 7);
  const auto singleton = api::EngineResolveName(
      ResolveRequest(fixture, fresh, "V_TEST"));
  const auto multi = api::EngineResolveName(
      ResolveRequest(fixture, fresh, "TEST"));
  RequireOk(singleton, "committed exact test_02 view did not reload");
  RequireOk(multi, "committed exact test_05 view did not reload");
  Require(singleton.semantic_projection.ordered_outputs[0].output_name ==
              "ID" &&
              singleton.semantic_projection.ordered_outputs[1].output_name ==
                  "X" &&
              multi.semantic_projection.ordered_outputs[0].output_name ==
                  "ID" &&
              multi.semantic_projection.ordered_outputs[1].output_name ==
                  "NUM",
          "explicit relation projection output names did not round-trip");
  RequireRows(api::EngineSelectRows(SelectRequest(fresh, singleton)),
              "X",
              {1},
              "exact test_02 committed SELECT failed");
  RequireRows(api::EngineSelectRows(SelectRequest(fresh, multi)),
              "NUM",
              {3, 10},
              "exact test_05 committed SELECT failed");
  const auto reloaded = api::DescribeEngineRelationProjectionView(
      fresh, multi_created.primary_object.uuid.canonical);
  Require(!reloaded.diagnostic.error &&
              SameAuthoritativeDescriptor(reloaded, multi_own_descriptor),
          "relation projection persisted descriptor did not round-trip exactly");

  auto stale_semantic = SelectRequest(fresh, multi);
  stale_semantic.relation_projection_view.view_descriptor_generation += 1;
  const auto stale_semantic_result =
      api::EngineSelectRows(stale_semantic);
  Require(!stale_semantic_result.ok &&
              HasDiagnostic(stale_semantic_result,
                            "semantic_descriptor_stale") &&
              EvidenceValue(stale_semantic_result,
                            "relation_projection_relation_scan")
                  .empty(),
          "stale semantic descriptor reached the MGA scan");
  Rollback(fresh);

  auto stale_create = Begin(fixture, 8);
  auto stale_source = ViewRequest(fixture,
                                  stale_create,
                                  fixture.multi_descriptor,
                                  "V_STALE_SOURCE",
                                  "NUM");
  SetOption(&stale_source,
            "source_relation_descriptor_generation:",
            std::to_string(fixture.multi_descriptor.descriptor_generation +
                           1));
  const auto stale_source_result = api::EngineCreateView(stale_source);
  Require(!stale_source_result.ok &&
              HasDiagnostic(stale_source_result,
                            "source_descriptor_stale"),
          "stale source descriptor was accepted at CREATE");
  auto stale_epoch = ViewRequest(fixture,
                                 stale_create,
                                 fixture.multi_descriptor,
                                 "V_STALE_EPOCH",
                                 "NUM");
  SetOption(&stale_epoch, "source_resource_epoch:", "2");
  const auto stale_epoch_result = api::EngineCreateView(stale_epoch);
  Require(!stale_epoch_result.ok &&
              HasDiagnostic(stale_epoch_result,
                            "source_descriptor_stale"),
          "stale source resource epoch was accepted at CREATE");

  auto quoted_view = ViewRequest(fixture,
                                 stale_create,
                                 fixture.multi_descriptor,
                                 "V_QUOTED",
                                 "NUM");
  quoted_view.localized_names[0].was_quoted = true;
  quoted_view.localized_names[0].quote_style = "double_quote";
  const auto quoted_view_result = api::EngineCreateView(quoted_view);
  Require(!quoted_view_result.ok &&
              HasDiagnostic(quoted_view_result,
                            "name_invalid_or_quoted"),
          "quoted relation projection view identity was silently folded");
  auto quoted_output = ViewRequest(fixture,
                                   stale_create,
                                   fixture.multi_descriptor,
                                   "V_QUOTED_OUTPUT",
                                   "NUM");
  quoted_output.columns[1].names[0].was_quoted = true;
  quoted_output.columns[1].names[0].quote_style = "double_quote";
  const auto quoted_output_result = api::EngineCreateView(quoted_output);
  Require(!quoted_output_result.ok &&
              HasDiagnostic(quoted_output_result,
                            "output_name_invalid"),
          "quoted relation projection output identity was silently folded");
  Rollback(stale_create);

  auto sblr_context = Begin(fixture, 80);
  const std::string rpvc_source = PackedRelationProjectionViewCreate(
      fixture.multi_descriptor, 0, "ID");
  const std::string rpvc_literal = PackedRelationProjectionViewCreate(
      fixture.multi_descriptor, 1, "NUM");
  const auto sblr_created = api::EngineCreateView(
      ViewRequest(fixture,
                  sblr_context,
                  fixture.multi_descriptor,
                  "V_SBLR_RELATION",
                  "NUM"));
  Require(sblr_created.ok &&
              !sblr_created.primary_object.uuid.canonical.empty() &&
              EvidenceValue(sblr_created,
                            "relation_projection_view_marker") ==
                  api::kEngineRelationProjectionViewMarkerV1 &&
              EvidenceValue(sblr_created,
                            "relation_projection_view_parser_sql") ==
                  "false",
          "engine-owned relation projection create failed");
  const auto sblr_descriptor = api::DescribeEngineRelationProjectionView(
      sblr_context,
      sblr_created.primary_object.uuid.canonical);
  Require(!sblr_descriptor.diagnostic.error &&
              sblr_descriptor.source_relation_uuid.canonical ==
                  fixture.multi_descriptor.relation_uuid.canonical &&
              sblr_descriptor.source_relation_descriptor_uuid.canonical ==
                  fixture.multi_descriptor.descriptor_uuid.canonical &&
              sblr_descriptor.source_relation_descriptor_generation ==
                  fixture.multi_descriptor.descriptor_generation &&
              sblr_descriptor.source_resource_epoch ==
                  sblr_context.resource_epoch &&
              sblr_descriptor.outputs.size() == 2 &&
              sblr_descriptor.outputs[0].output_name == "ID" &&
              sblr_descriptor.outputs[1].output_name == "NUM" &&
              sblr_descriptor.outputs[1].literal_int32 == 5,
          "rpvc1 changed the durable descriptor contract");
  RequireIdentityAuthority(sblr_descriptor);

  const auto sblr_resolved = api::EngineResolveName(
      ResolveRequest(fixture, sblr_context, "V_SBLR_RELATION"));
  RequireOk(sblr_resolved, "rpvc1-created view did not resolve");
  const std::string rpvs =
      PackedRelationProjectionViewSelect(sblr_resolved);
  const auto sblr_selected =
      api::EngineSelectRows(SelectRequest(sblr_context, sblr_resolved));
  Require(sblr_selected.ok &&
              sblr_selected.result_shape.result_kind ==
                  "query_rowset" &&
              sblr_selected.result_shape.columns.size() == 2 &&
              sblr_selected.result_shape.rows.size() == 2 &&
              EvidenceValue(sblr_selected,
                            "relation_projection_relation_scan") ==
                  "one_mga_visible_scan" &&
              EvidenceValue(sblr_selected,
                            "relation_projection_view_marker") ==
                  api::kEngineRelationProjectionViewMarkerV1 &&
              EvidenceValue(sblr_selected,
                            "relation_projection_view_uuid") ==
                  sblr_descriptor.view_uuid.canonical &&
              EvidenceValue(sblr_selected,
                            "relation_projection_view_descriptor_uuid") ==
                  sblr_descriptor.view_descriptor_uuid.canonical &&
              EvidenceValue(sblr_selected,
                            "relation_projection_view_parser_sql") ==
                  "false",
          "engine-owned relation projection did not execute one exact scan");
  for (std::size_t i = 0; i < 2; ++i) {
    const auto& fields = sblr_selected.result_shape.rows[i].fields;
    Require(fields.size() == 2 && fields[0].first == "ID" &&
                fields[1].first == "NUM" &&
                fields[0].second.encoded_value ==
                    std::to_string(i == 0 ? 3 : 10) &&
                fields[1].second.encoded_value == "5",
            "rpvs1 result row contract drifted");
  }

  const auto visible_before_rejections = api::VisibleApiBehaviorRecords(
      sblr_context, "view", sblr_context.local_transaction_id);
  Require(!rpvc_source.empty() && rpvc_source.back() == '|',
          "rpvc1 malformed-count fixture drifted");
  RequireRelationProjectionDispatchRejectedBeforeScan(
      DispatchRelationProjectionViewCreate(
          fixture,
          sblr_context,
          fixture.multi_descriptor,
          "V_BAD_RPVC_SHORT",
          rpvc_source.substr(0, rpvc_source.size() - 1),
          rpvc_literal),
      "15-field rpvc1 was accepted");
  RequireRelationProjectionDispatchRejectedBeforeScan(
      DispatchRelationProjectionViewCreate(
          fixture,
          sblr_context,
          fixture.multi_descriptor,
          "V_BAD_RPVC_LONG",
          rpvc_source,
          rpvc_literal + "|00"),
      "17-field rpvc1 was accepted");
  RequireRelationProjectionDispatchRejectedBeforeScan(
      DispatchRelationProjectionViewCreate(
          fixture,
          sblr_context,
          fixture.multi_descriptor,
          "V_BAD_RPVC_VERSION",
          rpvc_source,
          rpvc_literal,
          "engine.relation_projection_view.v2"),
      "unknown relation projection CREATE marker was accepted");
  RequireRelationProjectionDispatchRejectedBeforeScan(
      DispatchRelationProjectionViewCreate(
          fixture,
          sblr_context,
          fixture.multi_descriptor,
          "V_BAD_RPVC_DUPLICATE",
          rpvc_source,
          rpvc_literal,
          api::kEngineRelationProjectionViewMarkerV1,
          "2",
          {{"text", "view_query_shape",
            api::kEngineRelationProjectionViewMarkerV1}}),
      "duplicate relation projection CREATE marker was accepted");
  RequireRelationProjectionDispatchRejectedBeforeScan(
      DispatchRelationProjectionViewCreate(
          fixture,
          sblr_context,
          fixture.multi_descriptor,
          "V_BAD_RPVC_ROW",
          rpvc_source,
          rpvc_literal,
          api::kEngineRelationProjectionViewMarkerV1,
          "2",
          {{"row_field:int32", "row-1|ID", "1"}}),
      "rpvc1 normalized away an injected row");
  const auto visible_after_create_rejections = api::VisibleApiBehaviorRecords(
      sblr_context, "view", sblr_context.local_transaction_id);
  Require(visible_after_create_rejections.size() ==
              visible_before_rejections.size(),
          "malformed rpvc1 mutated durable view state");

  RequireRelationProjectionDispatchRejectedBeforeScan(
      DispatchRelationProjectionViewSelect(
          sblr_context,
          sblr_descriptor.view_uuid.canonical,
          rpvs + "|00"),
      "rpvs1 with an extra field was accepted");
  RequireRelationProjectionDispatchRejectedBeforeScan(
      DispatchRelationProjectionViewSelect(
          sblr_context,
          sblr_descriptor.view_uuid.canonical,
          PackedRelationProjectionViewSelect(
              sblr_resolved,
              sblr_resolved.semantic_projection.descriptor_generation + 1)),
      "stale rpvs1 descriptor generation was accepted");
  RequireRelationProjectionDispatchRejectedBeforeScan(
      DispatchRelationProjectionViewSelect(
          sblr_context,
          sblr_descriptor.view_uuid.canonical,
          rpvs,
          "engine.relation_projection_view.v2"),
      "unknown relation projection SELECT marker was accepted");
  RequireRelationProjectionDispatchRejectedBeforeScan(
      DispatchRelationProjectionViewSelect(
          sblr_context,
          sblr_descriptor.view_uuid.canonical,
          rpvs,
          api::kEngineRelationProjectionViewMarkerV1,
          "1",
          {{"text", "result_projection",
            api::kEngineRelationProjectionViewMarkerV1}}),
      "duplicate relation projection SELECT marker was accepted");
  RequireRelationProjectionDispatchRejectedBeforeScan(
      DispatchRelationProjectionViewSelect(
          sblr_context,
          sblr_descriptor.view_uuid.canonical,
          rpvs,
          api::kEngineRelationProjectionViewMarkerV1,
          "1",
          {{"row_field:int32", "row-1|ID", "1"}}),
      "rpvs1 normalized away an injected row");
  Commit(sblr_context);

  auto metadata_owner = Begin(fixture, 90);
  auto pinned_metadata = metadata_owner;
  pinned_metadata.statement_metadata_snapshot_engine_owned = true;
  pinned_metadata.statement_metadata_snapshot_uuid.canonical =
      NewUuid(platform::UuidKind::object, NowMillis());
  pinned_metadata
      .statement_metadata_snapshot_visible_through_local_transaction_id =
      metadata_owner.snapshot_visible_through_local_transaction_id;
  pinned_metadata
      .statement_metadata_snapshot_active_excluded_local_transaction_ids = {
      metadata_owner.local_transaction_id};
  Require(pinned_metadata
                  .statement_metadata_snapshot_visible_through_local_transaction_id !=
              0,
          "engine-owned metadata snapshot boundary was not selected");
  auto metadata_writer = Begin(fixture, 91);
  const auto late_created = api::EngineCreateView(
      ViewRequest(fixture,
                  metadata_writer,
                  fixture.multi_descriptor,
                  "V_LATE_METADATA",
                  "NUM"));
  RequireOk(late_created,
            "late metadata snapshot probe view create failed");
  Commit(metadata_writer);
  const auto pinned_late_descriptor =
      api::DescribeEngineRelationProjectionView(
          pinned_metadata, late_created.primary_object.uuid.canonical);
  Require(!pinned_late_descriptor.diagnostic.error &&
              !pinned_late_descriptor.present,
          "engine-owned metadata snapshot observed a later committed descriptor");
  auto metadata_fresh = Begin(fixture, 92);
  const auto fresh_late = api::EngineResolveName(
      ResolveRequest(fixture, metadata_fresh, "V_LATE_METADATA"));
  RequireOk(fresh_late,
            "fresh transaction did not observe committed view metadata");
  const auto fresh_late_descriptor =
      api::DescribeEngineRelationProjectionView(
          metadata_fresh, late_created.primary_object.uuid.canonical);
  Require(!fresh_late_descriptor.diagnostic.error &&
              fresh_late_descriptor.present,
          "fresh transaction did not reload committed view descriptor");
  RequireRows(api::EngineSelectRows(
                  SelectRequest(metadata_fresh, fresh_late)),
              "NUM",
              {3, 10},
              "fresh metadata snapshot probe SELECT failed");
  Rollback(metadata_fresh);
  Rollback(metadata_owner);

  auto snapshot_reader = Begin(fixture, 9, "snapshot");
  const auto snapshot_view = api::EngineResolveName(
      ResolveRequest(fixture, snapshot_reader, "TEST"));
  RequireOk(snapshot_view, "snapshot reader could not resolve view");
  RequireRows(api::EngineSelectRows(
                  SelectRequest(snapshot_reader, snapshot_view)),
              "NUM",
              {3, 10},
              "snapshot baseline relation projection failed");

  auto committed_writer = Begin(fixture, 10);
  Insert(committed_writer,
         fixture.multi_table_uuid,
         fixture.multi_descriptor.columns[0].value_descriptor,
         {11});
  Commit(committed_writer);
  RequireRows(api::EngineSelectRows(
                  SelectRequest(snapshot_reader, snapshot_view)),
              "NUM",
              {3, 10},
              "stable snapshot observed a later committed source row");

  auto rollback_writer = Begin(fixture, 11);
  Insert(rollback_writer,
         fixture.multi_table_uuid,
         fixture.multi_descriptor.columns[0].value_descriptor,
         {99});
  const auto rollback_writer_view = api::EngineResolveName(
      ResolveRequest(fixture, rollback_writer, "TEST"));
  RequireOk(rollback_writer_view,
            "rollback writer could not resolve relation projection view");
  RequireRows(api::EngineSelectRows(
                  SelectRequest(rollback_writer, rollback_writer_view)),
              "NUM",
              {3, 10, 11, 99},
              "relation projection lost own source-row visibility");
  Rollback(rollback_writer);
  Rollback(snapshot_reader);

  auto final_reader = Begin(fixture, 12);
  const auto final_view = api::EngineResolveName(
      ResolveRequest(fixture, final_reader, "TEST"));
  RequireOk(final_view, "final relation projection view resolve failed");
  RequireRows(api::EngineSelectRows(
                  SelectRequest(final_reader, final_view)),
              "NUM",
              {3, 10, 11},
              "fresh reader visibility/rollback filtering failed");

  const auto durable = api::DescribeEngineRelationProjectionView(
      final_reader, final_view.bound_object_identity.object_uuid.canonical);
  Require(!durable.diagnostic.error && durable.present,
          "durable descriptor required for stale runtime probe");

  const auto persisted_record = api::FindVisibleApiBehaviorRecord(
      final_reader,
      durable.view_uuid.canonical,
      final_reader.local_transaction_id);
  Require(persisted_record.has_value(),
          "persisted relation projection behavior record required");
  const auto persist_stale_variant =
      [&](std::string_view suffix,
          const std::string& expected_field,
          const std::string& replacement_field) {
        auto record = *persisted_record;
        const std::string original_uuid = record.object_uuid;
        record.creator_tx = final_reader.local_transaction_id;
        record.object_uuid = NewUuid(platform::UuidKind::object, NowMillis());
        record.default_name = "V_STALE_" + std::string(suffix);
        ReplaceOnce(&record.payload,
                    "target=" + original_uuid,
                    "target=" + record.object_uuid);
        ReplaceOnce(&record.payload, expected_field, replacement_field);
        Require(!api::AppendApiBehaviorEvent(
                     final_reader,
                     api::MakeApiBehaviorRecordEvent(record))
                     .error,
                "stale persisted descriptor variant append failed");
        const auto described = api::DescribeEngineRelationProjectionView(
            final_reader, record.object_uuid);
        Require(!described.diagnostic.error && described.present,
                "stale persisted descriptor variant did not decode");
        return described;
      };

  const std::string generation_prefix =
      "source_relation_descriptor_generation:";
  const auto stale_persisted_generation = persist_stale_variant(
      "GENERATION",
      generation_prefix +
          std::to_string(durable.source_relation_descriptor_generation),
      generation_prefix +
          std::to_string(durable.source_relation_descriptor_generation + 1));
  const auto stale_persisted_generation_result = api::EngineSelectRows(
      SelectRequest(final_reader, stale_persisted_generation));
  Require(!stale_persisted_generation_result.ok &&
              HasDiagnostic(stale_persisted_generation_result,
                            "relation_projection_view_source_descriptor_stale") &&
              EvidenceValue(stale_persisted_generation_result,
                            "relation_projection_relation_scan")
                  .empty(),
          "stale persisted source generation reached the MGA scan");

  const std::string resource_epoch_prefix = "source_resource_epoch:";
  const auto stale_persisted_epoch = persist_stale_variant(
      "RESOURCE_EPOCH",
      resource_epoch_prefix + std::to_string(durable.source_resource_epoch),
      resource_epoch_prefix +
          std::to_string(durable.source_resource_epoch + 1));
  const auto stale_persisted_epoch_result = api::EngineSelectRows(
      SelectRequest(final_reader, stale_persisted_epoch));
  Require(!stale_persisted_epoch_result.ok &&
              HasDiagnostic(stale_persisted_epoch_result,
                            "relation_projection_view_source_descriptor_stale") &&
              EvidenceValue(stale_persisted_epoch_result,
                            "relation_projection_relation_scan")
                  .empty(),
          "stale persisted source resource epoch reached the MGA scan");

  api::EngineSelectRowsRequest direct;
  direct.context = final_reader;
  direct.source_object.uuid = durable.source_relation_uuid;
  direct.source_object.object_kind = "table";
  direct.relation_projection.relation_uuid =
      durable.source_relation_uuid;
  direct.relation_projection.relation_descriptor_uuid =
      durable.source_relation_descriptor_uuid;
  direct.relation_projection.relation_descriptor_generation =
      durable.source_relation_descriptor_generation + 1;
  direct.relation_projection.source_resource_epoch =
      durable.source_resource_epoch;
  direct.relation_projection.outputs = durable.outputs;
  const auto stale_runtime_result = api::EngineSelectRows(direct);
  Require(!stale_runtime_result.ok &&
              HasDiagnostic(stale_runtime_result,
                            "source_descriptor_stale") &&
              EvidenceValue(stale_runtime_result,
                            "relation_projection_relation_scan")
                  .empty(),
          "stale runtime source descriptor reached the MGA scan");

  direct.relation_projection.relation_descriptor_generation =
      durable.source_relation_descriptor_generation;
  direct.context.resource_epoch = durable.source_resource_epoch + 1;
  const auto stale_runtime_epoch = api::EngineSelectRows(direct);
  Require(!stale_runtime_epoch.ok &&
              HasDiagnostic(stale_runtime_epoch,
                            "source_resource_epoch_stale") &&
              EvidenceValue(stale_runtime_epoch,
                            "relation_projection_relation_scan")
                  .empty(),
          "stale runtime resource epoch reached the MGA scan");

  direct.context.resource_epoch = durable.source_resource_epoch;
  direct.relation_projection.outputs[1].expression_uuid =
      direct.relation_projection.outputs[0].expression_uuid;
  const auto identity_collision = api::EngineSelectRows(direct);
  Require(!identity_collision.ok &&
              HasDiagnostic(identity_collision,
                            "relation_projection_identity_collision") &&
              EvidenceValue(identity_collision,
                            "relation_projection_relation_scan")
                  .empty(),
          "colliding relation projection identities reached the MGA scan");

  const std::string malformed_view_uuid =
      NewUuid(platform::UuidKind::object, NowMillis());
  api::ApiBehaviorRecord malformed_record;
  malformed_record.creator_tx = final_reader.local_transaction_id;
  malformed_record.operation_id = "ddl.create_view";
  malformed_record.object_uuid = malformed_view_uuid;
  malformed_record.object_kind = "view";
  malformed_record.default_name = "V_MALFORMED";
  malformed_record.state = "created";
  malformed_record.payload =
      "schema=" + fixture.schema_uuid + ";target=" + malformed_view_uuid +
      ";options=view_query_shape:" +
      api::kEngineRelationProjectionViewMarkerV1 +
      ";view_descriptor_uuid:not-a-canonical-uuid";
  Require(!api::AppendApiBehaviorEvent(
               final_reader,
               api::MakeApiBehaviorRecordEvent(malformed_record))
               .error,
          "malformed relation projection descriptor append failed");
  const auto malformed = api::DescribeEngineRelationProjectionView(
      final_reader, malformed_view_uuid);
  Require(malformed.diagnostic.error && !malformed.present &&
              malformed.diagnostic.detail.find(
                  "relation_projection_view_descriptor_invalid") !=
                  std::string::npos,
          "malformed persisted relation projection codec was accepted");
  Rollback(final_reader);
}

void TestUpdatableRelationProjectionView(Fixture& fixture) {
  auto metadata = Begin(fixture, 200);
  PersistTable(metadata,
               Int32Table(metadata,
                          fixture.updatable_table_uuid,
                          "tb_delete_03",
                          true),
               &fixture.updatable_descriptor);
  Commit(metadata);

  auto initial_rows = Begin(fixture, 201);
  InsertDelete03Rows(initial_rows,
                     fixture.updatable_table_uuid,
                     fixture.updatable_descriptor.columns[0]
                         .value_descriptor);
  Commit(initial_rows);

  auto create = Begin(fixture, 202);
  const std::string rpvc2 = PackedUpdatableRelationProjectionViewCreate(
      fixture.updatable_descriptor);
  Require(!rpvc2.empty() && rpvc2.back() == '|',
          "rpvc2 malformed-count fixture drifted");
  const auto visible_before_create_refusals =
      api::VisibleApiBehaviorRecords(
          create, "view", create.local_transaction_id);
  std::set<std::string> visible_view_ids_before;
  for (const auto& record : visible_before_create_refusals) {
    visible_view_ids_before.insert(record.object_uuid);
  }
  RequireUpdatableCreateDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewCreate(
          fixture,
          create,
          fixture.updatable_descriptor,
          "V2_BAD_RPVC_SHORT",
          rpvc2.substr(0, rpvc2.size() - 1)),
      "15-field rpvc2 mutated durable view state");
  RequireUpdatableCreateDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewCreate(
          fixture,
          create,
          fixture.updatable_descriptor,
          "V2_BAD_RPVC_LONG",
          rpvc2 + "|00"),
      "17-field rpvc2 mutated durable view state");
  RequireUpdatableCreateDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewCreate(
          fixture,
          create,
          fixture.updatable_descriptor,
          "V2_BAD_ROOT_MARKER",
          rpvc2,
          "engine.relation_projection_view.v3"),
      "wrong root rpvc2 marker mutated durable view state");
  RequireUpdatableCreateDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewCreate(
          fixture,
          create,
          fixture.updatable_descriptor,
          "V2_BAD_ROOT_COUNT",
          rpvc2,
          api::kEngineRelationProjectionViewMarkerV2,
          "2"),
      "wrong root rpvc2 count mutated durable view state");
  RequireUpdatableCreateDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewCreate(
          fixture,
          create,
          fixture.updatable_descriptor,
          "V2_BAD_DUPLICATE_PROJECTION",
          rpvc2,
          api::kEngineRelationProjectionViewMarkerV2,
          "1",
          {{"text", "view_projection_0", rpvc2}}),
      "duplicate root rpvc2 projection mutated durable view state");
  RequireUpdatableCreateDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewCreate(
          fixture,
          create,
          fixture.updatable_descriptor,
          "V2_BAD_DUPLICATE_MARKER",
          rpvc2,
          api::kEngineRelationProjectionViewMarkerV2,
          "1",
          {{"text", "view_query_shape",
            api::kEngineRelationProjectionViewMarkerV2}}),
      "duplicate root rpvc2 marker mutated durable view state");
  RequireUpdatableCreateDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewCreate(
          fixture,
          create,
          fixture.updatable_descriptor,
          "V2_BAD_STALE_GENERATION",
          PackedUpdatableRelationProjectionViewCreate(
              fixture.updatable_descriptor,
              "ID",
              fixture.updatable_descriptor.descriptor_generation + 1)),
      "stale rpvc2 source generation mutated durable view state");
  RequireUpdatableCreateDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewCreate(
          fixture,
          create,
          fixture.updatable_descriptor,
          "V2_BAD_STALE_RESOURCE_EPOCH",
          PackedUpdatableRelationProjectionViewCreate(
              fixture.updatable_descriptor, "ID", 0, 2)),
      "stale rpvc2 resource epoch mutated durable view state");
  RequireUpdatableCreateDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewCreate(
          fixture,
          create,
          fixture.updatable_descriptor,
          "V2_BAD_INJECTED_ROW",
          rpvc2,
          api::kEngineRelationProjectionViewMarkerV2,
          "1",
          {{"row_field:int32", "row-1|ID", "10"}}),
      "rpvc2 normalized away an injected row");
  const auto visible_after_create_refusals =
      api::VisibleApiBehaviorRecords(
          create, "view", create.local_transaction_id);
  std::set<std::string> visible_view_ids_after;
  for (const auto& record : visible_after_create_refusals) {
    visible_view_ids_after.insert(record.object_uuid);
  }
  Require(visible_after_create_refusals.size() ==
              visible_before_create_refusals.size() &&
              visible_view_ids_after == visible_view_ids_before,
          "refused rpvc2 packets changed durable view inventory");

  const auto created = api::EngineCreateView(
      UpdatableViewRequest(fixture,
                           create,
                           fixture.updatable_descriptor,
                           "TEST_DELETE_03"));
  Require(created.ok &&
              !created.primary_object.uuid.canonical.empty() &&
              EvidenceValue(created,
                            "relation_projection_view_marker") ==
                  api::kEngineRelationProjectionViewMarkerV2 &&
              EvidenceValue(created,
                            "relation_projection_view_parser_sql") ==
                  "false",
          "engine-owned one-column updatable view create failed");
  const std::string view_uuid = created.primary_object.uuid.canonical;
  const auto own_descriptor =
      api::DescribeEngineRelationProjectionView(create, view_uuid);
  Require(!own_descriptor.diagnostic.error && own_descriptor.present &&
              own_descriptor.source_relation_uuid.canonical ==
                  fixture.updatable_table_uuid &&
              own_descriptor.source_relation_descriptor_uuid.canonical ==
                  fixture.updatable_descriptor.descriptor_uuid.canonical &&
              own_descriptor.source_relation_descriptor_generation ==
                  fixture.updatable_descriptor.descriptor_generation,
          "rpvc2 own-transaction descriptor was not exact");
  RequireUpdatableIdentityAuthority(own_descriptor);

  auto precommit_reader = Begin(fixture, 203);
  const auto precommit = api::EngineResolveName(
      ResolveRequest(fixture, precommit_reader, "TEST_DELETE_03"));
  Require(!precommit.ok && !precommit.diagnostics.empty() &&
              precommit.diagnostics.front().code ==
                  "CATALOG.NAME.NOT_FOUND",
          "uncommitted rpvc2 view escaped MGA catalog visibility");
  Rollback(precommit_reader);
  Commit(create);

  auto rollback_create = Begin(fixture, 204);
  const auto rolled_back_create = api::EngineCreateView(
      UpdatableViewRequest(fixture,
                           rollback_create,
                           fixture.updatable_descriptor,
                           "V2_ROLLBACK"));
  RequireOk(rolled_back_create,
            "updatable relation projection rollback CREATE failed");
  const auto rollback_own = api::DescribeEngineRelationProjectionView(
      rollback_create,
      rolled_back_create.primary_object.uuid.canonical);
  Require(!rollback_own.diagnostic.error && rollback_own.present,
          "updatable rollback CREATE lacked own visibility");
  Rollback(rollback_create);
  auto rollback_create_reader = Begin(fixture, 205);
  const auto rollback_name = api::EngineResolveName(
      ResolveRequest(fixture, rollback_create_reader, "V2_ROLLBACK"));
  Require(!rollback_name.ok && !rollback_name.diagnostics.empty() &&
              rollback_name.diagnostics.front().code ==
                  "CATALOG.NAME.NOT_FOUND",
          "rolled-back updatable view remained name-visible");
  const auto rollback_descriptor =
      api::DescribeEngineRelationProjectionView(
          rollback_create_reader,
          rolled_back_create.primary_object.uuid.canonical);
  Require(!rollback_descriptor.diagnostic.error &&
              !rollback_descriptor.present,
          "rolled-back updatable view descriptor remained visible");
  Rollback(rollback_create_reader);

  auto durable_reader = Begin(fixture, 206);
  const auto resolved = api::EngineResolveName(
      ResolveRequest(fixture, durable_reader, "TEST_DELETE_03"));
  RequireOk(resolved, "committed rpvc2 view did not resolve");
  Require(resolved.bound_object_identity.object_uuid.canonical == view_uuid &&
              resolved.semantic_projection.present &&
              resolved.semantic_projection.marker ==
                  api::kEngineRelationProjectionViewMarkerV2 &&
              resolved.semantic_projection.ordered_outputs.size() == 1 &&
              resolved.semantic_projection.ordered_outputs[0].ordinal == 0 &&
              resolved.semantic_projection.ordered_outputs[0].output_name ==
                  "ID" &&
              resolved.semantic_projection.ordered_outputs[0].nullable,
          "rpvd2 public semantic descriptor did not round-trip exactly");
  const auto reloaded = api::DescribeEngineRelationProjectionView(
      durable_reader, view_uuid);
  Require(!reloaded.diagnostic.error &&
              SameAuthoritativeDescriptor(reloaded, own_descriptor),
          "V2 updatable descriptor did not persist exactly");
  RequireUpdatableIdentityAuthority(reloaded);
  const std::string resolved_rpvd2 =
      PackedUpdatableRelationProjectionViewDelete(reloaded);
  Require(resolved_rpvd2.rfind("rpvd2|", 0) == 0 &&
              resolved_rpvd2.find(
                  Hex(reloaded.source_relation_uuid.canonical)) ==
                  std::string::npos &&
              resolved_rpvd2.find(
                  Hex(reloaded.source_relation_descriptor_uuid.canonical)) ==
                  std::string::npos &&
              resolved_rpvd2.find(
                  Hex(reloaded.outputs[0].source_column_uuid.canonical)) ==
                  std::string::npos,
          "rpvd2 public semantics leaked hidden source identities");
  RequireDelete03VisibleRows(durable_reader,
                             fixture.updatable_table_uuid,
                             2,
                             1,
                             "delete-03 baseline rows drifted");
  Rollback(durable_reader);

  // Keep one stable snapshot open across a rolled-back delete and a later
  // committed delete.  The parser packet never owns visibility or finality.
  auto snapshot_reader = Begin(fixture, 207, "snapshot");
  RequireDelete03VisibleRows(snapshot_reader,
                             fixture.updatable_table_uuid,
                             2,
                             1,
                             "stable snapshot baseline drifted");

  auto rollback_writer = Begin(fixture, 208);
  const auto rollback_writer_descriptor =
      api::DescribeEngineRelationProjectionView(rollback_writer, view_uuid);
  Require(!rollback_writer_descriptor.diagnostic.error &&
              rollback_writer_descriptor.present,
          "rollback writer could not load V2 descriptor");
  const auto rolled_back_delete = api::EngineDeleteRows(
      UpdatableDeleteRequest(rollback_writer,
                             rollback_writer_descriptor));
  RequireOk(rolled_back_delete, "rollback view DELETE failed");
  Require(rolled_back_delete.matched_count == 2 &&
              rolled_back_delete.deleted_count == 2 &&
              EvidenceValue(rolled_back_delete,
                            "relation_projection_view_delete_expansion") ==
                  "engine_owned_sql_free" &&
              EvidenceValue(rolled_back_delete,
                            "relation_projection_view_delete_mga_authority") ==
                  "ordinary_optimized_delete" &&
              EvidenceValue(rolled_back_delete,
                            "relation_projection_view_source_relation_uuid") ==
                  fixture.updatable_table_uuid &&
              EvidenceValue(rolled_back_delete,
                            "relation_projection_view_parser_sql") ==
                  "false",
          "V2 DELETE did not map exactly two rows through engine MGA");
  RequireDelete03VisibleRows(rollback_writer,
                             fixture.updatable_table_uuid,
                             0,
                             1,
                             "delete writer lost own-delete visibility or NULL");
  RequireDelete03VisibleRows(snapshot_reader,
                             fixture.updatable_table_uuid,
                             2,
                             1,
                             "stable snapshot observed an uncommitted delete");
  Rollback(rollback_writer);
  auto rollback_verifier = Begin(fixture, 209);
  RequireDelete03VisibleRows(rollback_verifier,
                             fixture.updatable_table_uuid,
                             2,
                             1,
                             "rolled-back view DELETE remained visible");
  Rollback(rollback_verifier);

  auto refusal_writer = Begin(fixture, 210);
  const auto refusal_descriptor =
      api::DescribeEngineRelationProjectionView(refusal_writer, view_uuid);
  Require(!refusal_descriptor.diagnostic.error &&
              refusal_descriptor.present,
          "V2 refusal descriptor required");
  const auto require_direct_refusal =
      [&](api::EngineDeleteRowsRequest request,
          std::string_view expected_detail,
          std::string_view message) {
        const auto before = ReadDelete03VisibleRows(
            refusal_writer, fixture.updatable_table_uuid);
        const auto result = api::EngineDeleteRows(request);
        const auto after = ReadDelete03VisibleRows(
            refusal_writer, fixture.updatable_table_uuid);
        Require(!result.ok && HasDiagnostic(result, expected_detail) &&
                    before.row_count == after.row_count &&
                    before.ten_count == after.ten_count &&
                    before.null_count == after.null_count &&
                    before.other_count == after.other_count &&
                    EvidenceValue(
                        result,
                        "relation_projection_view_delete_mga_authority")
                        .empty(),
                message);
      };

  auto wrong_transaction =
      UpdatableDeleteRequest(refusal_writer, refusal_descriptor);
  wrong_transaction.context.transaction_uuid =
      snapshot_reader.transaction_uuid;
  require_direct_refusal(
      std::move(wrong_transaction),
      "exact_active_write_transaction_required",
      "mismatched transaction identity reached V2 mutation");

  auto stale_generation =
      UpdatableDeleteRequest(refusal_writer, refusal_descriptor);
  ++stale_generation.relation_projection_view.view_descriptor_generation;
  require_direct_refusal(
      std::move(stale_generation),
      "relation_projection_view_delete_semantic_descriptor_stale",
      "stale V2 semantic generation reached mutation");

  auto wrong_output =
      UpdatableDeleteRequest(refusal_writer, refusal_descriptor);
  wrong_output.relation_projection_view.outputs[0].output_name = "OTHER";
  wrong_output.delete_predicate.canonical_predicate_envelope = "OTHER";
  require_direct_refusal(
      std::move(wrong_output),
      "relation_projection_view_delete_semantic_descriptor_stale",
      "wrong V2 semantic output reached mutation");

  auto wrong_type =
      UpdatableDeleteRequest(refusal_writer, refusal_descriptor);
  wrong_type.relation_projection_view.outputs[0]
      .output_type.canonical_type_name = "int64";
  require_direct_refusal(
      std::move(wrong_type),
      "relation_projection_view_delete_semantic_descriptor_stale",
      "wrong V2 semantic output type reached mutation");

  auto wrong_predicate =
      UpdatableDeleteRequest(refusal_writer, refusal_descriptor);
  wrong_predicate.delete_predicate.canonical_predicate_envelope = "OTHER";
  require_direct_refusal(
      std::move(wrong_predicate),
      "relation_projection_view_delete_semantic_descriptor_stale",
      "unmapped V2 predicate output reached mutation");

  auto wrong_predicate_type =
      UpdatableDeleteRequest(refusal_writer, refusal_descriptor);
  wrong_predicate_type.delete_predicate.bound_values[0]
      .descriptor.descriptor_uuid =
      refusal_descriptor.outputs[0].output_type.type_descriptor_uuid;
  require_direct_refusal(
      std::move(wrong_predicate_type),
      "relation_projection_view_delete_predicate_invalid",
      "UUID-bearing V2 predicate literal reached mutation");

  auto wrong_marker =
      UpdatableDeleteRequest(refusal_writer, refusal_descriptor);
  wrong_marker.relation_projection_view.marker =
      api::kEngineRelationProjectionViewMarkerV1;
  wrong_marker.delete_surface_variant =
      api::kEngineRelationProjectionViewMarkerV1;
  require_direct_refusal(
      std::move(wrong_marker),
      "relation_projection_view_delete_shape_invalid",
      "V1 marker was admitted as an updatable V2 delete");

  const auto v1_resolved = api::EngineResolveName(
      ResolveRequest(fixture, refusal_writer, "TEST"));
  RequireOk(v1_resolved, "non-updatable V1 refusal view did not resolve");
  const auto v1_descriptor = api::DescribeEngineRelationProjectionView(
      refusal_writer,
      v1_resolved.bound_object_identity.object_uuid.canonical);
  Require(!v1_descriptor.diagnostic.error && v1_descriptor.present &&
              v1_descriptor.marker ==
                  api::kEngineRelationProjectionViewMarkerV1,
          "non-updatable V1 refusal descriptor drifted");
  const auto v1_row_count_before =
      VisibleRowCount(refusal_writer, fixture.multi_table_uuid);
  api::EngineDeleteRowsRequest non_updatable;
  non_updatable.context = refusal_writer;
  non_updatable.target_table.uuid = v1_descriptor.view_uuid;
  non_updatable.target_table.object_kind = "view";
  non_updatable.delete_surface_variant =
      api::kEngineRelationProjectionViewMarkerV2;
  non_updatable.delete_predicate.predicate_kind = "column_equals";
  non_updatable.delete_predicate.canonical_predicate_envelope = "ID";
  api::EngineDescriptor int32_predicate;
  int32_predicate.descriptor_kind = "scalar";
  int32_predicate.canonical_type_name = "int32";
  int32_predicate.encoded_descriptor = "type=int32";
  non_updatable.delete_predicate.bound_values.push_back(
      Int32Value(int32_predicate, 10));
  non_updatable.relation_projection_view.present = true;
  non_updatable.relation_projection_view.marker =
      api::kEngineRelationProjectionViewMarkerV2;
  non_updatable.relation_projection_view.view_descriptor_uuid =
      v1_descriptor.view_descriptor_uuid;
  non_updatable.relation_projection_view.view_descriptor_generation =
      v1_descriptor.view_descriptor_generation;
  non_updatable.relation_projection_view.outputs = {
      api::EngineRelationProjectionViewSemanticOutputs(v1_descriptor).front()};
  const auto non_updatable_result =
      api::EngineDeleteRows(non_updatable);
  Require(!non_updatable_result.ok &&
              (HasDiagnostic(
                   non_updatable_result,
                   "relation_projection_view_delete_descriptor_required") ||
               HasDiagnostic(
                   non_updatable_result,
                   "relation_projection_view_delete_semantic_descriptor_stale")) &&
              VisibleRowCount(refusal_writer,
                              fixture.multi_table_uuid) ==
                  v1_row_count_before,
          "non-updatable V1 view mutated its source table");

  const auto persisted_record = api::FindVisibleApiBehaviorRecord(
      refusal_writer, view_uuid, refusal_writer.local_transaction_id);
  Require(persisted_record.has_value(),
          "V2 persisted descriptor record required for corruption refusal");

  auto stale_source_record = *persisted_record;
  stale_source_record.creator_tx = refusal_writer.local_transaction_id;
  stale_source_record.object_uuid =
      NewUuid(platform::UuidKind::object, NowMillis());
  stale_source_record.default_name = "V2_STALE_SOURCE";
  ReplaceOnce(&stale_source_record.payload,
              "target=" + view_uuid,
              "target=" + stale_source_record.object_uuid);
  ReplaceOnce(
      &stale_source_record.payload,
      "source_relation_descriptor_generation:" +
          std::to_string(
              refusal_descriptor.source_relation_descriptor_generation),
      "source_relation_descriptor_generation:" +
          std::to_string(
              refusal_descriptor.source_relation_descriptor_generation + 1));
  Require(!api::AppendApiBehaviorEvent(
               refusal_writer,
               api::MakeApiBehaviorRecordEvent(stale_source_record))
               .error,
          "stale-source V2 descriptor append failed");
  const auto stale_source_descriptor =
      api::DescribeEngineRelationProjectionView(
          refusal_writer, stale_source_record.object_uuid);
  Require(!stale_source_descriptor.diagnostic.error &&
              stale_source_descriptor.present,
          "stale-source V2 descriptor did not decode");
  require_direct_refusal(
      UpdatableDeleteRequest(refusal_writer, stale_source_descriptor),
      "relation_projection_view_delete_source_descriptor_stale",
      "stale durable V2 source descriptor reached mutation");

  auto malformed_record = *persisted_record;
  malformed_record.creator_tx = refusal_writer.local_transaction_id;
  malformed_record.object_uuid =
      NewUuid(platform::UuidKind::object, NowMillis());
  malformed_record.default_name = "V2_MALFORMED";
  ReplaceOnce(&malformed_record.payload,
              "target=" + view_uuid,
              "target=" + malformed_record.object_uuid);
  ReplaceOnce(&malformed_record.payload,
              "output_count:1",
              "output_count:2");
  Require(!api::AppendApiBehaviorEvent(
               refusal_writer,
               api::MakeApiBehaviorRecordEvent(malformed_record))
               .error,
          "malformed V2 persisted descriptor append failed");
  const auto malformed_descriptor =
      api::DescribeEngineRelationProjectionView(
          refusal_writer, malformed_record.object_uuid);
  Require(malformed_descriptor.diagnostic.error &&
              !malformed_descriptor.present &&
              malformed_descriptor.diagnostic.detail.find(
                  "relation_projection_view_descriptor_invalid") !=
                  std::string::npos,
          "malformed V2 persisted descriptor decoded");
  auto malformed_delete =
      UpdatableDeleteRequest(refusal_writer, refusal_descriptor);
  malformed_delete.target_table.uuid.canonical =
      malformed_record.object_uuid;
  require_direct_refusal(
      std::move(malformed_delete),
      "relation_projection_view_descriptor_invalid",
      "malformed persisted V2 descriptor reached mutation");

  const auto before_packet_refusals = ReadDelete03VisibleRows(
      refusal_writer, fixture.updatable_table_uuid);
  const auto final_separator = resolved_rpvd2.rfind('|');
  Require(final_separator != std::string::npos,
          "rpvd2 short-packet fixture drifted");
  RequireUpdatableDeleteDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewDelete(
          refusal_writer,
          view_uuid,
          "ID",
          "10",
          resolved_rpvd2.substr(0, final_separator)),
      "12-field rpvd2 reached mutation");
  RequireUpdatableDeleteDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewDelete(
          refusal_writer,
          view_uuid,
          "ID",
          "10",
          resolved_rpvd2 + "|00"),
      "14-field rpvd2 reached mutation");
  RequireUpdatableDeleteDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewDelete(
          refusal_writer,
          view_uuid,
          "ID",
          "10",
          PackedUpdatableRelationProjectionViewDelete(
              refusal_descriptor,
              refusal_descriptor.view_descriptor_generation + 1)),
      "stale rpvd2 generation reached mutation");
  RequireUpdatableDeleteDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewDelete(
          refusal_writer,
          view_uuid,
          "ID",
          "10",
          PackedUpdatableRelationProjectionViewDelete(
              refusal_descriptor, 0, "OTHER")),
      "wrong rpvd2 output reached mutation");
  RequireUpdatableDeleteDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewDelete(
          refusal_writer,
          view_uuid,
          "ID",
          "10",
          resolved_rpvd2,
          api::kEngineRelationProjectionViewMarkerV2,
          "2"),
      "wrong root rpvd2 projection count reached mutation");
  RequireUpdatableDeleteDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewDelete(
          refusal_writer,
          view_uuid,
          "ID",
          "10",
          resolved_rpvd2,
          api::kEngineRelationProjectionViewMarkerV2,
          "1",
          "column_equals",
          "int32",
          {{"text", "projection_0", resolved_rpvd2}}),
      "duplicate root rpvd2 projection reached mutation");
  RequireUpdatableDeleteDispatchRejectedBeforeMutation(
      DispatchUpdatableRelationProjectionViewDelete(
          refusal_writer,
          view_uuid,
          "ID",
          "10",
          resolved_rpvd2,
          api::kEngineRelationProjectionViewMarkerV2,
          "1",
          "column_equals",
          "int32",
          {{"row_field:int32", "row-1|ID", "10"}}),
      "rpvd2 normalized away an injected row");
  const auto after_packet_refusals = ReadDelete03VisibleRows(
      refusal_writer, fixture.updatable_table_uuid);
  Require(before_packet_refusals.row_count ==
              after_packet_refusals.row_count &&
              before_packet_refusals.ten_count ==
                  after_packet_refusals.ten_count &&
              before_packet_refusals.null_count ==
                  after_packet_refusals.null_count &&
              before_packet_refusals.other_count ==
                  after_packet_refusals.other_count,
          "malformed/stale rpvd2 packet mutated source rows");
  Rollback(refusal_writer);

  auto committed_writer = Begin(fixture, 211);
  const auto committed_descriptor =
      api::DescribeEngineRelationProjectionView(committed_writer, view_uuid);
  Require(!committed_descriptor.diagnostic.error &&
              committed_descriptor.present,
          "committed delete writer could not reload V2 descriptor");
  const auto committed_delete = api::EngineDeleteRows(
      UpdatableDeleteRequest(committed_writer, committed_descriptor));
  Require(committed_delete.ok &&
              EvidenceValue(committed_delete,
                            "relation_projection_view_marker") ==
                  api::kEngineRelationProjectionViewMarkerV2 &&
              EvidenceValue(committed_delete,
                            "relation_projection_view_uuid") == view_uuid &&
              EvidenceValue(
                  committed_delete,
                  "relation_projection_view_descriptor_uuid") ==
                  committed_descriptor.view_descriptor_uuid.canonical &&
              EvidenceValue(
                  committed_delete,
                  "relation_projection_view_descriptor_generation") ==
                  std::to_string(
                      committed_descriptor.view_descriptor_generation) &&
              EvidenceValue(
                  committed_delete,
                  "relation_projection_view_source_relation_uuid") ==
                  fixture.updatable_table_uuid &&
              EvidenceValue(
                  committed_delete,
                  "relation_projection_view_delete_expansion") ==
                  "engine_owned_sql_free" &&
              EvidenceValue(
                  committed_delete,
                  "relation_projection_view_delete_mga_authority") ==
                  "ordinary_optimized_delete" &&
              EvidenceValue(committed_delete,
                            "relation_projection_view_parser_sql") ==
                  "false",
          "engine-owned SQL-free view DELETE failed");
  RequireDelete03VisibleRows(committed_writer,
                             fixture.updatable_table_uuid,
                             0,
                             1,
                             "committed writer did not leave exactly NULL");
  Commit(committed_writer);

  RequireDelete03VisibleRows(snapshot_reader,
                             fixture.updatable_table_uuid,
                             2,
                             1,
                             "stable snapshot observed later committed DELETE");
  Rollback(snapshot_reader);

  auto final_reader = Begin(fixture, 212);
  RequireDelete03VisibleRows(final_reader,
                             fixture.updatable_table_uuid,
                             0,
                             1,
                             "fresh reader did not observe two-row DELETE plus NULL");
  const auto final_descriptor = api::DescribeEngineRelationProjectionView(
      final_reader, view_uuid);
  Require(!final_descriptor.diagnostic.error &&
              SameAuthoritativeDescriptor(final_descriptor, own_descriptor),
          "V2 descriptor changed after DELETE mutation");
  Rollback(final_reader);
}

}  // namespace

int main() {
  try {
    TestExactDescriptorCacheFalseNegativeRecovery();
    auto fixture = CreateFixture();
    TestRelationProjectionView(fixture);
    TestUpdatableRelationProjectionView(fixture);
    std::cout << "relation projection view conformance passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return EXIT_FAILURE;
  }
}
