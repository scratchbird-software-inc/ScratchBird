// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission fixture. No public ABI,
// package gateway, executor registry, or statement receipt is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "engine/internal_api/ddl/create_api.hpp"
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

api::EngineColumnDefinition CatalogColumn(std::uint32_t ordinal,
                                          std::string value,
                                          std::string type) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.names.push_back(CatalogName(std::move(value)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  column.descriptor.encoded_descriptor =
      "type=" + column.descriptor.canonical_type_name;
  column.nullable = ordinal != 0;
  return column;
}

void SeedCatalogObject(api::EngineRequestContext context) {
  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid.canonical =
      Text(NewUuid(platform::UuidKind::schema, 36121));
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(CatalogName("catalog_probe_schema"));
  const auto created_schema = api::EngineCreateSchema(schema);
  Require(created_schema.ok,
          "003612 catalog-introspect fixture schema creation failed");

  api::EngineCreateTableRequest table;
  table.context = context;
  table.target_schema = schema.target_object;
  table.requested_table_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object, 36122));
  table.table_names.push_back(CatalogName("catalog_probe_table"));
  table.table_columns.push_back(CatalogColumn(0, "id", "int64"));
  table.table_columns.push_back(CatalogColumn(1, "value", "int64"));
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
    std::string_view parser_uuid,
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

int main() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<bool> cancel{false};
  std::atomic<unsigned> probes{0};
  auto context = BeginTransaction(fixture, &probes);
  context.trace_tags.push_back("right:SBLR_EXECUTOR_AVAILABILITY_ADMIN");
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 36120));
  context.current_package_uuid.canonical = parser_uuid;
  context.query_cancellation_requested = [&] {
    probes.fetch_add(1, std::memory_order_relaxed);
    return cancel.load(std::memory_order_relaxed);
  };
  api::EngineMaterializedAuthorizationGrant select_grant;
  select_grant.grant_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object, 36123));
  select_grant.subject_uuid = context.principal_uuid;
  select_grant.subject_kind = "principal";
  select_grant.right = "SELECT";
  select_grant.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(std::move(select_grant));

  // This fixture enters DDL through the private engine API. Obtain the exact
  // datatype registry cohort from the same receipt producer used by public
  // statements; the test never authors datatype snapshot identity.
  bridge::StatementContextAcquireRequest seed_acquire;
  seed_acquire.engine_context = &context;
  seed_acquire.exact_transaction_uuid = context.transaction_uuid.canonical;
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
              !seed_context.datatype_catalog_snapshot_uuid.canonical.empty() &&
              seed_context.datatype_catalog_generation != 0 &&
              seed_context.datatype_registry_generation != 0 &&
              seed_context.datatype_catalog_snapshot_uuid.canonical ==
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
  SeedCatalogObject(context);

  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  result = nullptr;
  Require(bridge::AcquireStatementContextReceipt(
              session.session, &acquire, &receipt, &view, &result) ==
              SB_ENGINE_STATUS_OK,
          "003612 live statement receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(view.catalog_introspect_executor_availability_generation != 0,
          "003612 receipt omitted catalog-introspect availability");

  bind::NameResolveBindRequestV1 public_bind;
  public_bind.authenticated_receipt_uuid = RawUuid(view.receipt_uuid);
  public_bind.occurrence = 1;
  public_bind.resolution_mode = 2;
  public_bind.object_class = 2;
  public_bind.target_name_atoms = {{"catalog_probe_schema", false},
                                   {"catalog_probe_table", false}};
  std::vector<std::uint8_t> exact_bind;
  std::string detail;
  Require(bind::EncodeNameResolveBindRequestV1(
              public_bind, &exact_bind, &detail),
          "003612 canonical target-name bind encoding failed");
  bind::NameResolveBindRequestV1 decoded_bind;
  Require(bind::DecodeNameResolveBindRequestV1(
              exact_bind.data(), exact_bind.size(), &decoded_bind, &detail),
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
  bind_request.request_evidence_sha256 = decoded_bind.request_evidence_sha256;
  bind_request.exact_bind_request_bytes = exact_bind;
  bridge::StatementNameResolveBindAckV1 bind_ack;
  result = nullptr;
  Require(bridge::BindStatementNameResolveAuthorityV1(
              receipt, &bind_request, &bind_ack, &result) ==
              SB_ENGINE_STATUS_OK,
          "003612 engine target-name binding failed");
  if (result != nullptr) (void)sb_engine_result_release(result);

  bridge::StatementCatalogIntrospectAuthorityV1 authority;
  result = nullptr;
  const auto catalog_bind_status =
      bridge::BindStatementCatalogIntrospectAuthorityV1(
          receipt, 1, 1, &authority, &result);
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
  auto descriptor_bytes = authority.canonical_descriptor_bytes;
  std::copy_n("CIDO", 4, descriptor_bytes.begin());
  sblr::SblrCatalogIntrospectDescriptorV1 decoded_descriptor;
  Require(sblr::DecodeSblrCatalogIntrospectDescriptorV1(
              descriptor_bytes.data(), descriptor_bytes.size(),
              &decoded_descriptor, &detail, true) &&
              decoded_descriptor.evidence ==
                  authority.descriptor_evidence_sha256,
          "003612 literal receipt CIDD-to-CIDO projection failed");
  const auto submission = PackageWithMember(
      fixture, view, parser_uuid,
      CatalogIntrospectMember(view, parser_uuid, descriptor_bytes));

  probes.store(0, std::memory_order_relaxed);
  cancel.store(true, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle cancel_reservation;
  auto cancel_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               submission, &cancel_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&cancel_dispatch, &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
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

  probes.store(0, std::memory_order_relaxed);
  cancel.store(false, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle success_reservation;
  auto success_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                submission, &success_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&success_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003612 retry did not publish catalog-introspect result");
  const auto result_bytes = ResultPayload(result);
  sblr::SblrCatalogIntrospectResultV1 decoded_result;
  Require(result_bytes.size() == 320 &&
              sblr::DecodeSblrCatalogIntrospectResultV1(
                  result_bytes.data(), result_bytes.size(), &decoded_result,
                  &detail) &&
              decoded_result.availability ==
                  view.catalog_introspect_executor_availability_generation,
          "003612 successful retry omitted canonical CIRS");
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
  revoke.database_uuid = context.database_uuid.canonical;
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

  probes.store(0, std::memory_order_relaxed);
  cancel.store(true, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle replay_reservation;
  auto replay_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               submission, &replay_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&replay_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr && ResultPayload(result) == result_bytes &&
              probes.load(std::memory_order_relaxed) == 0,
          "003612 postpublication replay was not byte-identical and cancellation-stable");
  (void)sb_engine_result_release(result);

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "003612 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "003612 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
