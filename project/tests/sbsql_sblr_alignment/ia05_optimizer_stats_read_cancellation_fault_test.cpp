// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission fixture. No public ABI,
// package gateway, executor registry, transaction inventory, or MGA catalog
// statistics reader is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "engine/sblr/sblr_optimizer_stats_read_runtime.hpp"

#include <atomic>
#include <cstdlib>
#include <string_view>

namespace {

sblr::SblrOperationEnvelope OptimizerStatsReadMember(
    const bridge::StatementContextReceiptView& view,
    const platform::Uuid& parser_uuid,
    const std::vector<std::uint8_t>& descriptor_bytes) {
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.optimizer_stats_read", "SBLR_OPTIMIZER_STATS_READ",
      "ia05.optimizer_stats_read.cancellation_atomicity");
  member.opcode_code = sblr::kSblrOptimizerStatsReadOpcodeCode;
  member.result_shape = "optimizer_stats_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "optimizer_stats_read_descriptor.v1";
  operand.name = "statistics";
  operand.value_kind = sblr::SblrValueKind::optimizer_stats_read_descriptor;
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
  const auto parser_uuid = Identity(NewUuid(platform::UuidKind::object, 36200));
  context.current_package_uuid = parser_uuid;
  context.query_cancellation_requested = [&] {
    const auto ordinal = probes.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto target = cancel_on_probe.load(std::memory_order_relaxed);
    return target != 0 && ordinal == target;
  };

  // Cancellation latches on the receipt (one-way; never cleared), so each
  // phase gets its own receipt: everything the operation binds lives on it.
  struct Attempt {
    bridge::StatementContextReceiptHandle receipt{};
    bridge::StatementContextReceiptView view{};
    sblr::SblrOptimizerStatsReadDescriptorV1 descriptor;
    Submission submission;
  };
  std::string detail;

