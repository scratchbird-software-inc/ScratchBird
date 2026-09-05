// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission and source-free query
// fixtures. No package gateway, availability registry, lifecycle state, or
// executor is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "engine/sblr/sblr_stmt_cancel_runtime.hpp"
#include "engine/sblr/sblr_stmt_execute_direct_runtime.hpp"
#include "engine/sblr/sblr_stmt_execute_runtime.hpp"
#include "engine/sblr/sblr_stmt_free_runtime.hpp"
#include "engine/sblr/sblr_stmt_prepare_runtime.hpp"

#include <atomic>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct FaultProfile {
  std::string_view mode;
  std::string_view csc_id;
  std::string_view operation_id;
  std::string_view opcode;
  std::uint16_t opcode_code;
  std::string_view operand_type;
  std::string_view result_shape;
  sblr::SblrValueKind value_kind;
  std::string_view cancellation_key;
};

constexpr FaultProfile kProfiles[] = {
    {"stmt-prepare", "CSC-TEST-003576", "engine.op.stmt_prepare",
     "SBLR_STMT_PREPARE", 4608, "stmt_prepare_descriptor",
     "stmt_prepare_result", sblr::SblrValueKind::stmt_prepare_descriptor,
     "sblr.stmt_prepare.cancelled_before_publication"},
    {"stmt-execute", "CSC-TEST-003580", "engine.op.stmt_execute",
     "SBLR_STMT_EXECUTE", 4609, "stmt_execute_descriptor",
     "stmt_execute_result", sblr::SblrValueKind::stmt_execute_descriptor,
     "sblr.stmt_execute.cancelled_before_dispatch"},
    {"stmt-execute-direct", "CSC-TEST-003584",
     "engine.op.stmt_execute_direct", "SBLR_STMT_EXECUTE_DIRECT", 4610,
     "stmt_execute_direct_descriptor", "stmt_execute_result",
     sblr::SblrValueKind::stmt_execute_direct_descriptor,
     "sblr.stmt_execute_direct.cancelled_before_dispatch"},
    {"stmt-free", "CSC-TEST-003588", "engine.op.stmt_free",
     "SBLR_STMT_FREE", 4611, "stmt_free_descriptor", "stmt_free_result",
     sblr::SblrValueKind::stmt_free_descriptor,
     "sblr.stmt_free.cancelled_before_publication"},
    {"stmt-cancel", "CSC-TEST-003592", "engine.op.stmt_cancel",
     "SBLR_STMT_CANCEL", 4612, "stmt_cancel_descriptor",
     "stmt_cancel_result", sblr::SblrValueKind::stmt_cancel_descriptor,
     "sblr.stmt_cancel.cancelled_before_publication"},
};

