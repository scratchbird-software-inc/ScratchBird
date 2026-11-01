// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission fixture. No public ABI,
// package gateway, executor registry, or statement receipt is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "datatype_type_codec_identity_current_projection.hpp"
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "../support/durable_authorization_fixture.hpp"
#include "../support/owned_bootstrap_database_fixture.hpp"
#include "engine/internal_api/ddl/create_api.hpp"
#include "engine/internal_api/security/security_principal_lifecycle.hpp"
#include "engine/internal_api/crud_support/crud_store.hpp"
#include "core/datatypes/datatype_catalog_manifest.hpp"
#include "core/datatypes/datatype_operations.hpp"
#include "engine/sblr/sblr_catalog_introspect_runtime.hpp"
#include "wire/parser_server_ipc/sbps_statement_management_bind_codec.hpp"

#include <atomic>
#include <cstdlib>
#include <string_view>

namespace {

namespace bind = scratchbird::wire::sbps_statement_management;

api::EngineLocalizedName CatalogName(std::string value) {
  api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = std::move(value);
  name.default_name = true;
  return name;
}

api::EngineColumnDefinition CatalogColumn(const api::EngineRequestContext& context,
                                          std::uint32_t ordinal,
                                          std::string value,
                                          std::string type) {
  namespace datatypes = scratchbird::core::datatypes;
  const auto manifest = datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "catalog fixture datatype catalog unavailable");
  const auto row = datatypes::LookupDatatypeCatalogRow(
      manifest.manifest, datatypes::CanonicalTypeIdFromStableName(type));
  Require(row.ok() && row.manifest.descriptor_rows.size() == 1,
          "catalog fixture datatype missing from catalog");
  const auto& descriptor = row.manifest.descriptor_rows.front();
  const auto codec = datatypes::LookupCurrentDatatypeTypeCodecIdentityAsV1(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, descriptor.descriptor_uuid.value,
      descriptor.descriptor_epoch);
  Require(codec.ok, "catalog fixture exact datatype codec binding unavailable");
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid = api::GenerateCrudEngineUuid("object");
  column.descriptor.descriptor_uuid = api::GenerateCrudEngineUuid("object");
  column.descriptor.datatype_descriptor_uuid = descriptor.descriptor_uuid.value;
  column.descriptor.datatype_descriptor_generation = descriptor.descriptor_epoch;
  column.descriptor.type_uuid = codec.row.type_uuid;
  // The cohort of the identity row; bound scalars and create table require it.
  column.descriptor.datatype_cohort = {codec.row.catalog_snapshot_uuid, codec.row.catalog_generation,
                               codec.row.registry_generation};
  column.names.push_back(CatalogName(std::move(value)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  column.descriptor.encoded_descriptor =
      "type=" + column.descriptor.canonical_type_name;
  column.nullable = ordinal != 0;
  return column;
}

// This fixture seeds a schema and table, so its principal must be the durable
// bootstrap owner of a credentialed create, not an invented UUID; catalog DDL
// authorizes CATALOG_MUTATE against the materialized durable grants.
Fixture CreateOwnerFixture() {
  Fixture fixture;
  const auto salt = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fixture.directory = std::filesystem::temp_directory_path() /
      ("sbsql_sblr_ia05_introspect_" + std::to_string(salt));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "fault.sbdb";
  fixture.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  fixture.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  fixture.session_uuid = NewUuid(platform::UuidKind::session, salt + 4);
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.creation_unix_epoch_millis = 1950000000000ull + salt % 1'000'000ull;
  create.page_size = 16384;
  create.allow_overwrite = true;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  scratchbird::tests::ConfigureOwnedBootstrapFixtureMemory("ia05_catalog_introspect_cancellation_fault");
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << "003612 owner create: " << created.diagnostic.diagnostic_code << ' '
              << created.diagnostic.message_key;
    for (const auto& argument : created.diagnostic.arguments) std::cerr << ' ' << argument.key;
    std::cerr << '\n';
    std::error_code ignored;
    std::filesystem::remove_all(fixture.directory, ignored);
  }
  Require(created.ok(), "003612 owner fixture database creation failed");
  fixture.resource_epoch = created.state.resource_seed_catalog.resource_epoch == 0
      ? 1 : created.state.resource_seed_catalog.resource_epoch;
  const auto bootstrap = db::ReadDatabaseBootstrapSecurityCatalog(create.path);
  Require(bootstrap.ok() && bootstrap.state.present && bootstrap.state.committed_by_inventory,
          "003612 owner fixture bootstrap principal missing");
  fixture.principal_uuid = bootstrap.state.principal_uuid;
  return fixture;
}

void SeedCatalogObject(api::EngineRequestContext context, const Fixture& fixture) {
  context.default_root_uuid = Identity(fixture.filespace_uuid);
  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid =
      Identity(NewUuid(platform::UuidKind::schema, 36121));
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(CatalogName("catalog_probe_schema"));
  const auto created_schema = api::EngineCreateSchema(schema);
  Require(created_schema.ok,
          "003612 catalog-introspect fixture schema creation failed");

  context.current_schema_uuid = schema.target_object.uuid;
  api::EngineCreateTableRequest table;
  table.context = context;
  table.target_schema = schema.target_object;
  table.requested_table_uuid =
      Identity(NewUuid(platform::UuidKind::object, 36122));
  table.table_names.push_back(CatalogName("catalog_probe_table"));
  table.table_columns.push_back(CatalogColumn(context, 0, "id", "int64"));
  table.table_columns.push_back(CatalogColumn(context, 1, "value", "int64"));
  const auto created_table = api::EngineCreateTable(table);
  if (!created_table.ok) {
    for (const auto& diagnostic : created_table.diagnostics) {
      std::cerr << "003612 table seed: " << diagnostic.code << ':'
                << diagnostic.message_key << ':' << diagnostic.detail
                << '\n';
    }
  }
  Require(created_table.ok,
          "003612 catalog-introspect fixture table creation failed");
}

sblr::SblrOperationEnvelope CatalogIntrospectMember(
    const bridge::StatementContextReceiptView& view,
    const platform::Uuid& parser_uuid,
    const std::vector<std::uint8_t>& descriptor_bytes) {
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.catalog_introspect", "SBLR_CATALOG_INTROSPECT",
      "ia05.catalog_introspect.cancellation_atomicity");
  member.opcode_code = 4864;
  member.result_shape = "catalog_introspect_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "catalog_introspect_descriptor";
  operand.name = "object_detail";
  operand.value_kind = sblr::SblrValueKind::catalog_introspect_descriptor;
  operand.value_body = descriptor_bytes;
  member.operands.push_back(std::move(operand));
  return member;
}

}  // namespace

