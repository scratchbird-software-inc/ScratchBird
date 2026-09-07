// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission and CREATE TRIGGER fixture.
// No binder, opcode-stream gateway, executor, catalog, or MGA service is mocked.
#define SCRATCHBIRD_IA08_CREATE_TRIGGER_CANCELLATION_FIXTURE_ONLY
#include "ia08_ddl_create_trigger_cancellation_fault_test.cpp"

#include "engine/sblr/sblr_ddl_alter_trigger_runtime.hpp"

namespace {

bridge::StatementDdlAlterTriggerBindRequestV1 AlterTriggerDemand(
    const bridge::StatementContextReceiptView& view) {
  sblr::SblrDdlAlterTriggerRequestV1 request;
  request.receipt = RawUuid(view.receipt_uuid);
  request.occurrence = 1;
  request.trigger_occurrence = 1;
  request.command_identity = 1;
  request.action_mask =
      sblr::kDdlAlterTriggerActionEnabled |
      sblr::kDdlAlterTriggerActionPosition |
      sblr::kDdlAlterTriggerActionSecurity |
      sblr::kDdlAlterTriggerActionCompile |
      sblr::kDdlAlterTriggerActionValidate;
  request.enabled_state = sblr::DdlAlterTriggerEnabledStateV1::kDisabled;
  request.position = 7;
  request.security_mode = sblr::DdlAlterTriggerSecurityModeV1::kDefiner;
  request.failure_policy =
      sblr::DdlAlterTriggerFailurePolicyV1::kPreserve;
  request.trigger_name_atoms = {{"app", false}, {"trig_items_ai", false}};
  const auto bytes = sblr::EncodeSblrDdlAlterTriggerRequestV1(request);
  sblr::SblrDdlAlterTriggerRequestV1 decoded;
  std::string detail;
  Require(bytes.size() == sblr::kSblrDdlAlterTriggerRequestV1Bytes &&
              sblr::DecodeSblrDdlAlterTriggerRequestV1(
                  bytes.data(), bytes.size(), &decoded, &detail),
          "002628 canonical TAQX construction failed");

  bridge::StatementDdlAlterTriggerBindRequestV1 demand;
  demand.authenticated_receipt_uuid = view.receipt_uuid;
  demand.occurrence = decoded.occurrence;
  demand.trigger_occurrence = decoded.trigger_occurrence;
  demand.command_identity = decoded.command_identity;
  demand.action_mask = decoded.action_mask;
  demand.enabled_state = static_cast<std::uint8_t>(decoded.enabled_state);
  demand.position = decoded.position;
  demand.security_mode = static_cast<std::uint8_t>(decoded.security_mode);
  demand.failure_policy = static_cast<std::uint8_t>(decoded.failure_policy);
  for (const auto& atom : decoded.trigger_name_atoms) {
    demand.trigger_name_atoms.push_back({atom.raw_utf8, atom.quoted});
  }
  demand.request_evidence_sha256 = decoded.evidence;
  demand.exact_bind_request_bytes = bytes;
  return demand;
}

sblr::SblrOperationEnvelope AlterTriggerMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const bridge::StatementDdlAlterTriggerAuthorityV1& authority) {
  auto operand_bytes = authority.canonical_descriptor_bytes;
  Require(operand_bytes.size() ==
              sblr::kSblrDdlAlterTriggerDescriptorV1Bytes &&
              std::equal(operand_bytes.begin(), operand_bytes.begin() + 4,
                         "TADX"),
          "002628 receipt did not retain exact TADX");
  std::copy_n("TADO", 4, operand_bytes.begin());
  sblr::SblrDdlAlterTriggerDescriptorV1 operand_descriptor;
  std::string detail;
  Require(sblr::DecodeSblrDdlAlterTriggerDescriptorV1(
              operand_bytes.data(), operand_bytes.size(),
              &operand_descriptor, &detail, true),
          "002628 TADX to TADO projection was not canonical");

  auto member = sblr::MakeSblrEnvelope(
      "engine.op.ddl_alter_trigger", "SBLR_DDL_ALTER_TRIGGER",
      "ia08.ddl_alter_trigger.cancellation_atomicity");
  member.opcode_code = 1552;
  member.result_shape = "ddl_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "alter_trigger_descriptor";
  operand.name = "trigger";
  operand.value_kind = sblr::SblrValueKind::alter_trigger_descriptor;
  operand.value_body = std::move(operand_bytes);
  member.operands.push_back(std::move(operand));
  return member;
}

