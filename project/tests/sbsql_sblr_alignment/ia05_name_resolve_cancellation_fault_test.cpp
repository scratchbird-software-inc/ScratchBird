// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission fixture. No public ABI,
// package gateway, executor registry, transaction inventory, or name resolver
// is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "engine/internal_api/sblr_name_resolve_journal.hpp"
#include "engine/sblr/sblr_name_resolve_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string_view>

namespace {

sblr::SblrOperationEnvelope NameResolveMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const std::vector<std::uint8_t>& descriptor_bytes) {
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.name_resolve", "SBLR_NAME_RESOLVE",
      "ia05.name_resolve.cancellation_atomicity");
  member.opcode_code = sblr::kSblrNameResolveOpcodeCode;
  member.result_shape = "name_resolve_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "name_resolve_descriptor.v1";
  operand.name = "name";
  operand.value_kind = sblr::SblrValueKind::name_resolve_descriptor;
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
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 36160));
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
          "003616 live statement receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(view.name_resolve_executor_availability_generation != 0,
          "003616 receipt omitted name-resolve availability");

  bind::NameResolveBindRequestV1 public_bind;
  public_bind.authenticated_receipt_uuid = RawUuid(view.receipt_uuid);
  public_bind.occurrence = 1;
  public_bind.resolution_mode = 2;
  public_bind.object_class = 2;
  public_bind.target_name_atoms = {{"definitely_absent_name", false}};
  std::vector<std::uint8_t> exact_bind;
  std::string detail;
  Require(bind::EncodeNameResolveBindRequestV1(
              public_bind, &exact_bind, &detail),
          "003616 canonical private bind request encoding failed");
  bind::NameResolveBindRequestV1 decoded_bind;
  Require(bind::DecodeNameResolveBindRequestV1(
              exact_bind.data(), exact_bind.size(), &decoded_bind, &detail),
          "003616 canonical private bind request decoding failed");

  bridge::StatementNameResolveBindRequestV1 bind_request;
  bind_request.authenticated_receipt_uuid = view.receipt_uuid;
  bind_request.occurrence = decoded_bind.occurrence;
  bind_request.resolution_mode = decoded_bind.resolution_mode;
  bind_request.object_class = decoded_bind.object_class;
  for (const auto& source : decoded_bind.target_name_atoms) {
    bind_request.target_name_atoms.push_back({source.raw_utf8, source.quoted});
  }
  for (const auto& source : decoded_bind.namespace_name_atoms) {
    bind_request.namespace_name_atoms.push_back(
        {source.raw_utf8, source.quoted});
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
          "003616 engine name-resolve binding failed");
  if (result != nullptr) (void)sb_engine_result_release(result);

  bridge::StatementNameResolveAuthorityV1 authority;
  result = nullptr;
  Require(bridge::CopyStatementNameResolveAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.canonical_descriptor_bytes.empty() &&
              authority.exact_bind_request_bytes == exact_bind &&
              authority.acknowledgement.resolution_uuid ==
                  bind_ack.resolution_uuid,
          "003616 engine name-resolve descriptor was not published");
  if (result != nullptr) (void)sb_engine_result_release(result);

  sblr::SblrNameResolveDescriptorV1 decoded_descriptor;
  Require(sblr::DecodeSblrNameResolveDescriptorV1(
              authority.canonical_descriptor_bytes.data(),
              authority.canonical_descriptor_bytes.size(),
              &decoded_descriptor, &detail),
          "003616 engine SNRD did not decode");
  auto journal_context = context;
  journal_context.statement_metadata_snapshot_engine_owned = true;
  journal_context.trace_tags.push_back("private_name_resolve_journal");
  api::SblrNameResolveJournalKeyV1 journal_key;
  journal_key.database_uuid = RawUuid(context.database_uuid.canonical);
  journal_key.statement_receipt_uuid =
      decoded_descriptor.statement_receipt_uuid;
  journal_key.resolution_uuid = decoded_descriptor.resolution_uuid;
  journal_key.descriptor_sha256 = decoded_descriptor.descriptor_sha256;
  journal_key.canonical_name_sha256 =
      decoded_descriptor.canonical_name_sha256;
  journal_key.namespace_generation = decoded_descriptor.namespace_generation;
  journal_key.catalog_generation = decoded_descriptor.catalog_generation;
  journal_key.security_epoch = decoded_descriptor.security_epoch;
  journal_key.resource_epoch = decoded_descriptor.resource_epoch;

  const auto submission = PackageWithMember(
      fixture, view, parser_uuid,
      NameResolveMember(view, parser_uuid,
                        authority.canonical_descriptor_bytes));

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle lookup_reservation;
  auto lookup_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               submission, &lookup_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&lookup_dispatch, &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr,
          "003616 prelookup cancellation did not stop NAME_RESOLVE");
  const auto lookup_code = DiagnosticCode(result);
  const auto lookup_key = DiagnosticKey(result);
  const auto lookup_probes = probes.load(std::memory_order_relaxed);
  if (lookup_code != "PROCESS.CANCELLED" ||
      lookup_key != "sblr.name_resolve.cancelled_before_lookup" ||
      lookup_probes != 1) {
    std::cerr << "003616 prelookup observation: code=" << lookup_code
              << ";key=" << lookup_key << ";probes=" << lookup_probes
              << '\n';
  }
  Require(lookup_code == "PROCESS.CANCELLED" &&
              lookup_key ==
                  "sblr.name_resolve.cancelled_before_lookup" &&
              lookup_probes == 1,
          "003616 prelookup cancellation precedence drifted");
  (void)sb_engine_result_release(result);
  result = nullptr;
  authority = {};
  Require(bridge::CopyStatementNameResolveAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes.empty(),
          "003616 prelookup cancellation published a terminal result");
  if (result != nullptr) (void)sb_engine_result_release(result);
  const auto absent_journal =
      api::LookupSblrNameResolveJournalV1(journal_context, journal_key);
  Require(absent_journal.ok && !absent_journal.found,
          "003616 prelookup cancellation published a durable identity");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(2, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle publication_reservation;
  auto publication_dispatch = Admit(fixture, session, view, receipt,
                                    parser_uuid, submission,
                                    &publication_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(
              &publication_dispatch, &result) == SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr,
          "003616 prepublication cancellation did not stop NAME_RESOLVE");
  Require(DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.name_resolve.cancelled_before_publication" &&
              probes.load(std::memory_order_relaxed) == 2,
          "003616 prepublication cancellation precedence drifted");
  (void)sb_engine_result_release(result);
  result = nullptr;
  authority = {};
  Require(bridge::CopyStatementNameResolveAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes.empty(),
          "003616 prepublication cancellation published a terminal result");
  if (result != nullptr) (void)sb_engine_result_release(result);
  const auto begun_journal =
      api::LookupSblrNameResolveJournalV1(journal_context, journal_key);
  Require(begun_journal.ok && begun_journal.found &&
              begun_journal.snapshot.state ==
                  api::SblrNameResolveJournalStateV1::begun &&
              begun_journal.snapshot.canonical_result_bytes.empty(),
          "003616 cancelled lookup did not retain only its durable identity");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle success_reservation;
  auto success_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                submission, &success_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&success_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003616 retry did not publish NAME_RESOLVE result");
  const auto first_result = ResultPayload(result);
  sblr::SblrNameResolveResultV1 decoded_result;
  Require(first_result.size() == sblr::kSblrNameResolveResultBytes &&
              sblr::DecodeSblrNameResolveResultV1(
                  first_result.data(), first_result.size(), &decoded_result,
                  &detail) &&
              decoded_result.status == 2 &&
              decoded_result.visibility == 2 &&
              decoded_result.object_class == 2 &&
              std::all_of(decoded_result.resolved_object_uuid.begin(),
                          decoded_result.resolved_object_uuid.end(),
                          [](std::uint8_t byte) { return byte == 0; }) &&
              probes.load(std::memory_order_relaxed) == 2,
          "003616 successful retry omitted nondisclosing canonical SNRR");
  (void)sb_engine_result_release(result);
  result = nullptr;
  authority = {};
  Require(bridge::CopyStatementNameResolveAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes == first_result,
          "003616 terminal result was not retained for exact replay");
  if (result != nullptr) (void)sb_engine_result_release(result);
  const auto recovered_journal =
      api::LookupSblrNameResolveJournalV1(journal_context, journal_key);
  Require(recovered_journal.ok && recovered_journal.found &&
              recovered_journal.snapshot.state ==
                  api::SblrNameResolveJournalStateV1::published &&
              recovered_journal.snapshot.journal_generation == 2 &&
              recovered_journal.snapshot.canonical_result_bytes ==
                  first_result,
          "003616 durable journal reopen did not recover exact SNRR");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle replay_reservation;
  auto replay_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               submission, &replay_reservation);
  result = nullptr;
  const auto replay_status =
      bridge::DispatchStatementContextReceipt(&replay_dispatch, &result);
  if (replay_status != SB_ENGINE_STATUS_OK || result == nullptr) {
    std::cerr << "003616 replay observation: status="
              << sb_engine_status_name(replay_status)
              << ";probes=" << probes.load(std::memory_order_relaxed);
    if (result != nullptr) {
      std::cerr << ";code=" << DiagnosticCode(result)
                << ";key=" << DiagnosticKey(result);
    }
    std::cerr << '\n';
  }
  Require(replay_status == SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003616 postpublication cancellation retracted exact replay");
  Require(ResultPayload(result) == first_result &&
              probes.load(std::memory_order_relaxed) == 0,
          "003616 replay was not byte-identical and cancellation-stable");
  (void)sb_engine_result_release(result);

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "003616 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "003616 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
