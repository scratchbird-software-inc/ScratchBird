// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission fixture. No public ABI,
// gateway, transaction, executor registry, or private bind service is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "engine/internal_api/sblr_database_attach_journal.hpp"
#include "engine/sblr/sblr_database_attach_runtime.hpp"

#include <atomic>
#include <cstdlib>
#include <string_view>

namespace {

Fixture CreateDatabaseAttachFixture() {
  Fixture fixture;
  const auto salt = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fixture.directory = std::filesystem::temp_directory_path() /
      ("sbsql_sblr_database_attach_cancel_" + std::to_string(salt));
  std::filesystem::create_directories(fixture.directory);
  fixture.database_path = fixture.directory / "fault.sbdb";
  fixture.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  fixture.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  fixture.principal_uuid = NewUuid(platform::UuidKind::principal, salt + 3);
  fixture.session_uuid = NewUuid(platform::UuidKind::session, salt + 4);
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.creation_unix_epoch_millis =
      1950000000000ull + salt % 1'000'000ull;
  create.page_size = 16384;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "003636 fixture database creation failed");
  fixture.resource_epoch =
      created.state.resource_seed_catalog.resource_epoch == 0
          ? 1
          : created.state.resource_seed_catalog.resource_epoch;
  const auto opened = db::OpenDatabaseFile(
      {fixture.database_path.string(), false, false, false});
  Require(opened.ok(), "003636 fixture lifecycle open failed");
  Require(db::MarkDatabaseCleanShutdown(fixture.database_path.string()).ok(),
          "003636 fixture lifecycle clean shutdown failed");
  return fixture;
}

sblr::SblrOperationEnvelope DatabaseAttachMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const std::vector<std::uint8_t>& descriptor_bytes) {
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.database_attach", "SBLR_DATABASE_ATTACH",
      "ia08.database_attach.cancellation_atomicity");
  member.opcode_code = sblr::kSblrDatabaseAttachOpcodeCode;
  member.result_shape = "database_attach_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "database_attach_descriptor";
  operand.name = "attachment";
  operand.value_kind = sblr::SblrValueKind::database_attach_descriptor;
  operand.value_body = descriptor_bytes;
  member.operands.push_back(std::move(operand));
  return member;
}

}  // namespace

