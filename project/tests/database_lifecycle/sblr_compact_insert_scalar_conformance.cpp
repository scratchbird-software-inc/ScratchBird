// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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

constexpr std::string_view kScalarMarker = "scalar.octet_from_int64.v1";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "compact scalar test UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  api::EngineRequestContext context;

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
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, 1000);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, 1001);
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

api::EngineRequestContext Begin(Fixture& fixture) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, "compact-scalar-begin");
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "compact scalar transaction begin failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.root = std::filesystem::temp_directory_path() /
                 ("scratchbird_sblr_compact_scalar_" +
                  std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.root);
  fixture.database_path = fixture.root / "compact_scalar.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, 10);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, 11);
  create.creation_unix_epoch_millis = NowMillis();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "compact scalar database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, 20);
  fixture.context = Begin(fixture);

  api::CrudTableRecord table;
  table.creator_tx = fixture.context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "compact_scalar_values";
  table.columns.push_back({"octet_value", "canonical=text"});
  const auto appended = api::AppendMgaTableMetadata(fixture.context, table);
  Require(!appended.error, "compact scalar table metadata append failed");
  return fixture;
}

std::string Hex(std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(bytes.size() * 2u);
  for (const unsigned char byte : bytes) {
    encoded.push_back(kHex[(byte >> 4u) & 0x0fu]);
    encoded.push_back(kHex[byte & 0x0fu]);
  }
  return encoded;
}

struct CompactCell {
  std::string type;
  std::string value;
  bool is_null = false;
};

std::string CompactPayload(const std::vector<CompactCell>& cells) {
  std::string payload;
  for (const auto& cell : cells) {
    if (!payload.empty()) payload.push_back(';');
    payload += Hex("octet_value");
    payload.push_back('|');
    payload += Hex(cell.type);
    payload.push_back('|');
    payload += Hex(cell.value);
    payload.push_back('|');
    payload += cell.is_null ? "1" : "0";
  }
  return payload;
}

sblr::SblrDispatchResult DispatchCompactInsert(
    const Fixture& fixture,
    std::string compact_format,
    const std::vector<CompactCell>& cells) {
  auto envelope = sblr::MakeSblrEnvelope(
      "dml.insert_rows", "SBLR_DML_INSERT_ROWS",
      "SBLR-COMPACT-SCALAR-CONFORMANCE");
  envelope.opcode_code = 0x030eu;
  envelope.result_shape = "mutation_result";
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.parser_package_uuid =
      NewUuidText(platform::UuidKind::object, 2000);
  envelope.registry_snapshot_uuid =
      NewUuidText(platform::UuidKind::object, 2001);
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  const auto text_descriptor =
      NewUuid(platform::UuidKind::object, 2002).value.bytes;
  const auto append_text = [&](std::string name, std::string value) {
    sblr::SblrOperand operand;
    operand.ordinal =
        static_cast<std::uint32_t>(envelope.operands.size() + 1u);
    operand.type = "text";
    operand.name = std::move(name);
    operand.value_kind = sblr::SblrValueKind::literal_typed;
    operand.value_body.assign(text_descriptor.begin(), text_descriptor.end());
    const auto size = static_cast<std::uint64_t>(value.size());
    for (unsigned byte = 0; byte < 8; ++byte) {
      operand.value_body.push_back(
          static_cast<std::uint8_t>(size >> (byte * 8u)));
    }
    operand.value_body.insert(operand.value_body.end(), value.begin(),
                              value.end());
    envelope.operands.push_back(std::move(operand));
  };
  append_text("target_object_uuid", fixture.table_uuid);
  append_text("target_object_kind", "table");
  append_text("insert_values_row_count", std::to_string(cells.size()));
  append_text("insert_values_column_count", "1");
  append_text("insert_values_column_list_present", "false");
  append_text("insert_values_descriptor_column_0", "octet_value");
  append_text("insert_values_compact_format", std::move(compact_format));
  append_text("insert_values_compact_payload", CompactPayload(cells));

  sblr::SblrDispatchRequest request;
  request.context = fixture.context;
  request.context.request_id = "compact-scalar-dispatch";
  request.envelope = std::move(envelope);
  return sblr::DispatchSblrOperation(std::move(request));
}

api::EngineSelectRowsResult SelectAll(const Fixture& fixture) {
  api::EngineSelectRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = "compact-scalar-select";
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  return api::EngineSelectRows(request);
}

