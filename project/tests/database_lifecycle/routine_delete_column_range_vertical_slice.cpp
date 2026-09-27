#include "../support/engine_evidence_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "database_lifecycle.hpp"
#include "catalog/name_registry.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "catalog/name_registry_codec.hpp"
#include "behavior_support/api_behavior_record_codec.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "memory.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "prepared_metadata_binding.hpp"
#include "sblr_dispatch.hpp"
#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/sblr/lowering.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include "../sbsql_parser_worker/canonical_sblr_admission_test_helper.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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

platform::Uuid NewUuid(platform::UuidKind kind, std::uint64_t salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1950000000000ull + salt);
  Require(generated.ok(), "routine vertical-slice UUID generation failed");
  return generated.value.value;
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
  platform::Uuid database_uuid;
  platform::Uuid filespace_uuid;
  platform::Uuid principal_uuid;
  platform::Uuid session_uuid;
  platform::Uuid schema_uuid;
  platform::Uuid table_uuid;
  platform::Uuid procedure_uuid;
  platform::Uuid column_uuid;
  std::uint64_t salt = 0;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> engine_session;

  Fixture() = default;
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;
  Fixture(Fixture&& other)
      : directory(std::exchange(other.directory, {})),
        database_path(std::move(other.database_path)), database_uuid(other.database_uuid),
        filespace_uuid(other.filespace_uuid), principal_uuid(other.principal_uuid),
        session_uuid(other.session_uuid), schema_uuid(other.schema_uuid),
        table_uuid(other.table_uuid), procedure_uuid(other.procedure_uuid),
        column_uuid(other.column_uuid), salt(other.salt),
        engine_session(std::move(other.engine_session)) {}

  ~Fixture() {
    engine_session.reset();
    if (directory.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

Fixture CreateFixture() {
  Fixture fixture;
  const auto path_nonce = std::chrono::steady_clock::now().time_since_epoch();
  // UUIDv7 timestamps are milliseconds, not native steady-clock ticks. Keep
  // the higher-resolution nonce only for the unique temporary path.
  fixture.salt = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(path_nonce).count());
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_routine_delete_range_" +
                       std::to_string(path_nonce.count()));
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
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "routine vertical-slice database creation failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.filespace_uuid = create.filespace_uuid.value;
  fixture.principal_uuid =
      scratchbird::tests::BootstrapFixtureOwnerContext(create).principal_uuid;
  fixture.session_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 5);
  fixture.schema_uuid =
      NewUuid(platform::UuidKind::schema, fixture.salt + 6);
  fixture.table_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 7);
  auto owner = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  owner.session_uuid = fixture.session_uuid;
  // Keep one owning engine/session alive for the entire transaction fixture.
  fixture.engine_session = std::make_shared<scratchbird::tests::FixtureEngineSession>(owner);
  return fixture;
}