  const auto prepare = [&](Attempt* out) {
    bridge::StatementContextAcquireRequest acquire;
    acquire.engine_context = &context;
    acquire.exact_transaction_uuid = context.transaction_uuid;
    sb_engine_result_t result = nullptr;
    Require(bridge::AcquireStatementContextReceipt(
                session.session, &acquire, &out->receipt, &out->view, &result) ==
                SB_ENGINE_STATUS_OK,
            "003620 live statement receipt acquisition failed");
    if (result != nullptr) (void)sb_engine_result_release(result);
    Require(out->view.optimizer_stats_read_executor_availability_generation != 0,
            "003620 receipt omitted optimizer-stats availability");

    bridge::StatementOptimizerStatsReadAuthorityV1 authority;
    result = nullptr;
    Require(bridge::BindStatementOptimizerStatsReadAuthorityV1(
                out->receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
                authority.occurrence == 1 &&
                !authority.statistics_snapshot_uuid.is_nil() &&
                !authority.canonical_descriptor_bytes.empty(),
            "003620 engine optimizer-stats descriptor binding failed");
    if (result != nullptr) (void)sb_engine_result_release(result);

    Require(sblr::DecodeSblrOptimizerStatsReadDescriptorV1(
                authority.canonical_descriptor_bytes.data(),
                authority.canonical_descriptor_bytes.size(), &out->descriptor,
                &detail) &&
                out->descriptor.executor_availability_generation ==
                    out->view.optimizer_stats_read_executor_availability_generation,
            "003620 engine OSRD did not decode against the receipt cohort");

    out->submission = PackageWithMember(
        fixture, out->view, parser_uuid,
        OptimizerStatsReadMember(out->view, parser_uuid,
                                 authority.canonical_descriptor_bytes));
  };
  const auto dispatch = [&](Attempt& attempt, sb_engine_result_t* result) {
    bridge::StatementPackageAdmissionReservationHandle reservation;
    auto admitted = Admit(fixture, session, attempt.view, attempt.receipt,
                          parser_uuid, attempt.submission, &reservation);
    *result = nullptr;
    return bridge::DispatchStatementContextReceipt(&admitted, result);
  };
  const auto no_terminal_result = [&](Attempt& attempt, const char* failure) {
    bridge::StatementOptimizerStatsReadAuthorityV1 authority;
    sb_engine_result_t result = nullptr;
    Require(bridge::CopyStatementOptimizerStatsReadAuthorityV1(
                attempt.receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
                !authority.terminal_result_published &&
                authority.canonical_terminal_result_bytes.empty(),
            failure);
    if (result != nullptr) (void)sb_engine_result_release(result);
  };

  // Phase 1: cancellation before the read publishes nothing.
  cancel_on_probe.store(0, std::memory_order_relaxed);
  Attempt read;
  prepare(&read);
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  sb_engine_result_t result = nullptr;
  const auto read_status = dispatch(read, &result);
  const auto read_code =
      result == nullptr ? std::string{} : DiagnosticCode(result);
  const auto read_key =
      result == nullptr ? std::string{} : DiagnosticKey(result);
  const auto read_probes = probes.load(std::memory_order_relaxed);
  if (read_status != SB_ENGINE_STATUS_TIMEOUT || result == nullptr ||
      read_code != "PROCESS.CANCELLED" ||
      read_key != "sblr.optimizer_stats_read.cancelled_before_read" ||
      read_probes != 1) {
    std::cerr << "003620 preread observation: status="
              << sb_engine_status_name(read_status) << ";code=" << read_code
              << ";key=" << read_key << ";probes=" << read_probes << '\n';
  }
  Require(read_status == SB_ENGINE_STATUS_TIMEOUT && result != nullptr &&
              read_code == "PROCESS.CANCELLED" &&
              read_key ==
                  "sblr.optimizer_stats_read.cancelled_before_read" &&
              read_probes == 1,
          "003620 preread cancellation precedence drifted");
  (void)sb_engine_result_release(result);
  no_terminal_result(read, "003620 preread cancellation published a terminal result");

  // The latch: the cancelled receipt stays cancelled with no new request, and
  // still publishes nothing.
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  Require(dispatch(read, &result) != SB_ENGINE_STATUS_OK && result != nullptr &&
              DiagnosticCode(result) == "PROCESS.CANCELLED",
          "003620 cancelled receipt did not stay cancelled");
  (void)sb_engine_result_release(result);
  no_terminal_result(read, "003620 latched receipt published a terminal result");

  // Phase 2: cancellation before publication publishes nothing.
  cancel_on_probe.store(0, std::memory_order_relaxed);
  Attempt publication;
  prepare(&publication);
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(2, std::memory_order_relaxed);
  Require(dispatch(publication, &result) == SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr &&
              DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.optimizer_stats_read.cancelled_before_publication" &&
              probes.load(std::memory_order_relaxed) == 2,
          "003620 prepublication cancellation precedence drifted");
  (void)sb_engine_result_release(result);
  no_terminal_result(publication,
                     "003620 prepublication cancellation published a terminal result");

  // Phase 3: a fresh receipt publishes the canonical result.
  cancel_on_probe.store(0, std::memory_order_relaxed);
  Attempt success;
  prepare(&success);
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  Require(dispatch(success, &result) == SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003620 fresh receipt did not publish optimizer-stats result");
  const auto& descriptor = success.descriptor;
  const auto first_result = ResultPayload(result);
  sblr::SblrOptimizerStatsReadResultV1 decoded_result;
  Require(first_result.size() == sblr::kSblrOptimizerStatsReadResultBytes &&
              sblr::DecodeSblrOptimizerStatsReadResultV1(
                  first_result.data(), first_result.size(), &decoded_result,
                  &detail) &&
              decoded_result.statistics_snapshot_uuid ==
                  descriptor.statistics_snapshot_uuid &&
              decoded_result.statement_receipt_uuid ==
                  descriptor.statement_receipt_uuid &&
              decoded_result.statement_snapshot_uuid ==
                  descriptor.statement_snapshot_uuid &&
              decoded_result.catalog_generation ==
                  descriptor.catalog_generation &&
              decoded_result.security_epoch == descriptor.security_epoch &&
              decoded_result.resource_epoch == descriptor.resource_epoch &&
              decoded_result.inventory_generation ==
                  descriptor.inventory_generation &&
              decoded_result.optimizer_statistics_epoch ==
                  descriptor.optimizer_statistics_epoch &&
              decoded_result.flags ==
                  sblr::kSblrOptimizerStatsReadCatalogFlags &&
              probes.load(std::memory_order_relaxed) == 2,
          "003620 successful dispatch omitted canonical OSRR authority");
  (void)sb_engine_result_release(result);
  {
    bridge::StatementOptimizerStatsReadAuthorityV1 authority;
    result = nullptr;
    Require(bridge::CopyStatementOptimizerStatsReadAuthorityV1(
                success.receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
                authority.terminal_result_published &&
                authority.canonical_terminal_result_bytes == first_result,
            "003620 terminal result was not retained for exact replay");
    if (result != nullptr) (void)sb_engine_result_release(result);
  }

  // Postpublication cancellation does not change the exact replay.
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  Require(dispatch(success, &result) == SB_ENGINE_STATUS_OK &&
              result != nullptr && ResultPayload(result) == first_result &&
              probes.load(std::memory_order_relaxed) == 0,
          "003620 replay was not byte-identical and cancellation-stable");
  (void)sb_engine_result_release(result);
  cancel_on_probe.store(0, std::memory_order_relaxed);

  for (auto* attempt : {&read, &publication, &success})
    Require(bridge::ReleaseStatementContextReceipt(attempt->receipt) ==
                SB_ENGINE_STATUS_OK,
            "003620 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "003620 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