bool HasDiagnostic(const sblr::SblrDispatchResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void RequireFailedWithoutRows(const Fixture& fixture,
                              const std::vector<CompactCell>& cells,
                              std::string_view diagnostic_code,
                              api::EngineApiU64 expected_visible_count) {
  const auto failed = DispatchCompactInsert(
      fixture, "sblr.dml.insert.cells.hex.v1", cells);
  Require(failed.envelope_validated && !failed.accepted &&
              !failed.dispatched_to_api && !failed.api_result.ok,
          "invalid compact scalar marker reached the insert API");
  Require(HasDiagnostic(failed, diagnostic_code),
          "invalid compact scalar marker diagnostic drifted");
  const auto selected = SelectAll(fixture);
  RequireOk(selected, "post-refusal compact scalar select failed");
  Require(selected.visible_count == expected_visible_count,
          "invalid compact scalar marker persisted a row");
}

}  // namespace

int main() {
  auto fixture = MakeFixture();

  const std::vector<CompactCell> bound_values{
      {std::string(kScalarMarker), "0", false},
      {std::string(kScalarMarker), "1", false},
      {std::string(kScalarMarker), "255", false},
      {std::string(kScalarMarker), "not-evaluated-for-null", true},
  };
  const std::string encoded_bound_values = CompactPayload(bound_values);
  Require(encoded_bound_values.find(Hex("255")) != std::string::npos &&
              encoded_bound_values.find(static_cast<char>(0xff)) ==
                  std::string::npos,
          "compact test input already contained the expected raw octet");

  const auto inserted = DispatchCompactInsert(
      fixture, "sblr.dml.insert.cells.hex.v1", bound_values);
  if (!inserted.envelope_validated || !inserted.accepted ||
      !inserted.dispatched_to_api || !inserted.api_result.ok) {
    std::cerr << "compact insert state: envelope_validated="
              << inserted.envelope_validated
              << " accepted=" << inserted.accepted
              << " dispatched=" << inserted.dispatched_to_api
              << " api_ok=" << inserted.api_result.ok << '\n';
    for (const auto& diagnostic : inserted.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
    for (const auto& diagnostic : inserted.api_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(inserted.envelope_validated && inserted.accepted &&
              inserted.dispatched_to_api && inserted.api_result.ok,
          "bound compact scalar insert did not reach engine MGA execution");

  auto selected = SelectAll(fixture);
  RequireOk(selected, "compact scalar row select failed");
  Require(selected.visible_count == 4 &&
              selected.result_shape.rows.size() == 4,
          "compact scalar insert row count drifted");

  bool saw_nul = false;
  bool saw_one = false;
  bool saw_ff = false;
  bool saw_null = false;
  for (const auto& row : selected.result_shape.rows) {
    Require(row.fields.size() == 1,
            "compact scalar persisted row width drifted");
    const auto& value = row.fields.front().second;
    Require(value.descriptor.canonical_type_name != kScalarMarker,
            "bound scalar marker leaked into the stored descriptor");
    if (value.isSqlNull()) {
      saw_null = true;
      continue;
    }
    Require(value.encoded_value.size() == 1,
            "engine scalar evaluation did not produce exactly one byte");
    const auto octet =
        static_cast<unsigned char>(value.encoded_value.front());
    saw_nul = saw_nul || octet == 0;
    saw_one = saw_one || octet == 1;
    saw_ff = saw_ff || octet == 255;
  }
  Require(saw_nul && saw_one && saw_ff && saw_null,
          "engine scalar evaluation lost NUL, byte 1, byte 255, or SQL NULL");

  const auto legacy = DispatchCompactInsert(
      fixture, "sbsql.insert_values.cells.v1", {{"text", "A", false}});
  Require(legacy.envelope_validated && legacy.accepted &&
              legacy.dispatched_to_api && legacy.api_result.ok,
          "legacy compact insert payload alias regressed");
  selected = SelectAll(fixture);
  RequireOk(selected, "legacy alias verification select failed");
  Require(selected.visible_count == 5,
          "legacy alias insert did not persist exactly one row");

  RequireFailedWithoutRows(
      fixture,
      {{std::string(kScalarMarker), "65", false},
       {std::string(kScalarMarker), "not-an-int", false}},
      "SB_SBLR_SCALAR_OCTET_FROM_INT64_INVALID_SYNTAX", 5);
  RequireFailedWithoutRows(
      fixture, {{std::string(kScalarMarker), "-1", false}},
      "SB_SBLR_SCALAR_OCTET_FROM_INT64_OUT_OF_RANGE", 5);
  RequireFailedWithoutRows(
      fixture, {{std::string(kScalarMarker), "256", false}},
      "SB_SBLR_SCALAR_OCTET_FROM_INT64_OUT_OF_RANGE", 5);

  api::EngineRollbackTransactionRequest rollback;
  rollback.context = fixture.context;
  rollback.context.request_id = "compact-scalar-rollback";
  const auto rolled_back = api::EngineRollbackTransaction(rollback);
  RequireOk(rolled_back, "compact scalar transaction rollback failed");
  return EXIT_SUCCESS;
}