void RequireAlterCancellation(
    const Fixture& fixture, PublicSession& session,
    const bridge::StatementContextReceiptView& view,
    bridge::StatementContextReceiptHandle receipt,
    std::string_view parser_uuid, const Submission& submission,
    std::atomic<unsigned>* probes, std::atomic<unsigned>* cancel_on_probe,
    unsigned expected_probe, std::string_view expected_key) {
  probes->store(0, std::memory_order_relaxed);
  cancel_on_probe->store(expected_probe, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle reservation;
  auto dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                        submission, &reservation);
  const auto before = SnapshotFixture(fixture);
  sb_engine_result_t result = nullptr;
  const auto status =
      bridge::DispatchStatementContextReceipt(&dispatch, &result);
  if (status != SB_ENGINE_STATUS_TIMEOUT || result == nullptr ||
      DiagnosticCode(result) != "PROCESS.CANCELLED" ||
      DiagnosticKey(result) != expected_key ||
      probes->load(std::memory_order_relaxed) != expected_probe) {
    std::cerr << "002628 cancellation observation: status="
              << sb_engine_status_name(status)
              << ";probes=" << probes->load(std::memory_order_relaxed);
    if (result != nullptr) {
      std::cerr << ";code=" << DiagnosticCode(result)
                << ";key=" << DiagnosticKey(result)
                << ";detail=" << DiagnosticDetail(result);
    }
    std::cerr << '\n';
  }
  Require(status == SB_ENGINE_STATUS_TIMEOUT && result != nullptr,
          "002628 ALTER TRIGGER cancellation did not stop dispatch");
  Require(DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) == expected_key &&
              probes->load(std::memory_order_relaxed) == expected_probe,
          "002628 ALTER TRIGGER cancellation boundary drifted");
  sb_engine_command_completion_view_v1_t completion{};
  Require(sb_engine_result_completion(result, &completion) !=
                  SB_ENGINE_STATUS_OK ||
              (completion.operation_id.size_bytes == 0 &&
               completion.affected_rows == 0),
          "002628 cancelled ALTER TRIGGER published completion");
  (void)sb_engine_result_release(result);
  Require(SnapshotFixture(fixture) == before,
          "002628 cancelled ALTER TRIGGER changed durable state");
}

}  // namespace

int main() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  std::atomic<unsigned> cancel_on_probe{0};
  auto context = BeginTransaction(fixture, &probes);
  context.query_cancellation_requested = [&] {
    const auto ordinal = probes.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto target = cancel_on_probe.load(std::memory_order_relaxed);
    return target != 0 && ordinal == target;
  };
  (void)PrepareTriggerTarget(fixture, &context);
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 26283));
  context.current_package_uuid.canonical = parser_uuid;
  PublishBaselineTrigger(fixture, session, &context, parser_uuid);

  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t result = nullptr;
  Require(bridge::AcquireStatementContextReceipt(
              session.session, &acquire, &receipt, &view, &result) ==
              SB_ENGINE_STATUS_OK,
          "002628 ALTER TRIGGER receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(view.ddl_alter_trigger_executor_availability_generation != 0,
          "002628 receipt omitted ALTER TRIGGER availability");

  const auto demand = AlterTriggerDemand(view);
  bridge::StatementDdlAlterTriggerAuthorityV1 authority;
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  result = nullptr;
  Require(bridge::BindStatementDdlAlterTriggerAuthorityV1(
              receipt, &demand, &authority, &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr && DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.ddl_alter_trigger.cancelled_before_resolution",
          "002628 bind cancellation did not stop before authority");
  (void)sb_engine_result_release(result);
  result = nullptr;
  Require(bridge::CopyStatementDdlAlterTriggerAuthorityV1(
              receipt, 1, 1, &authority, &result) ==
              SB_ENGINE_STATUS_SECURITY_DENIED,
          "002628 cancelled bind published receipt-private authority");
  if (result != nullptr) (void)sb_engine_result_release(result);

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  result = nullptr;
  Require(bridge::BindStatementDdlAlterTriggerAuthorityV1(
              receipt, &demand, &authority, &result) == SB_ENGINE_STATUS_OK &&
              result == nullptr &&
              !authority.canonical_descriptor_bytes.empty(),
          "002628 retry did not publish exact ALTER TRIGGER authority");
  const auto submission = PackageWithMember(
      fixture, view, parser_uuid,
      AlterTriggerMember(view, parser_uuid, authority));
  RequireAlterCancellation(
      fixture, session, view, receipt, parser_uuid, submission, &probes,
      &cancel_on_probe, 1,
      "sblr.ddl_alter_trigger.cancelled_before_catalog_mutation");
  RequireAlterCancellation(
      fixture, session, view, receipt, parser_uuid, submission, &probes,
      &cancel_on_probe, 2,
      "sblr.ddl_alter_trigger.cancelled_before_publication");
  result = nullptr;
  Require(bridge::CopyStatementDdlAlterTriggerAuthorityV1(
              receipt, 1, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes.empty(),
          "002628 cancellation changed ALTER terminal authority");
  if (result != nullptr) (void)sb_engine_result_release(result);

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle success_reservation;
  auto success_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                submission, &success_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&success_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "002628 cancellation retry did not alter the trigger");
  const auto first_result = ResultPayload(result);
  sblr::SblrDdlAlterTriggerResultV1 terminal;
  std::string detail;
  Require(first_result.size() == sblr::kSblrDdlAlterTriggerResultV1Bytes &&
              sblr::DecodeSblrDdlAlterTriggerResultV1(
                  first_result.data(), first_result.size(), &terminal,
                  &detail) &&
              terminal.receipt == RawUuid(view.receipt_uuid) &&
              terminal.trigger_uuid == RawUuid(authority.trigger_uuid) &&
              terminal.trigger_generation == authority.trigger_generation + 1,
          "002628 successful retry omitted exact TARS");
  (void)sb_engine_result_release(result);

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
          "002628 postpublication cancellation retracted exact TARS replay");
  (void)sb_engine_result_release(result);

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "002628 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "002628 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
