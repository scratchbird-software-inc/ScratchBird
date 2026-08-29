// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "catalog/name_registry.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "prepared_metadata_binding.hpp"
#include "sblr_dispatch.hpp"
#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/sblr/lowering.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include "../release/public_release_authz_fixture.hpp"
#include "../sbsql_parser_worker/canonical_sblr_admission_test_helper.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
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

std::string NewUuid(platform::UuidKind kind, std::uint64_t salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1950000000000ull + salt);
  Require(generated.ok(), "routine vertical-slice UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

platform::TypedUuid NewTypedUuid(platform::UuidKind kind,
                                 std::uint64_t salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1950000000000ull + salt);
  Require(generated.ok(), "routine vertical-slice typed UUID generation failed");
  return generated.value;
}

struct Fixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string principal_uuid;
  std::string session_uuid;
  std::string schema_uuid;
  std::string table_uuid;
  std::string procedure_uuid;
  std::string column_uuid;
  std::uint64_t salt = 0;

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

Fixture CreateFixture() {
  Fixture fixture;
  fixture.salt = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_routine_delete_range_" +
                       std::to_string(fixture.salt));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "routine.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid =
      NewTypedUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid =
      NewTypedUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = 1950000000000ull + fixture.salt + 3;
  create.page_size = 8192;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "routine vertical-slice database creation failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.principal_uuid =
      NewUuid(platform::UuidKind::principal, fixture.salt + 4);
  fixture.session_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 5);
  fixture.schema_uuid =
      NewUuid(platform::UuidKind::schema, fixture.salt + 6);
  fixture.table_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 7);
  return fixture;
}

api::EngineRequestContext Begin(Fixture& fixture, std::uint64_t ordinal) {
  api::EngineBeginTransactionRequest begin;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id =
      "routine-delete-range-begin-" + std::to_string(ordinal);
  begin.context.database_path = fixture.database_path.string();
  begin.context.database_uuid.canonical = fixture.database_uuid;
  begin.context.principal_uuid.canonical = fixture.principal_uuid;
  begin.context.session_uuid.canonical = fixture.session_uuid;
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.datatype_catalog_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000d701";
  begin.context.datatype_catalog_generation = 1;
  begin.context.datatype_registry_generation = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch = 1;
  begin.context.name_resolution_epoch = 1;
  begin.context.trace_tags.push_back("right:CATALOG_MUTATE");
  scratchbird::tests::release::GrantMaterializedRights(
      &begin.context, {"CATALOG_MUTATE"});
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  RequireOk(begun, "routine vertical-slice transaction begin failed");
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
  api::EngineCommitTransactionRequest commit;
  commit.context = context;
  RequireOk(api::EngineCommitTransaction(commit),
            "routine vertical-slice commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  RequireOk(api::EngineRollbackTransaction(rollback),
            "routine vertical-slice rollback failed");
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

api::EngineTypedValue IntegerValue(std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "integer";
  typed.descriptor.encoded_descriptor = "type=integer";
  typed.encoded_value = std::to_string(value);
  return typed;
}

void CreateTableAndRows(Fixture& fixture,
                        const api::EngineRequestContext& context) {
  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid.canonical = fixture.schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("routine_slice"));
  RequireOk(api::EngineCreateSchema(schema),
            "routine vertical-slice schema create failed");

  api::EngineCreateTableRequest table;
  table.context = context;
  table.target_schema.uuid.canonical = fixture.schema_uuid;
  table.target_schema.object_kind = "schema";
  table.requested_table_uuid.canonical = fixture.table_uuid;
  table.table_names.push_back(Name("test_values"));
  api::EngineColumnDefinition column;
  column.names.push_back(Name("a"));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "integer";
  column.descriptor.encoded_descriptor = "type=integer";
  column.ordinal = 0;
  column.nullable = false;
  table.table_columns.push_back(column);
  RequireOk(api::EngineCreateTable(table),
            "routine vertical-slice table create failed");

  api::EngineInsertRowsRequest insert;
  insert.context = context;
  insert.target_table.uuid.canonical = fixture.table_uuid;
  insert.target_table.object_kind = "table";
  for (std::int64_t value = 1; value <= 10; ++value) {
    api::EngineRowValue row;
    row.fields.push_back({"a", IntegerValue(value)});
    insert.input_rows.push_back(std::move(row));
  }
  const auto inserted = api::EngineInsertRows(insert);
  RequireOk(inserted, "routine vertical-slice row insert failed");
  Require(inserted.inserted_count == 10,
          "routine vertical-slice did not insert ten rows");

  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "routine vertical-slice MGA relation load failed");
  const api::CrudState state =
      api::BuildCrudCompatibilityStateFromMga(loaded.state);
  const auto visible = api::FindVisibleCrudTable(
      state, fixture.table_uuid, context.local_transaction_id);
  Require(visible.has_value(),
          "routine vertical-slice table is not MGA-visible");
  const auto indexes = api::VisibleCrudIndexesForTable(
      state, fixture.table_uuid, context.local_transaction_id);
  api::MgaRelationStorageDescriptor descriptor;
  const auto descriptor_ready = api::EnsureMgaRelationStorageDescriptor(
      context, *visible, indexes, &descriptor);
  Require(!descriptor_ready.error,
          "routine vertical-slice relation descriptor is unavailable");
  Require(descriptor.columns.size() == 1 &&
              !descriptor.columns.front().column_uuid.canonical.empty(),
          "routine vertical-slice column UUID binding is unavailable");
  fixture.column_uuid = descriptor.columns.front().column_uuid.canonical;
}

