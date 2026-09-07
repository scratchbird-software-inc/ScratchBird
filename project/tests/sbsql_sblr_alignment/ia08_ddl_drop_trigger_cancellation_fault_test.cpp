// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission and CREATE TRIGGER fixture.
// No binder, opcode-stream gateway, executor, catalog, or MGA service is mocked.
#define SCRATCHBIRD_IA08_CREATE_TRIGGER_CANCELLATION_FIXTURE_ONLY
#include "ia08_ddl_create_trigger_cancellation_fault_test.cpp"

#include "engine/sblr/sblr_ddl_drop_trigger_runtime.hpp"

namespace {

bridge::StatementDdlDropTriggerBindRequestV1 DropTriggerDemand(
    const bridge::StatementContextReceiptView& view) {
  sblr::SblrDdlDropTriggerRequestV1 request;
  request.receipt = RawUuid(view.receipt_uuid);
  request.occurrence = 1;
  request.trigger_occurrence = 1;
  request.command_identity = 1;
  request.dependency_mode =
      sblr::DdlDropTriggerDependencyModeV1::kRestrict;
  request.trigger_name_atoms = {{"app", false}, {"trig_items_ai", false}};
  const auto bytes = sblr::EncodeSblrDdlDropTriggerRequestV1(request);
  sblr::SblrDdlDropTriggerRequestV1 decoded;
  std::string detail;
  Require(bytes.size() == sblr::kSblrDdlDropTriggerRequestV1Bytes &&
              sblr::DecodeSblrDdlDropTriggerRequestV1(
                  bytes.data(), bytes.size(), &decoded, &detail),
          "002632 canonical TDQX construction failed");

  bridge::StatementDdlDropTriggerBindRequestV1 demand;
  demand.authenticated_receipt_uuid = view.receipt_uuid;
  demand.occurrence = decoded.occurrence;
  demand.trigger_occurrence = decoded.trigger_occurrence;
  demand.command_identity = decoded.command_identity;
  demand.dependency_mode =
      static_cast<std::uint8_t>(decoded.dependency_mode);
  for (const auto& atom : decoded.trigger_name_atoms) {
    demand.trigger_name_atoms.push_back({atom.raw_utf8, atom.quoted});
  }
  demand.request_evidence_sha256 = decoded.evidence;
  demand.exact_bind_request_bytes = bytes;
  return demand;
}

sblr::SblrOperationEnvelope DropTriggerMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const bridge::StatementDdlDropTriggerAuthorityV1& authority) {
  auto operand_bytes = authority.canonical_descriptor_bytes;
  Require(operand_bytes.size() ==
              sblr::kSblrDdlDropTriggerDescriptorV1Bytes &&
              std::equal(operand_bytes.begin(), operand_bytes.begin() + 4,
                         "TDDX"),
          "002632 receipt did not retain exact TDDX");
  std::copy_n("TDDO", 4, operand_bytes.begin());
  sblr::SblrDdlDropTriggerDescriptorV1 operand_descriptor;
  std::string detail;
  Require(sblr::DecodeSblrDdlDropTriggerDescriptorV1(
              operand_bytes.data(), operand_bytes.size(),
              &operand_descriptor, &detail, true),
          "002632 TDDX to TDDO projection was not canonical");

  auto member = sblr::MakeSblrEnvelope(
      "engine.op.ddl_drop_trigger", "SBLR_DDL_DROP_TRIGGER",
      "ia08.ddl_drop_trigger.cancellation_atomicity");
  member.opcode_code = 1553;
  member.result_shape = "ddl_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "drop_trigger_descriptor";
  operand.name = "trigger";
  operand.value_kind = sblr::SblrValueKind::drop_trigger_descriptor;
  operand.value_body = std::move(operand_bytes);
  member.operands.push_back(std::move(operand));
  return member;
}

void RequireDropCancellation(
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
    std::cerr << "002632 cancellation observation: status="
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
          "002632 DROP TRIGGER cancellation did not stop dispatch");
  Require(DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) == expected_key &&
              probes->load(std::memory_order_relaxed) == expected_probe,
          "002632 DROP TRIGGER cancellation boundary drifted");
  sb_engine_command_completion_view_v1_t completion{};
  Require(sb_engine_result_completion(result, &completion) !=
                  SB_ENGINE_STATUS_OK ||
              (completion.operation_id.size_bytes == 0 &&
               completion.affected_rows == 0),
          "002632 cancelled DROP TRIGGER published completion");
  (void)sb_engine_result_release(result);
  Require(SnapshotFixture(fixture) == before,
          "002632 cancelled DROP TRIGGER changed durable state");
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
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 26323));
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
          "002632 DROP TRIGGER receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(view.ddl_drop_trigger_executor_availability_generation != 0,
          "002632 receipt omitted DROP TRIGGER availability");

  const auto demand = DropTriggerDemand(view);
  bridge::StatementDdlDropTriggerAuthorityV1 authority;
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  result = nullptr;
  Require(bridge::BindStatementDdlDropTriggerAuthorityV1(
              receipt, &demand, &authority, &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr && DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.ddl_drop_trigger.cancelled_before_resolution",
          "002632 bind cancellation did not stop before authority");
  (void)sb_engine_result_release(result);
  result = nullptr;
  Require(bridge::CopyStatementDdlDropTriggerAuthorityV1(
              receipt, 1, 1, &authority, &result) ==
              SB_ENGINE_STATUS_SECURITY_DENIED,
          "002632 cancelled bind published receipt-private authority");
  if (result != nullptr) (void)sb_engine_result_release(result);

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  result = nullptr;
  Require(bridge::BindStatementDdlDropTriggerAuthorityV1(
              receipt, &demand, &authority, &result) == SB_ENGINE_STATUS_OK &&
              result == nullptr &&
              !authority.canonical_descriptor_bytes.empty(),
          "002632 retry did not publish exact DROP TRIGGER authority");
  const auto submission = PackageWithMember(
      fixture, view, parser_uuid,
      DropTriggerMember(view, parser_uuid, authority));
  RequireDropCancellation(
      fixture, session, view, receipt, parser_uuid, submission, &probes,
      &cancel_on_probe, 1,
      "sblr.ddl_drop_trigger.cancelled_before_catalog_mutation");
  RequireDropCancellation(
      fixture, session, view, receipt, parser_uuid, submission, &probes,
      &cancel_on_probe, 2,
      "sblr.ddl_drop_trigger.cancelled_before_publication");
  result = nullptr;
  Require(bridge::CopyStatementDdlDropTriggerAuthorityV1(
              receipt, 1, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes.empty(),
          "002632 cancellation changed DROP terminal authority");
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
          "002632 cancellation retry did not drop the trigger");
  const auto first_result = ResultPayload(result);
  sblr::SblrDdlDropTriggerResultV1 terminal;
  std::string detail;
  Require(first_result.size() == sblr::kSblrDdlDropTriggerResultV1Bytes &&
              sblr::DecodeSblrDdlDropTriggerResultV1(
                  first_result.data(), first_result.size(), &terminal,
                  &detail) &&
              terminal.receipt == RawUuid(view.receipt_uuid) &&
              terminal.trigger_uuid == RawUuid(authority.trigger_uuid) &&
              terminal.trigger_generation == authority.trigger_generation + 1,
          "002632 successful retry omitted exact TDRS");
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
          "002632 postpublication cancellation retracted exact TDRS replay");
  (void)sb_engine_result_release(result);

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "002632 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "002632 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
