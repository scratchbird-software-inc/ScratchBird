// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission and source-free VALUES
// fixture. No public ABI, gateway, registry, transaction, or parser binding
// service is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "engine/internal_api/sblr_catalog_epoch_check_journal.hpp"
#include "engine/sblr/sblr_catalog_epoch_check_runtime.hpp"

#include <atomic>
#include <cstdlib>
#include <string_view>

namespace {

sblr::SblrOperationEnvelope CatalogEpochCheckMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const std::vector<std::uint8_t>& descriptor_bytes) {
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.catalog_epoch_check", "SBLR_CATALOG_EPOCH_CHECK",
      "ia05.catalog_epoch_check.cancellation_atomicity");
  member.opcode_code = sblr::kSblrCatalogEpochCheckOpcodeCode;
  member.result_shape = "catalog_epoch_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "catalog_epoch_check_descriptor";
  operand.name = "catalog_epoch";
  operand.value_kind = sblr::SblrValueKind::catalog_epoch_check_descriptor;
  operand.value_body = descriptor_bytes;
  member.operands.push_back(std::move(operand));
  return member;
}

}  // namespace

int main() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  std::atomic<unsigned> cancel_on_probe{0};
  auto context = BeginTransaction(fixture, &probes);
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 36320));
  context.current_package_uuid.canonical = parser_uuid;
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
          "003632 live statement receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(view.catalog_epoch_check_executor_availability_generation != 0 &&
              !view.catalog_epoch_check_redaction_profile_uuid.empty() &&
              view.catalog_epoch_check_redaction_generation != 0 &&
              !view.catalog_epoch_check_policy_snapshot_uuid.empty() &&
              view.catalog_epoch_check_policy_generation != 0,
          "003632 receipt omitted catalog-epoch authority");

  bind::CatalogEpochCheckBindRequestV1 public_bind;
  public_bind.authenticated_receipt_uuid = RawUuid(view.receipt_uuid);
  public_bind.occurrence = 1;
  public_bind.object_scoped = false;
  std::vector<std::uint8_t> exact_bind;
  std::string detail;
  Require(bind::EncodeCatalogEpochCheckBindRequestV1(
              public_bind, &exact_bind, &detail),
          "003632 canonical private bind request encoding failed");
  bind::CatalogEpochCheckBindRequestV1 decoded_bind;
  Require(bind::DecodeCatalogEpochCheckBindRequestV1(
              exact_bind.data(), exact_bind.size(), &decoded_bind, &detail),
          "003632 canonical private bind request decoding failed");

  bridge::StatementCatalogEpochCheckBindRequestV1 bind_request;
  bind_request.authenticated_receipt_uuid = view.receipt_uuid;
  bind_request.occurrence = decoded_bind.occurrence;
  bind_request.object_scoped = decoded_bind.object_scoped;
  for (const auto& atom : decoded_bind.target_name_atoms) {
    bind_request.target_name_atoms.push_back({atom.raw_utf8, atom.quoted});
  }
  bind_request.target_name_atoms_sha256 =
      decoded_bind.target_name_atoms_sha256;
  bind_request.request_evidence_sha256 = decoded_bind.request_evidence_sha256;
  bind_request.exact_bind_request_bytes = exact_bind;

  bridge::StatementCatalogEpochCheckBindAckV1 bind_ack;
  result = nullptr;
  Require(bridge::BindStatementCatalogEpochCheckAuthorityV1(
              receipt, &bind_request, &bind_ack, &result) ==
              SB_ENGINE_STATUS_OK,
          "003632 engine catalog-epoch binding failed");
  if (result != nullptr) (void)sb_engine_result_release(result);

  bridge::StatementCatalogEpochCheckAuthorityV1 authority;
  result = nullptr;
  Require(bridge::CopyStatementCatalogEpochCheckAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.canonical_descriptor_bytes.empty() &&
              authority.exact_bind_request_bytes == exact_bind &&
              authority.acknowledgement.check_uuid == bind_ack.check_uuid,
          "003632 engine catalog-epoch descriptor was not published");
  if (result != nullptr) (void)sb_engine_result_release(result);

  sblr::SblrCatalogEpochCheckDescriptorV1 descriptor;
  Require(sblr::DecodeSblrCatalogEpochCheckDescriptorV1(
              authority.canonical_descriptor_bytes.data(),
              authority.canonical_descriptor_bytes.size(), &descriptor,
              &detail) &&
              !descriptor.object_scoped &&
              descriptor.statement_receipt_uuid == RawUuid(view.receipt_uuid),
          "003632 engine SECD did not decode");

  auto journal_context = context;
  journal_context.statement_metadata_snapshot_engine_owned = true;
  journal_context.trace_tags.push_back("private_catalog_epoch_check_journal");
  api::SblrCatalogEpochCheckJournalKeyV1 journal_key;
  journal_key.database_uuid = RawUuid(context.database_uuid.canonical);
  journal_key.statement_receipt_uuid = descriptor.statement_receipt_uuid;
  journal_key.check_uuid = descriptor.check_uuid;
  journal_key.descriptor_sha256 = descriptor.descriptor_sha256;
  journal_key.visibility_scope_sha256 = descriptor.visibility_scope_sha256;
  journal_key.requested_catalog_epoch_uuid =
      descriptor.requested_catalog_epoch_uuid;
  journal_key.requested_catalog_generation =
      descriptor.requested_catalog_generation;
  journal_key.schema_tree_generation = descriptor.schema_tree_generation;
  journal_key.security_epoch = descriptor.security_epoch;
  journal_key.resource_epoch = descriptor.resource_epoch;
  journal_key.executor_availability_generation =
      descriptor.executor_availability_generation;

  const auto submission = PackageWithMember(
      fixture, view, parser_uuid,
      CatalogEpochCheckMember(view, parser_uuid,
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
                  "sblr.catalog_epoch_check.cancelled_before_observation" &&
              probes.load(std::memory_order_relaxed) == 1,
          "003632 first cancellation crossed catalog observation");
  (void)sb_engine_result_release(result);
  result = nullptr;
  authority = {};
  Require(bridge::CopyStatementCatalogEpochCheckAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes.empty(),
          "003632 first cancellation published a receipt result");
  if (result != nullptr) (void)sb_engine_result_release(result);
  const auto begun = api::LookupSblrCatalogEpochCheckJournalV1(
      journal_context, journal_key);
  Require(begun.ok && begun.found &&
              begun.snapshot.state ==
                  api::SblrCatalogEpochCheckJournalStateV1::begun &&
              begun.snapshot.journal_generation == 1 &&
              begun.snapshot.canonical_result_bytes.empty(),
          "003632 first cancellation did not retain durable identity");

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
                  "sblr.catalog_epoch_check.cancelled_before_publication" &&
              probes.load(std::memory_order_relaxed) == 2,
          "003632 second cancellation crossed the publication barrier");
  (void)sb_engine_result_release(result);
  const auto still_begun = api::LookupSblrCatalogEpochCheckJournalV1(
      journal_context, journal_key);
  Require(still_begun.ok && still_begun.found &&
              still_begun.snapshot.state ==
                  api::SblrCatalogEpochCheckJournalStateV1::begun &&
              still_begun.snapshot.journal_generation == 1 &&
              still_begun.snapshot.canonical_result_bytes.empty(),
          "003632 prepublication cancellation changed durable result state");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle success_reservation;
  auto success_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                submission, &success_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&success_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003632 retry did not publish catalog-epoch result");
  const auto first_result = ResultPayload(result);
  sblr::SblrCatalogEpochCheckResultV1 decoded_result;
  Require(sblr::DecodeSblrCatalogEpochCheckResultV1(
              first_result.data(), first_result.size(), &decoded_result,
              &detail) &&
              decoded_result.check_uuid == descriptor.check_uuid &&
              decoded_result.status == 1 &&
              decoded_result.visibility == 1 &&
              decoded_result.observed_catalog_epoch_uuid ==
                  descriptor.requested_catalog_epoch_uuid &&
              decoded_result.observed_catalog_generation ==
                  descriptor.requested_catalog_generation &&
              probes.load(std::memory_order_relaxed) == 2,
          "003632 successful retry omitted canonical SECR");
  (void)sb_engine_result_release(result);
  result = nullptr;
  authority = {};
  Require(bridge::CopyStatementCatalogEpochCheckAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes == first_result,
          "003632 terminal result was not retained for exact replay");
  if (result != nullptr) (void)sb_engine_result_release(result);
  const auto published = api::LookupSblrCatalogEpochCheckJournalV1(
      journal_context, journal_key);
  Require(published.ok && published.found &&
              published.snapshot.state ==
                  api::SblrCatalogEpochCheckJournalStateV1::published &&
              published.snapshot.journal_generation == 2 &&
              published.snapshot.canonical_result_bytes == first_result,
          "003632 durable journal did not recover exact SECR");

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
          "003632 postpublication cancellation retracted exact replay");
  (void)sb_engine_result_release(result);

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "003632 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "003632 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