void AddText(sblr::SblrOperationEnvelope* envelope,
             std::string name,
             std::string value) {
  envelope->operands.push_back(
      {"text", std::move(name), std::move(value)});
}

sblr::SblrDispatchResult DispatchCreateOrAlter(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    bool include_published_uuid = true,
    bool valid_compiled_descriptor = true) {
  auto envelope = sblr::MakeSblrEnvelope(
      "engine.op.ddl_create_procedure",
      "SBLR_DDL_CREATE_PROCEDURE",
      "trace.routine.delete_column_range.create_or_alter");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  AddText(&envelope, "target_object_kind", "procedure");
  if (include_published_uuid && !fixture.procedure_uuid.empty()) {
    AddText(&envelope, "target_object_uuid", fixture.procedure_uuid);
    AddText(&envelope, "procedure_object_uuid", fixture.procedure_uuid);
  }
  AddText(&envelope, "procedure_name", "delete_between_values");
  AddText(&envelope, "target_schema_uuid", fixture.schema_uuid);
  AddText(&envelope, "executor", "sblr");
  AddText(&envelope,
          "sblr_hash",
          "sha256:3f4bbd573a74f8a6a99d1073cc8f6f954f030e20f44dfbcebd2f4f3df953f861");
  AddText(&envelope,
          "sblr_provenance",
          "engine_compiled_uuid_bound_routine_v1");
  AddText(&envelope, "side_effect_class", "data_mutation");
  AddText(&envelope,
          "executable_descriptor_kind",
          "create_or_alter_procedure");
  AddText(&envelope,
          "compiled_body_descriptor",
          std::string(api::kRoutineDeleteColumnRangeCountDescriptorV1) + "|" +
              fixture.table_uuid + "|" + fixture.column_uuid +
              (valid_compiled_descriptor ? "|0|1|2|2" : "|0|1|2|9"));
  AddText(&envelope, "routine_parameter_count", "2");
  AddText(&envelope, "routine_parameter_0_mode", "in");
  AddText(&envelope, "routine_parameter_0_type", "integer");
  AddText(&envelope, "routine_parameter_1_mode", "in");
  AddText(&envelope, "routine_parameter_1_type", "integer");
  AddText(&envelope, "routine_return_count", "1");
  AddText(&envelope, "routine_return_0_type", "integer");
  AddText(&envelope, "related_object_0_uuid", fixture.table_uuid);
  AddText(&envelope, "related_object_0_kind", "table");
  AddText(&envelope, "permission", "manage_executable");
  envelope = scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));
  return sblr::DispatchSblrOperation(
      {context, std::move(envelope), api::EngineApiRequest{}});
}

sblr::SblrOperationEnvelope MakeInvokeEnvelope(
    const Fixture& fixture,
    std::string lower,
    std::string upper,
    bool include_upper = true) {
  auto envelope = sblr::MakeSblrEnvelope(
      "routine.procedure_invoke",
      "SBLR_PROCEDURE_INVOKE",
      "trace.routine.delete_column_range.invoke");
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  AddText(&envelope, "target_object_uuid", fixture.procedure_uuid);
  AddText(&envelope, "target_object_kind", "procedure");
  AddText(&envelope, "routine_argument_count", "2");
  AddText(&envelope, "routine_argument_0_type", "integer");
  AddText(&envelope, "routine_argument_0_value", std::move(lower));
  if (include_upper) {
    AddText(&envelope, "routine_argument_1_type", "integer");
    AddText(&envelope, "routine_argument_1_value", std::move(upper));
  }
  AddText(&envelope, "permission", "invoke_executable");
  return scratchbird::test::sbsql::CanonicalizeEngineSblrEnvelopeForTest(
      std::move(envelope));
}

