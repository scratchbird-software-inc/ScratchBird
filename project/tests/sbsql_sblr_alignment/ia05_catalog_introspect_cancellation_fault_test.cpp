// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission fixture. No public ABI,
// package gateway, executor registry, or statement receipt is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "engine/internal_api/sblr_catalog_introspect_coordinator.hpp"
#include "engine/sblr/sblr_catalog_introspect_runtime.hpp"

#include <atomic>
#include <cstdlib>
#include <string_view>

namespace {

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
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 36120));
  context.current_package_uuid.canonical = parser_uuid;
  context.query_cancellation_requested = [&] {
    probes.fetch_add(1, std::memory_order_relaxed);
    return cancel.load(std::memory_order_relaxed);
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
          "003612 live statement receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(view.catalog_introspect_executor_availability_generation != 0,
          "003612 receipt omitted catalog-introspect availability");

  auto coordinator_context = context;
  coordinator_context.statement_uuid.canonical = view.receipt_uuid;
  coordinator_context.statement_metadata_snapshot_engine_owned = true;
  coordinator_context.trace_tags.push_back(
      "private_catalog_introspect_binder");
  auto coordinated =
      scratchbird::engine::internal_api::CompileSblrCatalogIntrospectDescriptor(
          coordinator_context, view.receipt_uuid, 1, 1,
          view.catalog_introspect_executor_availability_generation);
  Require(coordinated.ok,
          "003612 engine catalog-introspect descriptor was not published");
  const auto descriptor_bytes =
      sblr::EncodeSblrCatalogIntrospectDescriptorV1(coordinated.descriptor,
                                                    true);
  Require(descriptor_bytes.size() == 488,
          "003612 canonical CIDO encoding failed");
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
  Require(DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) == "sblr.catalog_introspect.cancelled" &&
              probes.load(std::memory_order_relaxed) == 1,
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
  std::string detail;
  sblr::SblrCatalogIntrospectResultV1 decoded_result;
  Require(result_bytes.size() == 320 &&
              sblr::DecodeSblrCatalogIntrospectResultV1(
                  result_bytes.data(), result_bytes.size(), &decoded_result,
                  &detail) &&
              decoded_result.availability ==
                  view.catalog_introspect_executor_availability_generation,
          "003612 successful retry omitted canonical CIRS");
  (void)sb_engine_result_release(result);

  probes.store(0, std::memory_order_relaxed);
  cancel.store(true, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle stale_reservation;
  auto stale_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                              submission, &stale_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&stale_dispatch, &result) ==
                  SB_ENGINE_STATUS_CONFLICT &&
              result != nullptr &&
              DiagnosticCode(result) == "MGA.TRANSACTION.STALE" &&
              probes.load(std::memory_order_relaxed) == 0,
          "003612 consumed descriptor replay did not precede cancellation");
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