api::EngineRequestContext Begin(Fixture& fixture, std::uint64_t ordinal) {
  api::EngineBeginTransactionRequest begin;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id =
      "routine-delete-range-begin-" + std::to_string(ordinal);
  begin.context.database_path = fixture.database_path.string();
  begin.context.database_uuid = fixture.database_uuid;
  begin.context.default_root_uuid = fixture.filespace_uuid;
  begin.context.principal_uuid = fixture.principal_uuid;
  begin.context.session_uuid = fixture.session_uuid;
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.datatype_catalog_snapshot_uuid = api::kBootstrapDatatypeCatalogUuid;
  begin.context.datatype_catalog_generation = api::kBootstrapDatatypeCatalogGeneration;
  begin.context.datatype_registry_generation = api::kBootstrapDatatypeRegistryGeneration;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch = 1;
  begin.context.name_resolution_epoch = 1;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(begin.context);
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  RequireOk(begun, "routine vertical-slice transaction begin failed");
  auto context = begin.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  context.current_schema_uuid = fixture.schema_uuid;
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
  schema.target_object.uuid = fixture.schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("routine_slice"));
  RequireOk(api::EngineCreateSchema(schema),
            "routine vertical-slice schema create failed");

  api::EngineCreateTableRequest table;
  table.context = context;
  table.target_schema.uuid = fixture.schema_uuid;
  table.target_schema.object_kind = "schema";
  table.requested_table_uuid = fixture.table_uuid;
  table.table_names.push_back(Name("test_values"));
  api::EngineColumnDefinition column;
  column.names.push_back(Name("a"));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "integer";
  column.descriptor.encoded_descriptor = "type=integer";
  namespace dt = scratchbird::core::datatypes;
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "routine fixture datatype catalog is unavailable");
  const auto datatype = dt::LookupDatatypeCatalogRow(manifest.manifest, dt::CanonicalTypeId::int32);
  Require(datatype.ok() && datatype.manifest.descriptor_rows.size() == 1,
          "routine fixture integer datatype is not catalog-bound");
  const auto& published = datatype.manifest.descriptor_rows.front();
  const auto binding = dt::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, published.descriptor_uuid.value,
      published.descriptor_epoch);
  Require(binding.ok, "routine fixture integer codec is not catalog-bound");
  column.requested_column_uuid = api::GenerateCrudEngineUuid("column");
  column.descriptor.descriptor_uuid = api::GenerateCrudEngineUuid("object");
  column.descriptor.datatype_descriptor_uuid = binding.row.descriptor_uuid;
  column.descriptor.datatype_descriptor_generation = binding.row.descriptor_generation;
  column.descriptor.type_uuid = binding.row.type_uuid;
  column.ordinal = 0;
  column.nullable = false;
  table.table_columns.push_back(column);
  RequireOk(api::EngineCreateTable(table),
            "routine vertical-slice table create failed");

  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> insert(
      *fixture.engine_session, context);
  insert.target_table.uuid = fixture.table_uuid;
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
  const api::MgaRelationReadView state =
      api::BuildMgaRelationReadView(loaded.state);
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
              !descriptor.columns.front().column_uuid.is_nil(),
          "routine vertical-slice column UUID binding is unavailable");
  fixture.column_uuid = descriptor.columns.front().column_uuid;
}

void AddText(sblr::SblrOperationEnvelope* envelope,
             std::string name,
             std::string value) {
  envelope->operands.push_back(
      {"text", std::move(name), std::move(value)});
}

std::string CompiledRangeDescriptor(const Fixture& fixture, bool valid) {
  std::string out = std::string(api::kRoutineDeleteColumnRangeCountDescriptorV1) + "|";
  out.append(reinterpret_cast<const char*>(fixture.table_uuid.bytes.data()), 16);
  out.append(reinterpret_cast<const char*>(fixture.column_uuid.bytes.data()), 16);
  for (std::uint32_t slot : {0u, 1u, 2u, valid ? 2u : 9u}) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      out.push_back(static_cast<char>((slot >> shift) & 0xff));
    }
  }
  return out;
}