sblr::SblrDispatchResult DispatchInvoke(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string lower,
    std::string upper,
    bool include_upper = true) {
  auto envelope = MakeInvokeEnvelope(
      fixture, std::move(lower), std::move(upper), include_upper);
  return sblr::DispatchSblrOperation(
      {context, std::move(envelope), api::EngineApiRequest{}});
}

std::vector<std::uint8_t> PublicInvokeEnvelope(const Fixture& fixture,
                                               std::string lower,
                                               std::string upper) {
  const std::string canonical = sblr::EncodeSblrEnvelope(
      MakeInvokeEnvelope(fixture, std::move(lower), std::move(upper)));
  return scratchbird::engine::sblr::EnvelopeBuilder()
      .operation(scratchbird::engine::SblrOperationFamily::management_control,
                 1)
      .append_bytes(
          reinterpret_cast<const std::uint8_t*>(canonical.data()),
          canonical.size())
      .encode();
}

sb_engine_uuid_t PublicUuid(std::string_view text) {
  const auto parsed = uuid::ParseUuid(std::string(text));
  Require(parsed.ok(), "routine private bridge UUID parse failed");
  sb_engine_uuid_t result{};
  static_assert(sizeof(result.bytes) == sizeof(parsed.value.bytes));
  std::memcpy(result.bytes, parsed.value.bytes.data(), sizeof(result.bytes));
  return result;
}

std::string PublicPayload(sb_engine_result_t result) {
  sb_engine_string_view_t payload{};
  Require(result != nullptr &&
              sb_engine_result_payload(result, &payload) ==
                  SB_ENGINE_STATUS_OK,
          "routine private bridge result payload unavailable");
  if (payload.data == nullptr) { return {}; }
  return std::string(payload.data, payload.data + payload.size_bytes);
}

void PrintPublicDiagnostics(sb_engine_result_t result) {
  if (result == nullptr) { return; }
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (sb_engine_result_diagnostics(result, &diagnostics) !=
      SB_ENGINE_STATUS_OK) {
    return;
  }
  for (std::uint64_t i = 0; i < diagnostics.diagnostic_count; ++i) {
    const auto& diagnostic = diagnostics.diagnostics[i];
    std::cerr << std::string_view(
                     diagnostic.symbolic_code.data,
                     static_cast<std::size_t>(
                         diagnostic.symbolic_code.size_bytes))
              << ':'
              << std::string_view(
                     diagnostic.safe_detail.data,
                     static_cast<std::size_t>(
                         diagnostic.safe_detail.size_bytes))
              << '\n';
  }
}

std::string FirstPublicDiagnosticCode(sb_engine_result_t result) {
  if (result == nullptr) { return {}; }
  sb_engine_diagnostic_set_view_t diagnostics{};
  if (sb_engine_result_diagnostics(result, &diagnostics) !=
          SB_ENGINE_STATUS_OK ||
      diagnostics.diagnostic_count == 0) {
    return {};
  }
  const auto& code = diagnostics.diagnostics[0].symbolic_code;
  return code.data == nullptr
             ? std::string{}
             : std::string(code.data, code.data + code.size_bytes);
}

class PrivatePreparedMetadataSession {
 public:
  explicit PrivatePreparedMetadataSession(const Fixture& fixture)
      : principal_uuid_(PublicUuid(fixture.principal_uuid)),
        session_uuid_(PublicUuid(fixture.session_uuid)) {
    const std::string path = fixture.database_path.string();
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open);
    open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.database_path_utf8 = path.data();
    open.database_path_size = path.size();
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    Require(sb_engine_open(&open, &engine_, nullptr) == SB_ENGINE_STATUS_OK &&
                engine_ != nullptr,
            "routine private bridge engine open failed");

