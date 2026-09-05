// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission and source-free VALUES
// fixture. No public ABI, gateway, registry, transaction, or parser binding
// service is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "engine/internal_api/sblr_parse_text_journal.hpp"
#include "engine/sblr/sblr_parse_text_runtime.hpp"

#include <atomic>
#include <cstdlib>
#include <string_view>

namespace {

sblr::SblrOperationEnvelope ParseTextMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const std::vector<std::uint8_t>& descriptor_bytes) {
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.parse_text", "SBLR_PARSE_TEXT",
      "ia05.parse_text.cancellation_atomicity");
  member.opcode_code = sblr::kSblrParseTextOpcodeCode;
  member.result_shape = "parse_text_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = false;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "parse_text_descriptor";
  operand.name = "text";
  operand.value_kind = sblr::SblrValueKind::parse_text_descriptor;
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
  context.language_context.language_resource_epoch = 1;
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 36280));
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
          "003628 live statement receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(view.parse_text_executor_availability_generation != 0 &&
              !view.parse_text_language_profile_uuid.empty() &&
              view.parse_text_language_profile_generation != 0,
          "003628 receipt omitted parse-text authority");

  const auto literal_binding = literal_fixture::FinalizeLiteral(receipt, view);
  const auto nested_submission = PackageWithMember(
      fixture, view, parser_uuid,
      SourceFreeValuesQueryMember(view, parser_uuid, literal_binding));

  bind::ParseTextBindRequestV1 public_bind;
  public_bind.authenticated_receipt_uuid = RawUuid(view.receipt_uuid);
  public_bind.occurrence = 1;
  public_bind.language_profile_id = context.language_context.language_profile_id;
  public_bind.canonical_input_utf8 = "SELECT 1;";
  public_bind.requested_maximum_bytes = 4096;
  public_bind.requested_maximum_depth = 64;
  public_bind.allow_donor_extensions = false;
  public_bind.canonical_container_bytes.assign(nested_submission.container.begin(),
                                               nested_submission.container.end());
  public_bind.canonical_execution_envelope_bytes.assign(
      nested_submission.ingress.begin(), nested_submission.ingress.end());
  std::vector<std::uint8_t> exact_bind;
  std::string detail;
  Require(bind::EncodeParseTextBindRequestV1(
              public_bind, &exact_bind, &detail),
          "003628 canonical private bind request encoding failed");
  bind::ParseTextBindRequestV1 decoded_bind;
  Require(bind::DecodeParseTextBindRequestV1(
              exact_bind.data(), exact_bind.size(), &decoded_bind, &detail),
          "003628 canonical private bind request decoding failed");

  bridge::StatementParseTextBindRequestV1 bind_request;
  bind_request.authenticated_receipt_uuid = view.receipt_uuid;
  bind_request.occurrence = decoded_bind.occurrence;
  bind_request.language_profile_id = decoded_bind.language_profile_id;
  bind_request.canonical_input_utf8 = decoded_bind.canonical_input_utf8;
  bind_request.requested_maximum_bytes =
      decoded_bind.requested_maximum_bytes;
  bind_request.requested_maximum_depth =
      decoded_bind.requested_maximum_depth;
  bind_request.allow_donor_extensions =
      decoded_bind.allow_donor_extensions;
  bind_request.canonical_container_bytes =
      decoded_bind.canonical_container_bytes;
  bind_request.canonical_execution_envelope_bytes =
      decoded_bind.canonical_execution_envelope_bytes;
  bind_request.canonical_input_sha256 =
      decoded_bind.canonical_input_sha256;
  bind_request.canonical_container_sha256 =
      decoded_bind.canonical_container_sha256;
  bind_request.canonical_execution_envelope_sha256 =
      decoded_bind.canonical_execution_envelope_sha256;
  bind_request.request_evidence_sha256 = decoded_bind.request_evidence_sha256;
  bind_request.exact_bind_request_bytes = exact_bind;

  bridge::StatementParseTextBindAckV1 bind_ack;
  result = nullptr;
  Require(bridge::BindStatementParseTextAuthorityV1(
              receipt, &bind_request, &bind_ack, &result) ==
              SB_ENGINE_STATUS_OK,
          "003628 engine parse-text binding failed");
  if (result != nullptr) (void)sb_engine_result_release(result);

  bridge::StatementParseTextAuthorityV1 authority;
  result = nullptr;
  Require(bridge::CopyStatementParseTextAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.canonical_descriptor_bytes.empty() &&
              authority.exact_bind_request_bytes == exact_bind &&
              authority.acknowledgement.parse_uuid == bind_ack.parse_uuid,
          "003628 engine parse-text descriptor was not published");
  if (result != nullptr) (void)sb_engine_result_release(result);

  sblr::SblrParseTextDescriptorV1 descriptor;
  Require(sblr::DecodeSblrParseTextDescriptorV1(
              authority.canonical_descriptor_bytes.data(),
              authority.canonical_descriptor_bytes.size(), &descriptor,
              &detail),
          "003628 engine SPTD did not decode");
  auto journal_context = context;
  journal_context.statement_metadata_snapshot_engine_owned = true;
  journal_context.trace_tags.push_back("private_parse_text_journal");
  api::SblrParseTextJournalKeyV1 journal_key;
  journal_key.database_uuid = RawUuid(context.database_uuid.canonical);
  journal_key.statement_receipt_uuid = descriptor.statement_receipt_uuid;
  journal_key.parse_uuid = descriptor.parse_uuid;
  journal_key.descriptor_sha256 = descriptor.descriptor_sha256;
  journal_key.canonical_input_sha256 = descriptor.canonical_input_sha256;
  journal_key.language_profile_uuid = descriptor.language_profile_uuid;
  journal_key.language_profile_generation =
      descriptor.language_profile_generation;
  journal_key.catalog_generation = descriptor.catalog_generation;
  journal_key.security_epoch = descriptor.security_epoch;
  journal_key.resource_epoch = descriptor.resource_epoch;
  journal_key.executor_availability_generation =
      descriptor.executor_availability_generation;

  const auto submission = PackageWithMember(
      fixture, view, parser_uuid,
      ParseTextMember(view, parser_uuid,
                      authority.canonical_descriptor_bytes));

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle first_reservation;
  auto first_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                              submission, &first_reservation);
  result = nullptr;
  const auto first_status =
      bridge::DispatchStatementContextReceipt(&first_dispatch, &result);
  if (first_status != SB_ENGINE_STATUS_TIMEOUT || result == nullptr) {
    std::cerr << "003628 first cancellation dispatch: status="
              << sb_engine_status_name(first_status)
              << ";probes=" << probes.load(std::memory_order_relaxed);
    if (result != nullptr) {
      std::cerr << ";code=" << DiagnosticCode(result)
                << ";key=" << DiagnosticKey(result);
    }
    std::cerr << '\n';
  }
  Require(first_status == SB_ENGINE_STATUS_TIMEOUT && result != nullptr,
          "003628 first prepublication cancellation did not stop PARSE_TEXT");
  const auto first_code = DiagnosticCode(result);
  const auto first_key = DiagnosticKey(result);
  const auto first_probes = probes.load(std::memory_order_relaxed);
  if (first_code != "PROCESS.CANCELLED" ||
      first_key != "sblr.parse_text.cancelled_before_publication" ||
      first_probes != 1) {
    std::cerr << "003628 first cancellation observation: code=" << first_code
              << ";key=" << first_key << ";probes=" << first_probes << '\n';
  }
  Require(first_code == "PROCESS.CANCELLED" &&
              first_key ==
                  "sblr.parse_text.cancelled_before_publication" &&
              first_probes == 1,
          "003628 first cancellation precedence drifted");
  (void)sb_engine_result_release(result);
  result = nullptr;
  authority = {};
  Require(bridge::CopyStatementParseTextAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes.empty(),
          "003628 first cancellation published a receipt result");
  if (result != nullptr) (void)sb_engine_result_release(result);
  const auto begun =
      api::LookupSblrParseTextJournalV1(journal_context, journal_key);
  Require(begun.ok && begun.found &&
              begun.snapshot.state ==
                  api::SblrParseTextJournalStateV1::begun &&
              begun.snapshot.journal_generation == 1 &&
              begun.snapshot.canonical_result_bytes.empty(),
          "003628 first cancellation did not retain only durable identity");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(2, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle second_reservation;
  auto second_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               submission, &second_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&second_dispatch, &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr,
          "003628 second prepublication cancellation did not stop PARSE_TEXT");
  Require(DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.parse_text.cancelled_before_publication" &&
              probes.load(std::memory_order_relaxed) == 2,
          "003628 second cancellation precedence drifted");
  (void)sb_engine_result_release(result);
  const auto still_begun =
      api::LookupSblrParseTextJournalV1(journal_context, journal_key);
  Require(still_begun.ok && still_begun.found &&
              still_begun.snapshot.state ==
                  api::SblrParseTextJournalStateV1::begun &&
              still_begun.snapshot.journal_generation == 1 &&
              still_begun.snapshot.canonical_result_bytes.empty(),
          "003628 second cancellation crossed the publication barrier");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle success_reservation;
  auto success_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                submission, &success_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&success_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003628 retry did not publish PARSE_TEXT result");
  const auto first_result = ResultPayload(result);
  sblr::SblrParseTextResultV1 decoded_result;
  Require(sblr::DecodeSblrParseTextResultV1(
              first_result.data(), first_result.size(), &decoded_result,
              &detail) &&
              decoded_result.parse_uuid == descriptor.parse_uuid &&
              decoded_result.canonical_sblr_bytes ==
                  descriptor.canonical_sblr_bytes &&
              decoded_result.status == 1 &&
              decoded_result.publication_barrier == 1 &&
              probes.load(std::memory_order_relaxed) == 2,
          "003628 successful retry omitted canonical SPTR");
  (void)sb_engine_result_release(result);
  result = nullptr;
  authority = {};
  Require(bridge::CopyStatementParseTextAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes == first_result,
          "003628 terminal result was not retained for exact replay");
  if (result != nullptr) (void)sb_engine_result_release(result);
  const auto published =
      api::LookupSblrParseTextJournalV1(journal_context, journal_key);
  Require(published.ok && published.found &&
              published.snapshot.state ==
                  api::SblrParseTextJournalStateV1::published &&
              published.snapshot.journal_generation == 2 &&
              published.snapshot.canonical_result_bytes == first_result,
          "003628 durable journal did not recover exact SPTR");

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
          "003628 postpublication cancellation retracted exact replay");
  (void)sb_engine_result_release(result);

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "003628 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "003628 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