[[noreturn]] void FaultFail(const FaultProfile& profile,
                            std::string_view message) {
  std::cerr << profile.csc_id << ": " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void FaultRequire(const FaultProfile& profile, bool condition,
                  std::string_view message) {
  if (!condition) FaultFail(profile, message);
}

struct LiveReceipt {
  bridge::StatementContextReceiptHandle handle;
  bridge::StatementContextReceiptView view;
};

LiveReceipt AcquireReceipt(
    const FaultProfile& profile, PublicSession& session,
    const api::EngineRequestContext& context) {
  bridge::StatementContextAcquireRequest request;
  request.engine_context = &context;
  request.exact_transaction_uuid = context.transaction_uuid.canonical;
  LiveReceipt receipt;
  sb_engine_result_t result = nullptr;
  const auto status = bridge::AcquireStatementContextReceipt(
      session.session, &request, &receipt.handle, &receipt.view, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  FaultRequire(profile, status == SB_ENGINE_STATUS_OK,
               "live statement receipt acquisition failed");
  return receipt;
}

sblr::SblrOperationEnvelope StatementMember(
    const LiveReceipt& receipt, std::string_view parser_uuid,
    const FaultProfile& profile,
    const std::vector<std::uint8_t>& descriptor_bytes) {
  auto member = sblr::MakeSblrEnvelope(
      std::string(profile.operation_id), std::string(profile.opcode),
      "ia05.statement_management.cancellation_atomicity");
  member.opcode_code = profile.opcode_code;
  member.result_shape = std::string(profile.result_shape);
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = std::string(parser_uuid);
  member.registry_snapshot_uuid = receipt.view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = std::string(profile.operand_type);
  operand.name = "statement";
  operand.value_kind = profile.value_kind;
  operand.value_body = descriptor_bytes;
  member.operands.push_back(std::move(operand));
  return member;
}

Submission StatementSubmission(
    const Fixture& fixture, const LiveReceipt& receipt,
    std::string_view parser_uuid, const FaultProfile& profile,
    const std::vector<std::uint8_t>& descriptor_bytes) {
  return PackageWithMember(
      fixture, receipt.view, parser_uuid,
      StatementMember(receipt, parser_uuid, profile, descriptor_bytes));
}

std::vector<std::uint8_t> DispatchSuccess(
    const FaultProfile& profile, const Fixture& fixture,
    PublicSession& session, const LiveReceipt& receipt,
    std::string_view parser_uuid, const Submission& submission) {
  bridge::StatementPackageAdmissionReservationHandle reservation;
  auto dispatch = Admit(fixture, session, receipt.view, receipt.handle,
                        parser_uuid, submission, &reservation);
  sb_engine_result_t result = nullptr;
  const auto status = bridge::DispatchStatementContextReceipt(&dispatch,
                                                               &result);
  if (status != SB_ENGINE_STATUS_OK && result != nullptr) {
    std::cerr << profile.csc_id << ": success dispatch diagnostic="
              << DiagnosticCode(result) << ':' << DiagnosticKey(result)
              << '\n';
  }
  FaultRequire(profile, status == SB_ENGINE_STATUS_OK && result != nullptr,
               "canonical statement lifecycle dispatch failed");
  const auto payload = ResultPayload(result);
  FaultRequire(profile, !payload.empty(),
               "canonical statement lifecycle result was empty");
  (void)sb_engine_result_release(result);
  return payload;
}

void DispatchCancelled(
    const FaultProfile& profile, const Fixture& fixture,
    PublicSession& session, const LiveReceipt& receipt,
    std::string_view parser_uuid, const Submission& submission,
    std::atomic<unsigned>* probes, std::atomic<unsigned>* cancel_on_probe) {
  probes->store(0, std::memory_order_relaxed);
  cancel_on_probe->store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle reservation;
  auto dispatch = Admit(fixture, session, receipt.view, receipt.handle,
                        parser_uuid, submission, &reservation);
  sb_engine_result_t result = nullptr;
  const auto status = bridge::DispatchStatementContextReceipt(&dispatch,
                                                               &result);
  FaultRequire(profile, status == SB_ENGINE_STATUS_TIMEOUT && result != nullptr,
               "prepublication cancellation did not stop lifecycle dispatch");
  FaultRequire(profile,
               DiagnosticCode(result) == "PROCESS.CANCELLED" &&
                   DiagnosticKey(result) == profile.cancellation_key &&
                   probes->load(std::memory_order_relaxed) == 1,
               "operation-owned cancellation boundary drifted");
  sb_engine_string_view_t payload{};
  FaultRequire(profile,
               sb_engine_result_payload(result, &payload) !=
                       SB_ENGINE_STATUS_OK ||
                   payload.size_bytes == 0,
               "cancelled lifecycle operation published a terminal result");
  (void)sb_engine_result_release(result);
}

void ProveCancellationAndReplay(
    const FaultProfile& profile, const Fixture& fixture,
    PublicSession& session, const LiveReceipt& receipt,
    std::string_view parser_uuid, const Submission& submission,
    std::atomic<unsigned>* probes, std::atomic<unsigned>* cancel_on_probe) {
  DispatchCancelled(profile, fixture, session, receipt, parser_uuid,
                    submission, probes, cancel_on_probe);

  probes->store(0, std::memory_order_relaxed);
  cancel_on_probe->store(0, std::memory_order_relaxed);
  const auto published = DispatchSuccess(profile, fixture, session, receipt,
                                         parser_uuid, submission);

  probes->store(0, std::memory_order_relaxed);
  cancel_on_probe->store(1, std::memory_order_relaxed);
  const auto replay = DispatchSuccess(profile, fixture, session, receipt,
                                      parser_uuid, submission);
  FaultRequire(profile,
               replay == published &&
                   probes->load(std::memory_order_relaxed) == 0,
               "postpublication cancellation changed exact byte replay");
  cancel_on_probe->store(0, std::memory_order_relaxed);
}

Submission SourceQuerySubmission(
    const FaultProfile& profile, const Fixture& fixture,
    const LiveReceipt& receipt, std::string_view parser_uuid) {
  auto literal_binding =
      literal_fixture::FinalizeLiteral(receipt.handle, receipt.view);
  auto submission = PackageWithMember(
      fixture, receipt.view, parser_uuid,
      SourceFreeValuesQueryMember(receipt.view, parser_uuid,
                                  literal_binding));
  FaultRequire(profile, !submission.stream.empty(),
               "source-free query fixture was not canonical");
  return submission;
}

std::vector<std::uint8_t> BindPrepare(
    const FaultProfile& profile, const LiveReceipt& receipt,
    std::string_view statement_name, const Submission& query_submission) {
  bind::PrepareBindRequestV1 public_request;
  public_request.authenticated_receipt_uuid = RawUuid(receipt.view.receipt_uuid);
  public_request.occurrence = 1;
  public_request.statement_name = std::string(statement_name);
  public_request.quoted = false;
  public_request.canonical_container_bytes.assign(
      query_submission.container.begin(), query_submission.container.end());
  public_request.canonical_execution_envelope_bytes.assign(
      query_submission.ingress.begin(), query_submission.ingress.end());
  std::vector<std::uint8_t> exact_request;
  std::string detail;
  FaultRequire(profile,
               bind::EncodePrepareBindRequestV1(public_request,
                                                &exact_request, &detail),
               "PREPARE bind request encoding failed");
  bind::PrepareBindRequestV1 decoded;
  FaultRequire(profile,
               bind::DecodePrepareBindRequestV1(
                   exact_request.data(), exact_request.size(), &decoded,
                   &detail),
               "PREPARE bind request decoding failed");

  bridge::StatementPrepareBindRequestV1 request;
  request.authenticated_receipt_uuid = receipt.view.receipt_uuid;
  request.occurrence = decoded.occurrence;
  request.statement_name = decoded.statement_name;
  request.quoted = decoded.quoted;
  request.declared_parameter_type_demands =
      decoded.declared_parameter_type_demands;
  request.canonical_container_bytes = decoded.canonical_container_bytes;
  request.canonical_execution_envelope_bytes =
      decoded.canonical_execution_envelope_bytes;
  request.request_evidence_sha256 = decoded.request_evidence_sha256;
  request.exact_bind_request_bytes = exact_request;
  bridge::StatementPrepareBindAckV1 ack;
  sb_engine_result_t result = nullptr;
  const auto status = bridge::BindStatementPrepareAuthorityV1(
      receipt.handle, &request, &ack, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  FaultRequire(profile, status == SB_ENGINE_STATUS_OK,
               "engine PREPARE binding failed");
  bridge::StatementPrepareAuthorityV1 authority;
  result = nullptr;
  const auto copied = bridge::CopyStatementPrepareAuthorityV1(
      receipt.handle, request.occurrence, &authority, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  FaultRequire(profile,
               copied == SB_ENGINE_STATUS_OK &&
                   !authority.canonical_descriptor_bytes.empty(),
               "engine PREPARE descriptor was not published");
  return authority.canonical_descriptor_bytes;
}

void PublishPreparedStatement(
    const FaultProfile& profile, const Fixture& fixture,
    PublicSession& session, const LiveReceipt& receipt,
    std::string_view parser_uuid, std::string_view statement_name) {
  const auto query =
      SourceQuerySubmission(profile, fixture, receipt, parser_uuid);
  const auto descriptor =
      BindPrepare(profile, receipt, statement_name, query);
  const FaultProfile& prepare_profile = kProfiles[0];
  const auto submission = StatementSubmission(
      fixture, receipt, parser_uuid, prepare_profile, descriptor);
  (void)DispatchSuccess(profile, fixture, session, receipt, parser_uuid,
                        submission);
}

std::vector<std::uint8_t> BindExecuteDirect(
    const FaultProfile& profile, const LiveReceipt& receipt,
    const Submission& query_submission) {
  bind::ExecuteDirectBindRequestV1 public_request;
  public_request.authenticated_receipt_uuid =
      RawUuid(receipt.view.receipt_uuid);
  public_request.occurrence = 1;
  public_request.canonical_container_bytes.assign(
      query_submission.container.begin(), query_submission.container.end());
  public_request.canonical_execution_envelope_bytes.assign(
      query_submission.ingress.begin(), query_submission.ingress.end());
  std::vector<std::uint8_t> exact_request;
  std::string detail;
  FaultRequire(profile,
               bind::EncodeExecuteDirectBindRequestV1(
                   public_request, &exact_request, &detail),
               "EXECUTE DIRECT bind request encoding failed");
  bind::ExecuteDirectBindRequestV1 decoded;
  FaultRequire(profile,
               bind::DecodeExecuteDirectBindRequestV1(
                   exact_request.data(), exact_request.size(), &decoded,
                   &detail),
               "EXECUTE DIRECT bind request decoding failed");

  bridge::StatementExecuteDirectBindRequestV1 request;
  request.authenticated_receipt_uuid = receipt.view.receipt_uuid;
  request.occurrence = decoded.occurrence;
  request.canonical_container_bytes = decoded.canonical_container_bytes;
  request.canonical_execution_envelope_bytes =
      decoded.canonical_execution_envelope_bytes;
  request.canonical_parameter_bytes = decoded.canonical_parameter_bytes;
  request.request_evidence_sha256 = decoded.request_evidence_sha256;
  request.exact_bind_request_bytes = exact_request;
  bridge::StatementExecuteDirectBindAckV1 ack;
  sb_engine_result_t result = nullptr;
  const auto status = bridge::BindStatementExecuteDirectAuthorityV1(
      receipt.handle, &request, &ack, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  FaultRequire(profile, status == SB_ENGINE_STATUS_OK,
               "engine EXECUTE DIRECT binding failed");
  bridge::StatementExecuteDirectAuthorityV1 authority;
  result = nullptr;
  const auto copied = bridge::CopyStatementExecuteDirectAuthorityV1(
      receipt.handle, request.occurrence, &authority, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  FaultRequire(profile,
               copied == SB_ENGINE_STATUS_OK &&
                   !authority.canonical_descriptor_bytes.empty(),
               "engine EXECUTE DIRECT descriptor was not published");
  return authority.canonical_descriptor_bytes;
}

std::vector<std::uint8_t> BindExecute(
    const FaultProfile& profile, const LiveReceipt& receipt,
    std::string_view statement_name) {
  sblr::SblrStmtExecuteRequestV1 wire_request;
  wire_request.statement_receipt_uuid = RawUuid(receipt.view.receipt_uuid);
  wire_request.occurrence = 1;
  wire_request.catalog_generation = receipt.view.catalog_generation_id;
  wire_request.security_epoch = receipt.view.security_epoch;
  wire_request.resource_epoch = receipt.view.resource_epoch;
  wire_request.statement_name = std::string(statement_name);
  wire_request.quoted = false;
  const auto exact_request =
      sblr::EncodeSblrStmtExecuteRequestV1(wire_request);
  FaultRequire(profile, !exact_request.empty(),
               "EXECUTE request encoding failed");
  bridge::StatementExecuteBindRequestV1 request;
  request.authenticated_receipt_uuid = receipt.view.receipt_uuid;
  request.occurrence = wire_request.occurrence;
  request.statement_name = wire_request.statement_name;
  request.quoted = wire_request.quoted;
  request.canonical_parameter_bytes = wire_request.canonical_parameter_bytes;
  request.exact_request_bytes = exact_request;
  std::vector<std::uint8_t> descriptor;
  sb_engine_result_t result = nullptr;
  const auto status = bridge::BindStatementExecuteAuthorityV1(
      receipt.handle, &request, &descriptor, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  FaultRequire(profile, status == SB_ENGINE_STATUS_OK && !descriptor.empty(),
               "engine EXECUTE binding failed");
  return descriptor;
}

std::vector<std::uint8_t> BindFree(
    const FaultProfile& profile, const LiveReceipt& receipt,
    std::string_view statement_name) {
  bind::FreeBindRequestV1 public_request;
  public_request.authenticated_receipt_uuid = RawUuid(receipt.view.receipt_uuid);
  public_request.occurrence = 1;
  public_request.statement_name = std::string(statement_name);
  public_request.quoted = false;
  std::vector<std::uint8_t> exact_request;
  std::string detail;
  FaultRequire(profile,
               bind::EncodeFreeBindRequestV1(public_request, &exact_request,
                                             &detail),
               "FREE bind request encoding failed");
  bind::FreeBindRequestV1 decoded;
  FaultRequire(profile,
               bind::DecodeFreeBindRequestV1(
                   exact_request.data(), exact_request.size(), &decoded,
                   &detail),
               "FREE bind request decoding failed");
  bridge::StatementFreeBindRequestV1 request;
  request.authenticated_receipt_uuid = receipt.view.receipt_uuid;
  request.occurrence = decoded.occurrence;
  request.statement_name = decoded.statement_name;
  request.quoted = decoded.quoted;
  request.request_evidence_sha256 = decoded.request_evidence_sha256;
  request.exact_bind_request_bytes = exact_request;
  bridge::StatementFreeBindAckV1 ack;
  sb_engine_result_t result = nullptr;
  const auto status = bridge::BindStatementFreeAuthorityV1(
      receipt.handle, &request, &ack, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  FaultRequire(profile, status == SB_ENGINE_STATUS_OK,
               "engine FREE binding failed");
  bridge::StatementFreeAuthorityV1 authority;
  result = nullptr;
  const auto copied = bridge::CopyStatementFreeAuthorityV1(
      receipt.handle, request.occurrence, &authority, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  FaultRequire(profile,
               copied == SB_ENGINE_STATUS_OK &&
                   !authority.canonical_descriptor_bytes.empty(),
               "engine FREE descriptor was not published");
  return authority.canonical_descriptor_bytes;
}

std::vector<std::uint8_t> BindCancel(
    const FaultProfile& profile, const LiveReceipt& receipt,
    std::string_view statement_name) {
  bind::CancelBindRequestV1 public_request;
  public_request.authenticated_receipt_uuid =
      RawUuid(receipt.view.receipt_uuid);
  public_request.occurrence = 1;
  public_request.statement_name = std::string(statement_name);
  public_request.quoted = false;
  public_request.reason = 1;
  public_request.mode = 1;
  public_request.deadline_monotonic_ns =
      std::numeric_limits<std::uint64_t>::max();
  std::vector<std::uint8_t> exact_request;
  std::string detail;
  FaultRequire(profile,
               bind::EncodeCancelBindRequestV1(
                   public_request, &exact_request, &detail),
               "CANCEL bind request encoding failed");
  bind::CancelBindRequestV1 decoded;
  FaultRequire(profile,
               bind::DecodeCancelBindRequestV1(
                   exact_request.data(), exact_request.size(), &decoded,
                   &detail),
               "CANCEL bind request decoding failed");
  bridge::StatementCancelBindRequestV1 request;
  request.authenticated_receipt_uuid = receipt.view.receipt_uuid;
  request.occurrence = decoded.occurrence;
  request.statement_name = decoded.statement_name;
  request.quoted = decoded.quoted;
  request.reason = decoded.reason;
  request.mode = decoded.mode;
  request.deadline_monotonic_ns = decoded.deadline_monotonic_ns;
  request.request_evidence_sha256 = decoded.request_evidence_sha256;
  request.exact_bind_request_bytes = exact_request;
  bridge::StatementCancelBindAckV1 ack;
  sb_engine_result_t result = nullptr;
  const auto status = bridge::BindStatementCancelAuthorityV1(
      receipt.handle, &request, &ack, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  FaultRequire(profile, status == SB_ENGINE_STATUS_OK,
               "engine CANCEL binding failed");
  bridge::StatementCancelAuthorityV1 authority;
  result = nullptr;
  const auto copied = bridge::CopyStatementCancelAuthorityV1(
      receipt.handle, request.occurrence, &authority, &result);
  if (result != nullptr) (void)sb_engine_result_release(result);
  FaultRequire(profile,
               copied == SB_ENGINE_STATUS_OK &&
                   !authority.canonical_descriptor_bytes.empty(),
               "engine CANCEL descriptor was not published");
  return authority.canonical_descriptor_bytes;
}

void ReleaseReceipts(const FaultProfile& profile,
                     const std::vector<LiveReceipt>& receipts) {
  for (auto iterator = receipts.rbegin(); iterator != receipts.rend();
       ++iterator) {
    FaultRequire(profile,
                 bridge::ReleaseStatementContextReceipt(iterator->handle) ==
                     SB_ENGINE_STATUS_OK,
                 "statement receipt cleanup failed");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0]
              << " <stmt-prepare|stmt-execute|stmt-execute-direct|stmt-free|"
                 "stmt-cancel>\n";
    return EXIT_FAILURE;
  }
  const std::string_view mode = argv[1];
  const FaultProfile* profile = nullptr;
  for (const auto& candidate : kProfiles) {
    if (candidate.mode == mode) {
      profile = &candidate;
      break;
    }
  }
  if (profile == nullptr) {
    std::cerr << "unknown statement-management cancellation mode: " << mode
              << '\n';
    return EXIT_FAILURE;
  }

  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  std::atomic<unsigned> cancel_on_probe{0};
  auto context = BeginTransaction(fixture, &probes);
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 35920));
  context.current_package_uuid.canonical = parser_uuid;
  context.query_cancellation_requested = [&] {
    const auto ordinal = probes.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto target = cancel_on_probe.load(std::memory_order_relaxed);
    return target != 0 && ordinal == target;
  };

  std::vector<LiveReceipt> receipts;
  if (mode == "stmt-prepare") {
    receipts.push_back(AcquireReceipt(*profile, session, context));
    const auto query = SourceQuerySubmission(
        *profile, fixture, receipts.back(), parser_uuid);
    const auto descriptor = BindPrepare(
        *profile, receipts.back(), "fault_prepare", query);
    const auto submission = StatementSubmission(
        fixture, receipts.back(), parser_uuid, *profile, descriptor);
    ProveCancellationAndReplay(*profile, fixture, session, receipts.back(),
                               parser_uuid, submission, &probes,
                               &cancel_on_probe);
  } else if (mode == "stmt-execute-direct") {
    receipts.push_back(AcquireReceipt(*profile, session, context));
    const auto query = SourceQuerySubmission(
        *profile, fixture, receipts.back(), parser_uuid);
    const auto descriptor =
        BindExecuteDirect(*profile, receipts.back(), query);
    const auto submission = StatementSubmission(
        fixture, receipts.back(), parser_uuid, *profile, descriptor);
    ProveCancellationAndReplay(*profile, fixture, session, receipts.back(),
                               parser_uuid, submission, &probes,
                               &cancel_on_probe);
  } else {
    const std::string statement_name =
        mode == "stmt-execute" ? "fault_execute"
        : mode == "stmt-free"  ? "fault_free"
                                : "fault_cancel";
    receipts.push_back(AcquireReceipt(*profile, session, context));
    PublishPreparedStatement(*profile, fixture, session, receipts.back(),
                             parser_uuid, statement_name);

    if (mode == "stmt-execute") {
      receipts.push_back(AcquireReceipt(*profile, session, context));
      const auto descriptor =
          BindExecute(*profile, receipts.back(), statement_name);
      const auto submission = StatementSubmission(
          fixture, receipts.back(), parser_uuid, *profile, descriptor);
      ProveCancellationAndReplay(*profile, fixture, session, receipts.back(),
                                 parser_uuid, submission, &probes,
                                 &cancel_on_probe);
    } else if (mode == "stmt-free") {
      receipts.push_back(AcquireReceipt(*profile, session, context));
      const auto descriptor =
          BindFree(*profile, receipts.back(), statement_name);
      const auto submission = StatementSubmission(
          fixture, receipts.back(), parser_uuid, *profile, descriptor);
      ProveCancellationAndReplay(*profile, fixture, session, receipts.back(),
                                 parser_uuid, submission, &probes,
                                 &cancel_on_probe);
    } else {
      receipts.push_back(AcquireReceipt(*profile, session, context));
      const auto execute_descriptor =
          BindExecute(*profile, receipts.back(), statement_name);
      const auto execute_submission = StatementSubmission(
          fixture, receipts.back(), parser_uuid, kProfiles[1],
          execute_descriptor);
      probes.store(0, std::memory_order_relaxed);
      cancel_on_probe.store(0, std::memory_order_relaxed);
      (void)DispatchSuccess(*profile, fixture, session, receipts.back(),
                            parser_uuid, execute_submission);

      receipts.push_back(AcquireReceipt(*profile, session, context));
      const auto cancel_descriptor =
          BindCancel(*profile, receipts.back(), statement_name);
      const auto cancel_submission = StatementSubmission(
          fixture, receipts.back(), parser_uuid, *profile,
          cancel_descriptor);
      ProveCancellationAndReplay(*profile, fixture, session, receipts.back(),
                                 parser_uuid, cancel_submission, &probes,
                                 &cancel_on_probe);
    }
  }

  ReleaseReceipts(*profile, receipts);
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  FaultRequire(*profile, api::EngineRollbackTransaction(rollback).ok,
               "fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