    sb_engine_session_params_v1_t session{};
    session.struct_size = sizeof(session);
    session.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    session.effective_user_uuid = principal_uuid_;
    session.session_uuid = session_uuid_;
    session.default_language_utf8 = "en";
    session.default_language_size = 2;
    session.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    Require(sb_engine_session_begin(
                engine_, &session, &session_, nullptr) ==
                SB_ENGINE_STATUS_OK &&
                session_ != nullptr,
            "routine private bridge session begin failed");
  }

  PrivatePreparedMetadataSession(const PrivatePreparedMetadataSession&) = delete;
  PrivatePreparedMetadataSession& operator=(
      const PrivatePreparedMetadataSession&) = delete;

  ~PrivatePreparedMetadataSession() {
    if (binding_ != nullptr) {
      (void)bridge::ReleasePreparedMetadataBinding(binding_);
    }
    if (session_ != nullptr) {
      sb_engine_session_end_params_v1_t end{};
      end.struct_size = sizeof(end);
      end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
      end.rollback_active_transactions = 1;
      end.cancel_open_results = 1;
      (void)sb_engine_session_end(session_, &end, nullptr);
    }
    if (engine_ != nullptr) { (void)sb_engine_close(engine_, nullptr); }
  }

  std::string Bind(const std::vector<std::uint8_t>& envelope,
                   std::uint64_t prepare_transaction_ref,
                   std::string_view sealed_prepare_transaction_uuid) {
    const auto context = Context(prepare_transaction_ref);
    auto dispatch = DispatchParams(envelope);
    sb_engine_result_t result = nullptr;
    const auto status = bridge::CreatePreparedMetadataBinding(
        session_,
        &context,
        sealed_prepare_transaction_uuid,
        &dispatch,
        &binding_,
        &result);
    if (status != SB_ENGINE_STATUS_OK) { PrintPublicDiagnostics(result); }
    Require(status == SB_ENGINE_STATUS_OK && binding_ != nullptr &&
                result != nullptr,
            "routine private bridge prepared metadata bind failed");
    const std::string payload = PublicPayload(result);
    Require(sb_engine_result_release(result) == SB_ENGINE_STATUS_OK,
            "routine private bridge bind result release failed");
    return payload;
  }

  std::pair<sb_engine_status_t, std::string> RejectMismatchedPrepareSelector(
      const std::vector<std::uint8_t>& envelope,
      std::uint64_t prepare_transaction_ref,
      std::string_view mismatched_prepare_transaction_uuid) {
    Require(binding_ == nullptr,
            "routine private bridge mismatch probe already has a binding");
    const auto context = Context(prepare_transaction_ref);
    auto dispatch = DispatchParams(envelope);
    bridge::PreparedMetadataBindingHandle rejected_binding = nullptr;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::CreatePreparedMetadataBinding(
        session_,
        &context,
        mismatched_prepare_transaction_uuid,
        &dispatch,
        &rejected_binding,
        &result);
    const std::string diagnostic = FirstPublicDiagnosticCode(result);
    if (result != nullptr) {
      Require(sb_engine_result_release(result) == SB_ENGINE_STATUS_OK,
              "routine mismatch bind result release failed");
    }
    Require(rejected_binding == nullptr,
            "routine mismatch bind published a binding handle");
    return {status, diagnostic};
  }

  std::string Dispatch(const std::vector<std::uint8_t>& envelope,
                       std::uint64_t execution_transaction_ref) {
    Require(binding_ != nullptr,
            "routine private bridge dispatch has no prepared metadata binding");
    const auto context = Context(execution_transaction_ref);
    auto dispatch = DispatchParams(envelope);
    sb_engine_result_t result = nullptr;
    const auto status = bridge::DispatchWithPreparedMetadataBinding(
        session_, nullptr, &context, &dispatch, binding_, &result);
    if (status != SB_ENGINE_STATUS_OK) { PrintPublicDiagnostics(result); }
    Require(status == SB_ENGINE_STATUS_OK && result != nullptr,
            "routine private bridge prepared metadata dispatch failed");
    const std::string payload = PublicPayload(result);
    Require(sb_engine_result_release(result) == SB_ENGINE_STATUS_OK,
            "routine private bridge dispatch result release failed");
    return payload;
  }

  std::pair<sb_engine_status_t, std::string> DispatchFailure(
      const std::vector<std::uint8_t>& envelope,
      std::uint64_t execution_transaction_ref) {
    Require(binding_ != nullptr,
            "routine private bridge failure dispatch has no binding");
    const auto context = Context(execution_transaction_ref);
    auto dispatch = DispatchParams(envelope);
    sb_engine_result_t result = nullptr;
    const auto status = bridge::DispatchWithPreparedMetadataBinding(
        session_, nullptr, &context, &dispatch, binding_, &result);
    const std::string diagnostic = FirstPublicDiagnosticCode(result);
    if (result != nullptr) {
      Require(sb_engine_result_release(result) == SB_ENGINE_STATUS_OK,
              "routine failed dispatch result release failed");
    }
    return {status, diagnostic};
  }

  void ReleaseBinding() {
    Require(binding_ != nullptr &&
                bridge::ReleasePreparedMetadataBinding(binding_) ==
                    SB_ENGINE_STATUS_OK,
            "routine private bridge prepared metadata release failed");
    binding_ = nullptr;
  }

 private:
  sb_engine_request_context_v1_t Context(
      std::uint64_t transaction_ref) const {
    sb_engine_request_context_v1_t context{};
    context.struct_size = sizeof(context);
    context.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    context.effective_user_uuid = principal_uuid_;
    context.session_uuid = session_uuid_;
    context.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    context.rights_set_ref = 1;
    context.capability_set_ref = 1;
    context.transaction_ref = transaction_ref;
    return context;
  }

  static sb_engine_sblr_dispatch_params_v1_t DispatchParams(
      const std::vector<std::uint8_t>& envelope) {
    sb_engine_sblr_dispatch_params_v1_t dispatch{};
    dispatch.struct_size = sizeof(dispatch);
    dispatch.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    dispatch.envelope_bytes = envelope.data();
    dispatch.envelope_size_bytes = envelope.size();
    return dispatch;
  }

  sb_engine_uuid_t principal_uuid_{};
  sb_engine_uuid_t session_uuid_{};
  sb_engine_handle_t engine_ = nullptr;
  sb_engine_session_t session_ = nullptr;
  bridge::PreparedMetadataBindingHandle binding_ = nullptr;
};