api::EngineCreateProcedureResult ExecuteCreateOrAlter(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    bool include_published_uuid = true,
    bool valid_compiled_descriptor = true) {
  api::EngineCreateProcedureRequest request;
  request.context = context;
  request.operation_id = "ddl.create_procedure";
  request.target_object.object_kind = "procedure";
  if (include_published_uuid && !fixture.procedure_uuid.is_nil()) {
    request.target_object.uuid = fixture.procedure_uuid;
  }
  request.target_schema.uuid = fixture.schema_uuid;
  request.target_schema.object_kind = "schema";
  request.localized_names.push_back(Name("delete_between_values"));
  request.option_envelopes = {
      "executor:sblr",
      "sblr_hash:sha256:3f4bbd573a74f8a6a99d1073cc8f6f954f030e20f44dfbcebd2f4f3df953f861",
      "sblr_provenance:engine_compiled_uuid_bound_routine_v1",
      "side_effect_class:data_mutation",
      "executable_descriptor_kind:create_or_alter_procedure",
      "compiled_body_descriptor:" +
          CompiledRangeDescriptor(fixture, valid_compiled_descriptor),
      "routine_parameter_count:2",
      "routine_parameter_0_mode:in",
      "routine_parameter_0_type:integer",
      "routine_parameter_1_mode:in",
      "routine_parameter_1_type:integer",
      "routine_return_count:1",
      "routine_return_0_type:integer"};
  api::EngineObjectReference related;
  related.uuid = fixture.table_uuid;
  related.object_kind = "table";
  request.related_objects.push_back(std::move(related));
  return api::EngineCreateProcedure(request);
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
  sblr::SblrOperand target;
  target.type = "uuid";
  target.name = "target_object_uuid";
  target.value_kind = sblr::SblrValueKind::uuid_ref;
  target.value_body.assign(fixture.procedure_uuid.bytes.begin(),
                           fixture.procedure_uuid.bytes.end());
  envelope.operands.push_back(std::move(target));
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

api::EngineInvokeExecutableObjectResult ExecuteInvoke(
    const Fixture& fixture,
    const scratchbird::tests::FixtureEngineStatement& statement,
    std::string lower,
    std::string upper,
    bool include_upper = true,
    const platform::Uuid* transaction_override = nullptr) {
  api::EngineInvokeExecutableObjectRequest request;
  request.context = statement.context;
  // This component route binds the persisted routine version selected by the
  // live statement. Never invent its identity, generation, or metadata epoch.
  const auto metadata = api::LoadExecutableObjectLifecycleState(request.context);
  Require(metadata.ok, "routine statement metadata load failed");
  for (const auto& object : metadata.state.objects) {
    if (object.object_uuid != fixture.procedure_uuid) continue;
    request.context.prepared_metadata_required_object_uuid = object.object_uuid;
    request.context.prepared_metadata_required_executable_generation = object.executable_generation;
    request.context.prepared_metadata_required_metadata_epoch = object.metadata_epoch;
  }
  Require(!request.context.prepared_metadata_required_object_uuid.is_nil(),
          "routine statement did not select its persisted executable");
  // Fault injection belongs after receipt acquisition: this case tests the
  // routine boundary's exact-selector check, not malformed fixture admission.
  if (transaction_override) request.context.transaction_uuid = *transaction_override;
  request.operation_id = "routine.procedure_invoke";
  request.target_object.uuid = fixture.procedure_uuid;
  request.target_object.object_kind = "procedure";
  request.option_envelopes = {
      "routine_argument_count:2",
      "routine_argument_0_type:integer",
      "routine_argument_0_value:" + std::move(lower),
      "policy:executable.side_effect:allow"};
  if (include_upper) {
    request.option_envelopes.push_back("routine_argument_1_type:integer");
    request.option_envelopes.push_back("routine_argument_1_value:" +
                                       std::move(upper));
  }
  return api::EngineInvokeExecutableObject(request);
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

sb_engine_uuid_t PublicUuid(const platform::Uuid& identity) {
  sb_engine_uuid_t result{};
  static_assert(sizeof(result.bytes) == sizeof(identity.bytes));
  std::memcpy(result.bytes, identity.bytes.data(), sizeof(result.bytes));
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
        session_uuid_(PublicUuid(fixture.session_uuid)),
        owner_(fixture.engine_session), session_(owner_->get()) {
    // Reuse the live owning engine/session; its constructor checked both
    // public open and session admission before any fixture transactions.
    Require(session_ != nullptr, "routine private bridge has no owning session");
  }

  PrivatePreparedMetadataSession(const PrivatePreparedMetadataSession&) = delete;
  PrivatePreparedMetadataSession& operator=(
      const PrivatePreparedMetadataSession&) = delete;

  ~PrivatePreparedMetadataSession() {
    if (binding_ != nullptr) {
      (void)bridge::ReleasePreparedMetadataBinding(binding_);
    }
  }

  std::string Bind(const std::vector<std::uint8_t>& envelope,
                   std::uint64_t prepare_transaction_ref,
                   const platform::Uuid& sealed_prepare_transaction_uuid) {
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
      const platform::Uuid& mismatched_prepare_transaction_uuid) {
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
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> owner_;
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
    if (evidence.evidence_kind == kind && scratchbird::tests::EvidenceTextEquals(evidence.evidence_id, value)) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 const platform::Uuid& identity) {
  for (const auto& evidence : result.evidence) {
    const auto* actual = std::get_if<platform::Uuid>(&evidence.evidence_id);
    if (evidence.evidence_kind == kind && actual && *actual == identity) return true;
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
  for (const auto& suffix : {".sb.api_events.v2", ".sb.name_events.v2"}) {
    std::ifstream events(fixture.database_path.string() + suffix, std::ios::binary);
    Require(events.is_open(), "routine durable binary identity store unavailable");
    while (events.peek() != std::char_traits<char>::eof()) {
      api::ApiBehaviorRecord record;
      Require(api::ReadApiBehaviorRecord(events, &record),
              "routine durable identity record failed framing/checksum validation");
      if (record.operation_id == "ddl.create_procedure" &&
          record.object_kind == "procedure" && record.object_uuid == fixture.procedure_uuid) {
        ++counts.create_records;
      }
      if (record.object_kind == "name_registry.entry") {
        api::NameRegistryEntry entry;
        Require(api::DecodeNameRegistryEntry(record.payload, &entry),
                "routine durable name identity record invalid");
        if (entry.object_uuid == fixture.procedure_uuid &&
            entry.object_class == "procedure" && entry.scope_uuid == fixture.schema_uuid) {
          ++counts.name_entries;
        }
      }
    }
    Require(!events.bad(), "routine durable identity store read failed");
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

void RequireApiOk(const api::EngineApiResult& result,
                  std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, message);
}

}  // namespace

int main() {
  scratchbird::core::memory::AllocationPolicy memory_policy;
  memory_policy.policy_name = "routine_delete_column_range_vertical_slice";
  memory_policy.hard_limit_bytes = 64ull * 1024 * 1024;
  memory_policy.soft_limit_bytes = 48ull * 1024 * 1024;
  memory_policy.per_context_limit_bytes = 32ull * 1024 * 1024;
  memory_policy.page_buffer_pool_limit_bytes = 16ull * 1024 * 1024;
  memory_policy.track_allocations = true;
  memory_policy.zero_memory_on_release = true;
  const auto memory = scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
      memory_policy, "routine_delete_column_range_vertical_slice");
  Require(memory.ok() && memory.fixture_mode, "routine memory fixture configuration failed");
  Fixture fixture = CreateFixture();

  auto setup = Begin(fixture, 1);
  CreateTableAndRows(fixture, setup);

  fixture.procedure_uuid =
      NewUuid(platform::UuidKind::object, fixture.salt + 1000);
  const auto invented_uuid = ExecuteCreateOrAlter(fixture, setup);
  fixture.procedure_uuid = {};
  Require(!invented_uuid.ok &&
              !invented_uuid.diagnostics.empty() &&
              invented_uuid.diagnostics.front().detail ==
                  "ddl.create_procedure:create_or_alter_uuid_not_engine_resolved",
          "routine create route admitted a parser-invented procedure UUID");

  const auto invalid_program =
      ExecuteCreateOrAlter(fixture, setup, true, false);
  Require(!invalid_program.ok &&
              !invalid_program.diagnostics.empty() &&
              invalid_program.diagnostics.front().code ==
                  api::kExecutableObjectDiagnosticRoutineDescriptorInvalid,
          "routine create preflight admitted an invalid compiled descriptor");
  api::EngineApiRequest failed_create_lookup;
  failed_create_lookup.context = setup;
  failed_create_lookup.target_schema.uuid = fixture.schema_uuid;
  failed_create_lookup.target_schema.object_kind = "schema";
  failed_create_lookup.localized_names.push_back(
      Name("delete_between_values"));
  const auto failed_create_name =
      api::ResolveNameRegistryPrivate(failed_create_lookup, "procedure");
  Require(!failed_create_name.ok &&
              failed_create_name.diagnostic.code == "CATALOG.NAME.NOT_FOUND",
          "failed routine preflight appended partial name/catalog state");

  const auto created = ExecuteCreateOrAlter(fixture, setup);
  RequireApiOk(created, "routine CREATE OR ALTER create route failed");
  fixture.procedure_uuid = created.primary_object.uuid;
  Require(!fixture.procedure_uuid.is_nil(),
          "routine create route did not publish an engine-owned UUID");
  Require(HasEvidence(created,
                      "create_or_alter_resolution",
                      "create") &&
              HasEvidence(created,
                          "create_or_alter_authority",
                          "engine_exact_mga") &&
              HasEvidence(created,
                          "create_or_alter_binding",
                          "engine_allocated_uuid") &&
              HasEvidence(created,
                          "create_or_alter_preflight",
                          "validated_before_catalog_persistence"),
          "routine create route did not expose exact-MGA preflight authority");
  Commit(setup);

  // Hold a data snapshot that predates every later routine generation.  The
  // private bridge must be able to refresh metadata without replacing this
  // engine-owned transaction data boundary.
  auto old_data_context = Begin(fixture, 2);
  // Pin the real statement metadata before later ALTERs, retaining its owning
  // receipt throughout invocation. Reacquisition would select newer metadata.
  scratchbird::tests::FixtureEngineStatement old_data_statement(
      *fixture.engine_session, old_data_context);
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
      ExecuteCreateOrAlter(fixture, rolled_back_alter, false);
  const auto durable_identity_after_alter =
      DurableRoutineIdentityState(fixture);
  RequireApiOk(altered_then_rolled_back,
               "routine CREATE OR ALTER alter route failed");
  Require(HasEvidence(altered_then_rolled_back,
                      "create_or_alter_resolution",
                      "alter") &&
              HasEvidence(altered_then_rolled_back,
                          "create_or_alter_binding",
                          "engine_name_schema") &&
              !HasEvidence(altered_then_rolled_back,
                           "name_registry",
                           fixture.procedure_uuid) &&
              altered_then_rolled_back.catalog_row_uuid.is_nil() &&
              HasEvidence(altered_then_rolled_back,
                          "create_or_alter_catalog_mutation",
                          "no_create_or_name_append_on_alter") &&
              durable_identity_after_alter == durable_identity_before_alter,
          "routine alter route appended create/catalog identity state");
  Rollback(rolled_back_alter);

  auto committed_alter = Begin(fixture, 4);
  Require(VisibleGeneration(fixture, committed_alter) == 1,
          "rolled-back routine alteration became MGA-visible");
  const auto altered = ExecuteCreateOrAlter(fixture, committed_alter);
  RequireApiOk(altered, "routine committed CREATE OR ALTER route failed");
  Require(HasEvidence(altered,
                      "executable_generation",
                      "2"),
          "routine committed alteration did not advance generation");
  Commit(committed_alter);

  auto active_later_alter = Begin(fixture, 5);
  Require(VisibleGeneration(fixture, active_later_alter) == 2,
          "routine later ALTER did not begin from generation two");
  const auto later_altered =
      ExecuteCreateOrAlter(fixture, active_later_alter);
  RequireApiOk(later_altered,
               "routine active later CREATE OR ALTER route failed");
  Require(HasEvidence(later_altered,
                      "executable_generation",
                      "3"),
          "routine active later ALTER did not create generation three");

  auto prepare_before_later_commit = Begin(fixture, 6);
  Require(VisibleGeneration(fixture, prepare_before_later_commit) == 2,
          "active later routine generation leaked into prepare visibility");
  Commit(active_later_alter);
  Require(VisibleGeneration(fixture, prepare_before_later_commit) == 2,
          "routine statement snapshot adopted a later committed generation");
  Commit(prepare_before_later_commit);

  auto current_prepare = Begin(fixture, 7);
  Require(VisibleGeneration(fixture, current_prepare) == 3,
          "committed later routine generation is not current metadata");
  Commit(current_prepare);

  Require(VisibleGeneration(fixture, old_data_context) == 1,
          "routine old data snapshot unexpectedly adopted later metadata");

  auto stale_context = old_data_context;
  stale_context.transaction_uuid =
      NewUuid(platform::UuidKind::transaction, fixture.salt + 1000);
  const auto stale = ExecuteInvoke(fixture, old_data_statement, "4", "7", true,
                                   &stale_context.transaction_uuid);
  Require(!stale.ok && !stale.diagnostics.empty() &&
              stale.diagnostics.front().code ==
                  api::kExecutableObjectDiagnosticExactMgaSelectorMismatch,
          "routine invocation admitted a mismatched MGA transaction selector");

  const auto missing_argument =
      ExecuteInvoke(fixture, old_data_statement, "4", "", false);
  Require(!missing_argument.ok &&
              !missing_argument.diagnostics.empty() &&
              missing_argument.diagnostics.front().code ==
                  api::kExecutableObjectDiagnosticRoutineArgumentInvalid,
          "routine invocation admitted a missing INTEGER input slot");

  auto concurrent_alter = Begin(fixture, 8);
  Require(VisibleGeneration(fixture, concurrent_alter) == 3,
          "routine atomicity ALTER did not begin from generation three");
  const auto generation_four =
      ExecuteCreateOrAlter(fixture, concurrent_alter);
  RequireApiOk(generation_four,
               "routine atomicity CREATE OR ALTER route failed");
  Require(HasEvidence(generation_four,
                      "executable_generation",
                      "4"),
          "routine atomicity ALTER did not stage generation four");

  const auto invoked = ExecuteInvoke(fixture, old_data_statement, "4", "7");
  RequireApiOk(invoked, "routine engine invocation failed");
  Require(invoked.result_shape.result_kind ==
                  "routine.procedure.result.v1" &&
              invoked.result_shape.rows.size() == 1 &&
              FieldValue(invoked.result_shape.rows.front(),
                         "routine_output_slot_2") == "4",
          "routine invocation did not return one authoritative output row");
  Require(HasEvidence(invoked,
                      "routine_instruction",
                      "delete.uuid_bound.column_range") &&
              HasEvidence(invoked,
                          "routine_target_column_uuid",
                          fixture.column_uuid) &&
              HasEvidence(invoked,
                          "routine_affected_rows_output_slot",
                          "2:4") &&
              HasEvidence(invoked, "executable_generation", "1"),
          "routine invocation did not preserve the old snapshot's exact UUID-bound program");
  Commit(old_data_context);
  Commit(concurrent_alter);

  auto reader = Begin(fixture, 9);
  Require(VisibleGeneration(fixture, reader) == 4,
          "routine concurrent ALTER did not publish generation four");
  api::EngineSelectRowsRequest select;
  select.context = reader;
  select.source_object.uuid = fixture.table_uuid;
  select.source_object.object_kind = "table";
  const auto remaining = api::EngineSelectRows(select);
  RequireOk(remaining, "routine post-commit row read failed");
  Require(remaining.visible_count == 6,
          "routine UUID-bound range delete committed an incorrect row count");
  Commit(reader);

  // Routine binding is validation-only.  If the CREATE TABLE owner did not
  // leave its authoritative sealed relation-metadata snapshot, routine
  // CREATE/ALTER must refuse and must not reconstruct it as a side effect.
  const std::filesystem::path relation_metadata_path =
      fixture.database_path.string() + ".sb.mga_relation_metadata";
  Require(std::filesystem::exists(relation_metadata_path),
          "routine load-only refusal fixture has no relation metadata");
  std::error_code remove_error;
  std::filesystem::remove(relation_metadata_path, remove_error);
  Require(!remove_error && !std::filesystem::exists(relation_metadata_path),
          "routine load-only refusal fixture could not remove relation metadata");
  auto missing_descriptor = Begin(fixture, 10);
  const auto refused_without_descriptor =
      ExecuteCreateOrAlter(fixture, missing_descriptor, false);
  Require(!refused_without_descriptor.ok,
          "routine binding synthesized a missing relation descriptor");
  Require(!std::filesystem::exists(relation_metadata_path),
          "routine binding recreated missing relation metadata");
  Rollback(missing_descriptor);

  std::cout << "routine_delete_column_range_vertical_slice=passed\n";
  return EXIT_SUCCESS;
}