int main() {
  auto fixture = CreateDatabaseAttachFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  std::atomic<unsigned> cancel_on_probe{0};
  auto context = BeginTransaction(fixture, &probes);
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 36350));
  context.current_package_uuid.canonical = parser_uuid;
  context.authorization_context.security_context_generation = 1;
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object, 36351));
  grant.subject_uuid = context.principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid = context.database_uuid;
  grant.right = "FILESPACE_LIFECYCLE_CONTROL";
  grant.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(std::move(grant));
  context.query_cancellation_requested = [&] {
    const auto ordinal = probes.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto target = cancel_on_probe.load(std::memory_order_relaxed);
    return target != 0 && ordinal == target;
  };

  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t result = nullptr;
  Require(bridge::AcquireStatementContextReceipt(
              session.session, &acquire, &receipt, &view, &result) ==
              SB_ENGINE_STATUS_OK,
          "003636 live statement receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(view.database_attach_executor_availability_generation != 0 &&
              !view.resource_admission_uuid.empty() &&
              view.resource_epoch != 0,
          "003636 receipt omitted database-attach authority");

  bind::DatabaseAttachBindRequestV1 public_bind;
  public_bind.authenticated_receipt_uuid = RawUuid(view.receipt_uuid);
  public_bind.occurrence = 1;
  public_bind.mode = 1;
  public_bind.alias_scope = 1;
  public_bind.storage_reference = {"CURRENT", false};
  public_bind.database_alias = {"cancellation_alias", false};
  std::vector<std::uint8_t> exact_bind;
  std::string detail;
  Require(bind::EncodeDatabaseAttachBindRequestV1(
              public_bind, &exact_bind, &detail),
          "003636 canonical private bind request encoding failed");
  bind::DatabaseAttachBindRequestV1 decoded_bind;
  Require(bind::DecodeDatabaseAttachBindRequestV1(
              exact_bind.data(), exact_bind.size(), &decoded_bind, &detail),
          "003636 canonical private bind request decoding failed");

  bridge::StatementDatabaseAttachBindRequestV1 bind_request;
  bind_request.authenticated_receipt_uuid = view.receipt_uuid;
  bind_request.occurrence = decoded_bind.occurrence;
  bind_request.mode = decoded_bind.mode;
  bind_request.alias_scope = decoded_bind.alias_scope;
  bind_request.storage_reference = {
      decoded_bind.storage_reference.raw_utf8,
      decoded_bind.storage_reference.quoted};
  bind_request.database_alias = {decoded_bind.database_alias.raw_utf8,
                                 decoded_bind.database_alias.quoted};
  bind_request.request_evidence_sha256 =
      decoded_bind.request_evidence_sha256;
  bind_request.exact_bind_request_bytes = exact_bind;

  bridge::StatementDatabaseAttachBindAckV1 bind_ack;
  result = nullptr;
  const auto bind_status = bridge::BindStatementDatabaseAttachAuthorityV1(
      receipt, &bind_request, &bind_ack, &result);
  if (bind_status != SB_ENGINE_STATUS_OK) {
    std::cerr << "003636 bind refusal: status="
              << sb_engine_status_name(bind_status)
              << " code=" << (result == nullptr ? "" : DiagnosticCode(result))
              << " key=" << (result == nullptr ? "" : DiagnosticKey(result))
              << '\n';
  }
  Require(bind_status == SB_ENGINE_STATUS_OK,
          "003636 engine database-attach binding failed");
  if (result != nullptr) (void)sb_engine_result_release(result);

  bridge::StatementDatabaseAttachAuthorityV1 authority;
  result = nullptr;
  Require(bridge::CopyStatementDatabaseAttachAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.canonical_descriptor_bytes.empty() &&
              authority.exact_bind_request_bytes == exact_bind &&
              authority.acknowledgement.attach_uuid == bind_ack.attach_uuid,
          "003636 engine database-attach descriptor was not published");
  if (result != nullptr) (void)sb_engine_result_release(result);

  sblr::SblrDatabaseAttachDescriptorV1 descriptor;
  Require(sblr::DecodeSblrDatabaseAttachDescriptorV1(
              authority.canonical_descriptor_bytes.data(),
              authority.canonical_descriptor_bytes.size(), &descriptor,
              &detail) &&
              descriptor.statement_receipt_uuid == RawUuid(view.receipt_uuid) &&
              descriptor.mode == 1 && descriptor.alias_scope == 1,
          "003636 engine SADD did not decode");

  api::EngineRequestContext journal_context;
  Require(bridge::CopyStatementContextEngineContextV1(
              receipt, &journal_context, nullptr) == SB_ENGINE_STATUS_OK,
          "003636 private statement context copy failed");
  journal_context.statement_metadata_snapshot_engine_owned = true;
  journal_context.trace_tags.push_back("private_database_attach_journal");
  api::SblrDatabaseAttachJournalKeyV1 journal_key;
  journal_key.database_uuid = descriptor.database_uuid;
  journal_key.session_uuid = RawUuid(journal_context.session_uuid.canonical);
  journal_key.statement_receipt_uuid = descriptor.statement_receipt_uuid;
  journal_key.attach_uuid = descriptor.attach_uuid;
  journal_key.storage_uuid = descriptor.storage_uuid;
  journal_key.alias_uuid = descriptor.alias_uuid;
  journal_key.alias_name_sha256 =
      api::SblrDatabaseAttachAliasNameSha256V1("cancellation_alias", false);
  journal_key.descriptor_sha256 = descriptor.descriptor_sha256;
  journal_key.storage_alias_binding_sha256 =
      descriptor.storage_alias_binding_sha256;
  journal_key.catalog_snapshot_uuid = descriptor.catalog_snapshot_uuid;
  journal_key.catalog_generation = descriptor.catalog_generation;
  journal_key.security_context_uuid = descriptor.security_context_uuid;
  journal_key.security_epoch = view.security_epoch;
  journal_key.policy_snapshot_uuid = descriptor.policy_snapshot_uuid;
  journal_key.policy_generation = descriptor.policy_generation;
  journal_key.transaction_uuid = descriptor.transaction_uuid;
  journal_key.transaction_generation = descriptor.transaction_generation;
  journal_key.mode = descriptor.mode;
  journal_key.alias_scope = descriptor.alias_scope;
  journal_key.resource_admission_uuid = RawUuid(view.resource_admission_uuid);
  journal_key.resource_epoch = view.resource_epoch;
  journal_key.executor_availability_generation =
      descriptor.executor_availability_generation;

  const auto submission = PackageWithMember(
      fixture, view, parser_uuid,
      DatabaseAttachMember(view, parser_uuid,
                           authority.canonical_descriptor_bytes));

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle first_reservation;
  auto first_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                              submission, &first_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&first_dispatch, &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr &&
              DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.database_attach.cancelled_before_revalidation" &&
              probes.load(std::memory_order_relaxed) == 1,
          "003636 first cancellation crossed storage revalidation");
  (void)sb_engine_result_release(result);
  result = nullptr;
  authority = {};
  Require(bridge::CopyStatementDatabaseAttachAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes.empty(),
          "003636 first cancellation published a receipt result");
  if (result != nullptr) (void)sb_engine_result_release(result);
  const auto begun = api::LookupSblrDatabaseAttachJournalV1(
      journal_context, journal_key);
  Require(begun.ok && begun.found &&
              begun.snapshot.state ==
                  api::SblrDatabaseAttachJournalStateV1::begun &&
              begun.snapshot.journal_generation == 1 &&
              begun.snapshot.canonical_result_bytes.empty(),
          "003636 first cancellation did not retain durable identity");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(2, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle second_reservation;
  auto second_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               submission, &second_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&second_dispatch, &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr &&
              DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.database_attach.cancelled_before_publication" &&
              probes.load(std::memory_order_relaxed) == 2,
          "003636 second cancellation crossed the publication barrier");
  (void)sb_engine_result_release(result);
  const auto still_begun = api::LookupSblrDatabaseAttachJournalV1(
      journal_context, journal_key);
  Require(still_begun.ok && still_begun.found &&
              still_begun.snapshot.state ==
                  api::SblrDatabaseAttachJournalStateV1::begun &&
              still_begun.snapshot.journal_generation == 1 &&
              still_begun.snapshot.canonical_result_bytes.empty(),
          "003636 prepublication cancellation changed durable result state");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle success_reservation;
  auto success_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                submission, &success_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&success_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003636 retry did not publish database-attach result");
  const auto first_result = ResultPayload(result);
  sblr::SblrDatabaseAttachResultV1 decoded_result;
  Require(sblr::DecodeSblrDatabaseAttachResultV1(
              first_result.data(), first_result.size(), &decoded_result,
              &detail) &&
              decoded_result.attach_uuid == descriptor.attach_uuid &&
              decoded_result.database_uuid == descriptor.database_uuid &&
              decoded_result.alias_uuid == descriptor.alias_uuid &&
              decoded_result.status == 1 &&
              decoded_result.lifecycle_state == 1 &&
              decoded_result.publication_barrier == 1 &&
              probes.load(std::memory_order_relaxed) == 2,
          "003636 successful retry omitted canonical SBAR");
  (void)sb_engine_result_release(result);
  result = nullptr;
  authority = {};
  Require(bridge::CopyStatementDatabaseAttachAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes == first_result,
          "003636 terminal result was not retained for exact replay");
  if (result != nullptr) (void)sb_engine_result_release(result);
  const auto published = api::LookupSblrDatabaseAttachJournalV1(
      journal_context, journal_key);
  Require(published.ok && published.found &&
              published.snapshot.state ==
                  api::SblrDatabaseAttachJournalStateV1::published &&
              published.snapshot.journal_generation == 2 &&
              published.snapshot.canonical_result_bytes == first_result,
          "003636 durable journal did not recover exact SBAR");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle replay_reservation;
  auto replay_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               submission, &replay_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&replay_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr && ResultPayload(result) == first_result &&
              probes.load(std::memory_order_relaxed) == 0,
          "003636 postpublication cancellation retracted exact replay");
  (void)sb_engine_result_release(result);

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "003636 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "003636 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