struct PreparedMetadataInvocationBarrier {
  std::mutex mutex;
  std::condition_variable condition;
  bool dispatch_paused = false;
  bool release_dispatch = false;
  bool invocation_finished = false;
  bool commit_started = false;
  bool commit_finished = false;
};

void PausePreparedMetadataInvocation(std::string_view phase,
                                     void* context) {
  if (phase != "exact_version_acquired_under_inventory_guard") { return; }
  auto* barrier =
      static_cast<PreparedMetadataInvocationBarrier*>(context);
  Require(barrier != nullptr,
          "routine prepared metadata test hook has no barrier");
  std::unique_lock<std::mutex> lock(barrier->mutex);
  barrier->dispatch_paused = true;
  barrier->condition.notify_all();
  barrier->condition.wait(lock,
                          [&] { return barrier->release_dispatch; });
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == value) {
      return true;
    }
  }
  return false;
}

struct DurableRoutineIdentityCounts {
  std::size_t create_records = 0;
  std::size_t name_entries = 0;

  bool operator==(const DurableRoutineIdentityCounts&) const = default;
};

DurableRoutineIdentityCounts DurableRoutineIdentityState(
    const Fixture& fixture) {
  DurableRoutineIdentityCounts counts;
  std::ifstream events(fixture.database_path.string() + ".sb.api_events",
                       std::ios::binary);
  std::string line;
  const std::string create_record =
      "\tddl.create_procedure\t" + fixture.procedure_uuid +
      "\tprocedure\t";
  const std::string name_entry =
      "\t" + fixture.procedure_uuid + "\tprocedure\t" +
      fixture.schema_uuid + "\t";
  while (std::getline(events, line)) {
    if (line.starts_with("SBAPI1\tRECORD\t") &&
        line.find(create_record) != std::string::npos) {
      ++counts.create_records;
    }
    if (line.starts_with("SBNAME1\tENTRY\t") &&
        line.find(name_entry) != std::string::npos) {
      ++counts.name_entries;
    }
  }
  return counts;
}

std::string FieldValue(const api::EngineRowValue& row,
                       std::string_view name) {
  for (const auto& field : row.fields) {
    if (field.first == name) { return field.second.encoded_value; }
  }
  return {};
}

std::uint64_t VisibleGeneration(const Fixture& fixture,
                                const api::EngineRequestContext& context) {
  const auto loaded = api::LoadExecutableObjectLifecycleState(context);
  Require(loaded.ok, "routine executable lifecycle load failed");
  for (const auto& object : loaded.state.objects) {
    if (object.object_uuid == fixture.procedure_uuid) {
      return object.executable_generation;
    }
  }
  return 0;
}

void RequireDispatchOk(const sblr::SblrDispatchResult& result,
                       std::string_view message) {
  if (!result.api_result.ok) {
    for (const auto& diagnostic : result.api_result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.envelope_validated && result.accepted &&
              result.dispatched_to_api && result.api_result.ok,
          message);
}

}  // namespace