void VerifyCatalogSnapshot(bool retire_owner) {
  auto fixture = CreateOwnerFixture();
  PublicSession session(fixture);
  std::atomic<bool> cancel{false};
  std::atomic<unsigned> probes{0};
  auto context = BeginTransaction(fixture, &probes);
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(context);
  context.trace_tags.push_back("right:SBLR_EXECUTOR_AVAILABILITY_ADMIN");
  const auto parser_uuid = Identity(NewUuid(platform::UuidKind::object, 36120));
  context.current_package_uuid = parser_uuid;
  context.query_cancellation_requested = [&] {
    probes.fetch_add(1, std::memory_order_relaxed);
    return cancel.load(std::memory_order_relaxed);
  };
  const auto security = api::LoadSecurityPrincipalLifecycleState(context);
  Require(security.ok && security.state.security_context_generation != 0 &&
              security.state.security_generation == context.security_epoch &&
              security.state.policy_generation == context.authorization_context.policy_epoch,
          "catalog fixture durable security cohort unavailable");
  context.authorization_context.security_context_generation =
      security.state.security_context_generation;

  // This fixture enters DDL through the private engine API. Obtain the exact
  // datatype registry cohort from the same receipt producer used by public
  // statements; the test never authors datatype snapshot identity.
  bridge::StatementContextAcquireRequest seed_acquire;
  seed_acquire.engine_context = &context;
  seed_acquire.exact_transaction_uuid = context.transaction_uuid;
  bridge::StatementContextReceiptHandle seed_receipt;
  bridge::StatementContextReceiptView seed_view;
  sb_engine_result_t result = nullptr;
  Require(bridge::AcquireStatementContextReceipt(
              session.session, &seed_acquire, &seed_receipt, &seed_view,
              &result) == SB_ENGINE_STATUS_OK,
          "003612 datatype authority receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  api::EngineRequestContext seed_context;
  result = nullptr;
  Require(bridge::CopyStatementContextEngineContextV1(
              seed_receipt, &seed_context, &result) == SB_ENGINE_STATUS_OK &&
              !seed_context.datatype_catalog_snapshot_uuid.is_nil() &&
              seed_context.datatype_catalog_generation != 0 &&
              seed_context.datatype_registry_generation != 0 &&
              seed_context.datatype_catalog_snapshot_uuid ==
                  seed_view.literal_catalog_snapshot_uuid &&
              seed_context.datatype_catalog_generation ==
                  seed_view.literal_catalog_generation &&
              seed_context.datatype_registry_generation ==
                  seed_view.literal_registry_generation,
          "003612 datatype authority receipt projection failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(bridge::ReleaseStatementContextReceipt(seed_receipt) ==
              SB_ENGINE_STATUS_OK,
          "003612 datatype authority receipt cleanup failed");
  context.datatype_catalog_snapshot_uuid =
      seed_context.datatype_catalog_snapshot_uuid;
  context.datatype_catalog_generation =
      seed_context.datatype_catalog_generation;
  context.datatype_registry_generation =
      seed_context.datatype_registry_generation;
  SeedCatalogObject(context, fixture);

  std::string detail;
  // Cancellation latches on the receipt (one-way; never cleared), so each
  // phase gets its own receipt: everything the operation binds lives on it.
  struct Attempt {
    bridge::StatementContextReceiptHandle receipt{};
    bridge::StatementContextReceiptView view{};
    std::vector<std::uint8_t> exact_bind;
    std::vector<std::uint8_t> descriptor_bytes;
    Submission submission;
  };

  const auto prepare = [&](Attempt* out) {
    bridge::StatementContextAcquireRequest acquire;
    acquire.engine_context = &context;
    acquire.exact_transaction_uuid = context.transaction_uuid;
    sb_engine_result_t result = nullptr;
    Require(bridge::AcquireStatementContextReceipt(
                session.session, &acquire, &out->receipt, &out->view,
                &result) == SB_ENGINE_STATUS_OK,
            "003612 live statement receipt acquisition failed");
    if (result != nullptr) (void)sb_engine_result_release(result);
    const auto& view = out->view;
    Require(view.catalog_introspect_executor_availability_generation != 0,
            "003612 receipt omitted catalog-introspect availability");

    bind::NameResolveBindRequestV1 public_bind;
    public_bind.authenticated_receipt_uuid = RawUuid(view.receipt_uuid);
    public_bind.occurrence = 1;
    public_bind.resolution_mode = 2;
    public_bind.object_class = 2;
    public_bind.target_name_atoms = {{"catalog_probe_schema", false},
                                     {"catalog_probe_table", false}};
    Require(bind::EncodeNameResolveBindRequestV1(
                public_bind, &out->exact_bind, &detail),
            "003612 canonical target-name bind encoding failed");
    bind::NameResolveBindRequestV1 decoded_bind;
    Require(bind::DecodeNameResolveBindRequestV1(
                out->exact_bind.data(), out->exact_bind.size(), &decoded_bind,
                &detail),
            "003612 canonical target-name bind decoding failed");

    bridge::StatementNameResolveBindRequestV1 bind_request;
    bind_request.authenticated_receipt_uuid = view.receipt_uuid;
    bind_request.occurrence = decoded_bind.occurrence;
    bind_request.resolution_mode = decoded_bind.resolution_mode;
    bind_request.object_class = decoded_bind.object_class;
    for (const auto& atom : decoded_bind.target_name_atoms) {
      bind_request.target_name_atoms.push_back({atom.raw_utf8, atom.quoted});
    }
    for (const auto& atom : decoded_bind.namespace_name_atoms) {
      bind_request.namespace_name_atoms.push_back(
          {atom.raw_utf8, atom.quoted});
    }
    bind_request.target_name_atoms_sha256 =
        decoded_bind.target_name_atoms_sha256;
    bind_request.namespace_name_atoms_sha256 =
        decoded_bind.namespace_name_atoms_sha256;
    bind_request.request_evidence_sha256 =
        decoded_bind.request_evidence_sha256;
    bind_request.exact_bind_request_bytes = out->exact_bind;
    bridge::StatementNameResolveBindAckV1 bind_ack;
    result = nullptr;
    Require(bridge::BindStatementNameResolveAuthorityV1(
                out->receipt, &bind_request, &bind_ack, &result) ==
                SB_ENGINE_STATUS_OK,
            "003612 engine target-name binding failed");
    if (result != nullptr) (void)sb_engine_result_release(result);

    bridge::StatementCatalogIntrospectAuthorityV1 authority;
    result = nullptr;
    const auto catalog_bind_status =
        bridge::BindStatementCatalogIntrospectAuthorityV1(
            out->receipt, 1, 1, &authority, &result);
    if (catalog_bind_status != SB_ENGINE_STATUS_OK && result != nullptr) {
      std::cerr << "003612 catalog bind: " << DiagnosticCode(result) << ':'
                << DiagnosticKey(result) << '\n';
    }
    if (catalog_bind_status == SB_ENGINE_STATUS_OK &&
        (authority.canonical_path_utf8 !=
             "CATALOG_PROBE_SCHEMA.CATALOG_PROBE_TABLE" ||
         authority.result_shape.result_kind !=
             "rs.sbsql.show_object_detail.v1")) {
      std::cerr << "003612 catalog authority: path="
                << authority.canonical_path_utf8 << ";result_kind="
                << authority.result_shape.result_kind << ";rows="
                << authority.result_shape.rows.size() << '\n';
    }
    Require(catalog_bind_status == SB_ENGINE_STATUS_OK &&
                authority.canonical_descriptor_bytes.size() == 488 &&
                authority.canonical_path_utf8 ==
                    "CATALOG_PROBE_SCHEMA.CATALOG_PROBE_TABLE" &&
                authority.result_shape.result_kind ==
                    "rs.sbsql.show_object_detail.v1" &&
                !authority.result_shape.rows.empty(),
            "003612 receipt-private catalog authority was not published");
    if (result != nullptr) (void)sb_engine_result_release(result);
    auto& descriptor_bytes = out->descriptor_bytes;
    descriptor_bytes = authority.canonical_descriptor_bytes;
    std::copy_n("CIDO", 4, descriptor_bytes.begin());
    sblr::SblrCatalogIntrospectDescriptorV1 decoded_descriptor;
    Require(sblr::DecodeSblrCatalogIntrospectDescriptorV1(
                descriptor_bytes.data(), descriptor_bytes.size(),
                &decoded_descriptor, &detail, true) &&
                decoded_descriptor.evidence ==
                    authority.descriptor_evidence_sha256,
            "003612 literal receipt CIDD-to-CIDO projection failed");
    out->submission = PackageWithMember(
        fixture, view, parser_uuid,
        CatalogIntrospectMember(view, parser_uuid, descriptor_bytes));
  };
  const auto admit = [&](Attempt& attempt,
                         bridge::StatementPackageAdmissionReservationHandle*
                             reservation) {
    return Admit(fixture, session, attempt.view, attempt.receipt, parser_uuid,
                 attempt.submission, reservation);
  };
  const auto dispatch = [&](Attempt& attempt, sb_engine_result_t* result) {
    bridge::StatementPackageAdmissionReservationHandle reservation;
    auto admitted = admit(attempt, &reservation);
    *result = nullptr;
    return bridge::DispatchStatementContextReceipt(&admitted, result);
  };

  // Every receipt is captured before the sibling publication below, so each
  // one exercises the same frozen visibility the single receipt used to.
  Attempt cancelled;
  Attempt success;
  cancel.store(false, std::memory_order_relaxed);
  prepare(&cancelled);
  if (!retire_owner) prepare(&success);

  // A later transaction publication must not replace or invalidate this
  // statement's frozen visibility. Exercise real inventory publication, not
  // a changed clock or fabricated receipt generation.
  std::atomic<unsigned> sibling_probes{0};
  const auto sibling = BeginTransaction(fixture, &sibling_probes);
  api::EngineCommitTransactionRequest sibling_commit;
  sibling_commit.context = sibling;
  Require(api::EngineCommitTransaction(sibling_commit).ok,
          "003612 independent transaction did not commit");

  result = nullptr;
  if (retire_owner) {
    bridge::StatementPackageAdmissionReservationHandle reservation;
    auto retired_owner_dispatch = admit(cancelled, &reservation);
    api::EngineRollbackTransactionRequest rollback;
    rollback.context = context;
    Require(api::EngineRollbackTransaction(rollback).ok,
            "003612 owning transaction did not roll back before dispatch");
    result = nullptr;
    const auto status = bridge::DispatchStatementContextReceipt(&retired_owner_dispatch, &result);
    Require(status == SB_ENGINE_STATUS_CONFLICT && result != nullptr &&
                DiagnosticCode(result) == "ENGINE.STATEMENT_CONTEXT.SNAPSHOT_STALE" &&
                ResultPayload(result).empty(),
            "003612 terminal owning transaction published catalog rows");
    (void)sb_engine_result_release(result);
    Require(bridge::ReleaseStatementContextReceipt(cancelled.receipt) ==
                SB_ENGINE_STATUS_OK,
            "003612 retired-owner receipt cleanup failed");
    return;
  }

  // Phase 1: cancellation stops catalog access on its own receipt.
  probes.store(0, std::memory_order_relaxed);
  cancel.store(true, std::memory_order_relaxed);
  Require(dispatch(cancelled, &result) == SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr,
          "003612 cancellation did not stop catalog access");
  const auto cancellation_code = DiagnosticCode(result);
  const auto cancellation_key = DiagnosticKey(result);
  const auto cancellation_probes = probes.load(std::memory_order_relaxed);
  if (cancellation_code != "PROCESS.CANCELLED" ||
      cancellation_key !=
          "sblr.catalog_introspect.cancelled_before_catalog_revalidation" ||
      cancellation_probes != 1) {
    std::cerr << "003612 cancellation observation: code="
              << cancellation_code << ";key=" << cancellation_key
              << ";probes=" << cancellation_probes << '\n';
  }
  Require(cancellation_code == "PROCESS.CANCELLED" &&
              cancellation_key ==
                  "sblr.catalog_introspect.cancelled_before_catalog_revalidation" &&
              cancellation_probes == 1,
          "003612 cancellation precedence or checkpoint drifted");
  (void)sb_engine_result_release(result);

  // The latch: the cancelled receipt stays cancelled with no new request, and
  // still publishes no catalog rows.
  probes.store(0, std::memory_order_relaxed);
  cancel.store(false, std::memory_order_relaxed);
  Require(dispatch(cancelled, &result) != SB_ENGINE_STATUS_OK &&
              result != nullptr &&
              DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              ResultPayload(result).empty(),
          "003612 cancelled receipt did not stay cancelled");
  (void)sb_engine_result_release(result);

  // Phase 2: a fresh receipt publishes the catalog-introspect result.
  probes.store(0, std::memory_order_relaxed);
  cancel.store(false, std::memory_order_relaxed);
  Require(dispatch(success, &result) == SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003612 fresh receipt did not publish catalog-introspect result");
  const auto result_bytes = ResultPayload(result);
  sblr::SblrCatalogIntrospectResultV1 decoded_result;
  Require(result_bytes.size() == 320 &&
              sblr::DecodeSblrCatalogIntrospectResultV1(
                  result_bytes.data(), result_bytes.size(), &decoded_result,
                  &detail) &&
              decoded_result.availability ==
                  success.view.catalog_introspect_executor_availability_generation,
          "003612 successful dispatch omitted canonical CIRS");
  (void)sb_engine_result_release(result);

  const api::SblrExecutorAvailabilityRowIdentity availability_identity{
      api::kSblrCatalogIntrospectExecutorId,
      api::kSblrCatalogIntrospectOpcodeCode,
      api::kSblrCatalogIntrospectOpcodeVersion,
      api::kSblrCatalogIntrospectOperandDescriptorId,
      api::kSblrCatalogIntrospectResultDescriptorId,
      api::kSblrCatalogIntrospectResultDescriptorVersion};
  const auto current_availability =
      api::LoadCurrentSblrExecutorAvailabilitySnapshot(
          context, availability_identity);
  Require(current_availability.ok &&
              current_availability.snapshot.installed &&
              current_availability.snapshot.generation ==
                  decoded_result.availability,
          "003612 current availability did not match the published CIRS");
  api::SblrExecutorAvailabilitySetRequest revoke;
  revoke.database_uuid = context.database_uuid;
  revoke.expected_snapshot_uuid =
      current_availability.snapshot.snapshot_uuid;
  revoke.expected_generation = current_availability.snapshot.generation;
  revoke.exact_row_identity = availability_identity;
  revoke.requested_state = api::SblrExecutorAvailabilityState::revoked;
  revoke.reason_code = "CSC-TEST-003612-postpublication-replay";
  const auto revoked = api::SetSblrExecutorAvailability(context, revoke);
  Require(revoked.ok && !revoked.snapshot.installed &&
              revoked.snapshot.availability_state ==
                  api::SblrExecutorAvailabilityState::revoked,
          "003612 postpublication availability revocation was not durable");

  // Postpublication cancellation does not change the exact replay.
  probes.store(0, std::memory_order_relaxed);
  cancel.store(true, std::memory_order_relaxed);
  const auto replay_status = dispatch(success, &result);
  if (replay_status != SB_ENGINE_STATUS_OK && result != nullptr) {
    std::cerr << "003612 terminal replay: " << DiagnosticCode(result) << ':'
              << DiagnosticKey(result) << '\n';
  }
  Require(replay_status == SB_ENGINE_STATUS_OK &&
              result != nullptr && ResultPayload(result) == result_bytes &&
              probes.load(std::memory_order_relaxed) == 0,
          "003612 postpublication replay was not byte-identical and cancellation-stable");
  (void)sb_engine_result_release(result);

  // Terminal-result replay does not grant authority to use a retired
  // transaction/statement handle. The common snapshot lifecycle gate remains
  // mandatory even though the catalog result itself is immutable.
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "003612 owning transaction rollback after publication failed");
  Require(dispatch(success, &result) == SB_ENGINE_STATUS_CONFLICT &&
              result != nullptr &&
              DiagnosticCode(result) == "ENGINE.STATEMENT_CONTEXT.SNAPSHOT_STALE" &&
              ResultPayload(result).empty(),
          "003612 terminal result bypassed retired transaction snapshot authority");
  (void)sb_engine_result_release(result);
  cancel.store(false, std::memory_order_relaxed);

  for (auto* attempt : {&cancelled, &success})
    Require(bridge::ReleaseStatementContextReceipt(attempt->receipt) ==
                SB_ENGINE_STATUS_OK,
            "003612 statement receipt cleanup failed");
}

int main() {
  VerifyCatalogSnapshot(false);
  VerifyCatalogSnapshot(true);
  return EXIT_SUCCESS;
}