int main() {
  Fixture fixture = CreateFixture();

  auto setup = Begin(fixture, 1);
  CreateTableAndRows(fixture, setup);

  fixture.procedure_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 1000);
  const auto invented_uuid = DispatchCreateOrAlter(fixture, setup);
  fixture.procedure_uuid.clear();
  Require(!invented_uuid.api_result.ok &&
              !invented_uuid.api_result.diagnostics.empty() &&
              invented_uuid.api_result.diagnostics.front().detail ==
                  "ddl.create_procedure:create_or_alter_uuid_not_engine_resolved",
          "routine create route admitted a parser-invented procedure UUID");

  const auto invalid_program =
      DispatchCreateOrAlter(fixture, setup, true, false);
  Require(!invalid_program.api_result.ok &&
              !invalid_program.api_result.diagnostics.empty() &&
              invalid_program.api_result.diagnostics.front().code ==
                  api::kExecutableObjectDiagnosticRoutineDescriptorInvalid,
          "routine create preflight admitted an invalid compiled descriptor");
  api::EngineApiRequest failed_create_lookup;
  failed_create_lookup.context = setup;
  failed_create_lookup.target_schema.uuid.canonical = fixture.schema_uuid;
  failed_create_lookup.target_schema.object_kind = "schema";
  failed_create_lookup.localized_names.push_back(
      Name("delete_between_values"));
  const auto failed_create_name =
      api::ResolveNameRegistryPrivate(failed_create_lookup, "procedure");
  Require(!failed_create_name.ok &&
              failed_create_name.diagnostic.code == "CATALOG.NAME.NOT_FOUND",
          "failed routine preflight appended partial name/catalog state");

  const auto created = DispatchCreateOrAlter(fixture, setup);
  RequireDispatchOk(created,
                    "routine CREATE OR ALTER create route failed");
  fixture.procedure_uuid = created.api_result.primary_object.uuid.canonical;
  Require(!fixture.procedure_uuid.empty(),
          "routine create route did not publish an engine-owned UUID");
  Require(HasEvidence(created.api_result,
                      "create_or_alter_resolution",
                      "create") &&
              HasEvidence(created.api_result,
                          "create_or_alter_authority",
                          "engine_exact_mga") &&
              HasEvidence(created.api_result,
                          "create_or_alter_binding",
                          "engine_allocated_uuid") &&
              HasEvidence(created.api_result,
                          "create_or_alter_preflight",
                          "validated_before_catalog_persistence"),
          "routine create route did not expose exact-MGA preflight authority");
  Commit(setup);

  // Hold a data snapshot that predates every later routine generation.  The
  // private bridge must be able to refresh metadata without replacing this
  // engine-owned transaction data boundary.
  auto old_data_context = Begin(fixture, 2);
  Require(old_data_context.snapshot_visible_through_local_transaction_id ==
              setup.local_transaction_id,
          "routine old data snapshot did not preserve its begin boundary");

  auto rolled_back_alter = Begin(fixture, 3);
  const auto durable_identity_before_alter =
      DurableRoutineIdentityState(fixture);
  Require(durable_identity_before_alter.create_records == 1 &&
              durable_identity_before_alter.name_entries >= 1,
          "routine durable identity baseline is unavailable");
  const auto altered_then_rolled_back =
      DispatchCreateOrAlter(fixture, rolled_back_alter, false);
  const auto durable_identity_after_alter =
      DurableRoutineIdentityState(fixture);
  RequireDispatchOk(altered_then_rolled_back,
                    "routine CREATE OR ALTER alter route failed");
  Require(HasEvidence(altered_then_rolled_back.api_result,
                      "create_or_alter_resolution",
                      "alter") &&
              HasEvidence(altered_then_rolled_back.api_result,
                          "create_or_alter_binding",
                          "engine_name_schema") &&
              !HasEvidence(altered_then_rolled_back.api_result,
                           "name_registry",
                           fixture.procedure_uuid) &&
              altered_then_rolled_back.api_result.catalog_row_uuid.canonical.empty() &&
              HasEvidence(altered_then_rolled_back.api_result,
                          "create_or_alter_catalog_mutation",
                          "no_create_or_name_append_on_alter") &&
              durable_identity_after_alter == durable_identity_before_alter,
          "routine alter route appended create/catalog identity state");
  Rollback(rolled_back_alter);

  auto committed_alter = Begin(fixture, 4);
  Require(VisibleGeneration(fixture, committed_alter) == 1,
          "rolled-back routine alteration became MGA-visible");
  const auto altered = DispatchCreateOrAlter(fixture, committed_alter);
  RequireDispatchOk(altered,
                    "routine committed CREATE OR ALTER route failed");
  Require(HasEvidence(altered.api_result,
                      "executable_generation",
                      "2"),
          "routine committed alteration did not advance generation");
  Commit(committed_alter);

  auto active_later_alter = Begin(fixture, 5);
  Require(VisibleGeneration(fixture, active_later_alter) == 2,
          "routine later ALTER did not begin from generation two");
  const auto later_altered =
      DispatchCreateOrAlter(fixture, active_later_alter);
  RequireDispatchOk(later_altered,
                    "routine active later CREATE OR ALTER route failed");
  Require(HasEvidence(later_altered.api_result,
                      "executable_generation",
                      "3"),
          "routine active later ALTER did not create generation three");

  auto prepare_before_later_commit = Begin(fixture, 6);
  Require(VisibleGeneration(fixture, prepare_before_later_commit) == 2,
          "active later routine generation leaked into prepare visibility");
  const auto public_invoke = PublicInvokeEnvelope(fixture, "4", "7");
  PrivatePreparedMetadataSession private_session(fixture);

  const auto selector_mismatch =
      private_session.RejectMismatchedPrepareSelector(
          public_invoke,
          prepare_before_later_commit.local_transaction_id,
          NewUuid(platform::UuidKind::transaction, fixture.salt + 2000));
  Require(selector_mismatch.first == SB_ENGINE_STATUS_CONFLICT &&
              selector_mismatch.second ==
                  "ENGINE.PREPARED_METADATA_BINDING.EXACT_MGA_SELECTOR_MISMATCH",
          "routine metadata bind admitted a mismatched local-ID/UUID selector");

  const std::string generation_two_binding = private_session.Bind(
      public_invoke,
      prepare_before_later_commit.local_transaction_id,
      prepare_before_later_commit.transaction_uuid.canonical);
  Require(generation_two_binding.find("executable_generation=2\n") !=
              std::string::npos &&
              generation_two_binding.find("metadata_snapshot_uuid=") !=
                  std::string::npos,
          "routine bind did not pin generation two while generation three was active");

  Commit(active_later_alter);
  const auto stale_binding = private_session.DispatchFailure(
      public_invoke, old_data_context.local_transaction_id);
  Require(stale_binding.first == SB_ENGINE_STATUS_CONFLICT &&
              stale_binding.second ==
                  "ENGINE.PREPARED_METADATA_BINDING.STALE",
          "routine bind did not fail stale after committed ALTER");
  const auto invalidated_binding = private_session.DispatchFailure(
      public_invoke, old_data_context.local_transaction_id);
  Require(invalidated_binding.first == SB_ENGINE_STATUS_CONFLICT &&
              invalidated_binding.second ==
                  "ENGINE.PREPARED_METADATA_BINDING.STALE",
          "stale routine binding remained reusable");
  private_session.ReleaseBinding();
  Commit(prepare_before_later_commit);

  auto current_prepare = Begin(fixture, 7);
  Require(VisibleGeneration(fixture, current_prepare) == 3,
          "committed later routine generation is not current metadata");
  const std::string generation_three_binding = private_session.Bind(
      public_invoke,
      current_prepare.local_transaction_id,
      current_prepare.transaction_uuid.canonical);
  Require(generation_three_binding.find("executable_generation=3\n") !=
              std::string::npos,
          "routine current metadata bind did not pin generation three");
  Commit(current_prepare);

  Require(VisibleGeneration(fixture, old_data_context) == 1,
          "routine old data snapshot unexpectedly adopted later metadata");

  auto stale_context = old_data_context;
  stale_context.transaction_uuid.canonical =
      NewUuid(platform::UuidKind::transaction, fixture.salt + 1000);
  const auto stale = DispatchInvoke(fixture, stale_context, "4", "7");
  Require(!stale.api_result.ok && !stale.api_result.diagnostics.empty() &&
              stale.api_result.diagnostics.front().code ==
                  api::kExecutableObjectDiagnosticExactMgaSelectorMismatch,
          "routine invocation admitted a mismatched MGA transaction selector");

  const auto missing_argument =
      DispatchInvoke(fixture, old_data_context, "4", "", false);
  Require(!missing_argument.api_result.ok &&
              !missing_argument.api_result.diagnostics.empty() &&
              missing_argument.api_result.diagnostics.front().code ==
                  api::kExecutableObjectDiagnosticRoutineArgumentInvalid,
          "routine invocation admitted a missing INTEGER input slot");

  auto concurrent_alter = Begin(fixture, 8);
  Require(VisibleGeneration(fixture, concurrent_alter) == 3,
          "routine atomicity ALTER did not begin from generation three");
  const auto generation_four =
      DispatchCreateOrAlter(fixture, concurrent_alter);
  RequireDispatchOk(generation_four,
                    "routine atomicity CREATE OR ALTER route failed");
  Require(HasEvidence(generation_four.api_result,
                      "executable_generation",
                      "4"),
          "routine atomicity ALTER did not stage generation four");

  PreparedMetadataInvocationBarrier invocation_barrier;
  bridge::SetPreparedMetadataBindingDispatchTestHookForTesting(
      &PausePreparedMetadataInvocation, &invocation_barrier);
  std::string invoked;
  std::thread invocation_thread([&] {
    invoked = private_session.Dispatch(
        public_invoke, old_data_context.local_transaction_id);
    std::lock_guard<std::mutex> lock(invocation_barrier.mutex);
    invocation_barrier.invocation_finished = true;
    invocation_barrier.condition.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(invocation_barrier.mutex);
    Require(invocation_barrier.condition.wait_for(
                lock,
                std::chrono::seconds(5),
                [&] { return invocation_barrier.dispatch_paused; }),
            "routine invocation did not pause after exact-version acquisition");
  }

  // The dispatch has copied one immutable engine-owned binding snapshot. Its
  // raw opaque handle may now be released without shortening the in-flight
  // invocation's lifetime or permitting a second handle dereference.
  private_session.ReleaseBinding();
  std::thread commit_thread([&] {
    {
      std::lock_guard<std::mutex> lock(invocation_barrier.mutex);
      invocation_barrier.commit_started = true;
      invocation_barrier.condition.notify_all();
    }
    Commit(concurrent_alter);
    std::lock_guard<std::mutex> lock(invocation_barrier.mutex);
    invocation_barrier.commit_finished = true;
    invocation_barrier.condition.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(invocation_barrier.mutex);
    Require(invocation_barrier.condition.wait_for(
                lock,
                std::chrono::seconds(5),
                [&] { return invocation_barrier.commit_started; }),
            "routine concurrent ALTER commit thread did not start");
    const bool commit_overtook_invocation =
        invocation_barrier.condition.wait_for(
            lock,
            std::chrono::milliseconds(150),
            [&] { return invocation_barrier.commit_finished; });
    Require(!commit_overtook_invocation,
            "routine ALTER commit overtook exact invocation acquisition");
    invocation_barrier.release_dispatch = true;
    invocation_barrier.condition.notify_all();
  }
  invocation_thread.join();
  commit_thread.join();
  bridge::SetPreparedMetadataBindingDispatchTestHookForTesting(nullptr,
                                                               nullptr);
  Require(invocation_barrier.invocation_finished &&
              invocation_barrier.commit_finished,
          "routine atomicity invocation or ALTER commit did not finish");
  Require(invoked.find("result_kind=routine.procedure.result.v1\n") !=
              std::string::npos &&
              invoked.find("row_count=1\n") != std::string::npos &&
              invoked.find("row[0]=routine_output_slot_2=4\n") !=
                  std::string::npos,
          "routine private-bridge invocation did not return one authoritative output row");
  Require(invoked.find(
              "evidence=routine_instruction:delete.uuid_bound.column_range\n") !=
              std::string::npos &&
              invoked.find("evidence=routine_target_column_uuid:" +
                           fixture.column_uuid + "\n") !=
                  std::string::npos &&
              invoked.find(
                  "evidence=routine_affected_rows_output_slot:2:4\n") !=
                  std::string::npos &&
              invoked.find("evidence=prepared_metadata_binding:consumed\n") !=
              std::string::npos &&
              invoked.find("evidence=prepared_metadata_exact_version:" +
                           fixture.procedure_uuid + ":3:") !=
                  std::string::npos &&
              invoked.find(
                  "evidence=prepared_metadata_atomicity:"
                  "routed_owner_inventory_guard_exact_version_lease\n") !=
                  std::string::npos,
          "routine invocation evidence did not preserve exact metadata/UUID atomicity");
  Commit(old_data_context);

  auto reader = Begin(fixture, 9);
  Require(VisibleGeneration(fixture, reader) == 4,
          "routine concurrent ALTER did not publish generation four");
  api::EngineSelectRowsRequest select;
  select.context = reader;
  select.source_object.uuid.canonical = fixture.table_uuid;
  select.source_object.object_kind = "table";
  const auto remaining = api::EngineSelectRows(select);
  RequireOk(remaining, "routine post-commit row read failed");
  Require(remaining.visible_count == 6,
          "routine UUID-bound range delete committed an incorrect row count");
  Commit(reader);

  // Routine binding is validation-only.  If the CREATE TABLE owner did not
  // leave a persisted relation descriptor, routine CREATE/ALTER must refuse
  // the binding and must not repair the missing sidecar as a side effect.
  const std::filesystem::path descriptor_path =
      fixture.database_path.string() + ".sb.mga_relation_descriptors";
  Require(std::filesystem::exists(descriptor_path),
          "routine load-only refusal fixture has no persisted descriptor");
  std::error_code remove_error;
  std::filesystem::remove(descriptor_path, remove_error);
  Require(!remove_error && !std::filesystem::exists(descriptor_path),
          "routine load-only refusal fixture could not remove the descriptor");
  auto missing_descriptor = Begin(fixture, 10);
  const auto refused_without_descriptor =
      DispatchCreateOrAlter(fixture, missing_descriptor, false);
  Require(!refused_without_descriptor.api_result.ok,
          "routine binding synthesized a missing relation descriptor");
  Require(!std::filesystem::exists(descriptor_path),
          "routine binding recreated a missing relation descriptor sidecar");
  Rollback(missing_descriptor);

  std::cout << "routine_delete_column_range_vertical_slice=passed\n";
  return EXIT_SUCCESS;
}
