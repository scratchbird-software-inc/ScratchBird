// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission fixture. No public ABI,
// package gateway, executor registry, transaction inventory, authorization,
// durable epoch journal, or cache invalidator is mocked.
#define SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
#include "ia05_query_explain_cancellation_fault_test.cpp"

#include "engine/internal_api/sblr_optimizer_stats_drop_journal.hpp"
#include "engine/sblr/sblr_optimizer_stats_drop_runtime.hpp"
#include "storage/database/local_transaction_store.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace {

sblr::SblrOperationEnvelope OptimizerStatsDropMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const std::vector<std::uint8_t>& descriptor_bytes) {
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.optimizer_stats_drop", "SBLR_OPTIMIZER_STATS_DROP",
      "ia05.optimizer_stats_drop.cancellation_atomicity");
  member.opcode_code = sblr::kSblrOptimizerStatsDropOpcodeCode;
  member.result_shape = "optimizer_stats_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "optimizer_stats_drop_descriptor.v1";
  operand.name = "statistics";
  operand.value_kind = sblr::SblrValueKind::optimizer_stats_drop_descriptor;
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
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 36240));
  context.current_package_uuid.canonical = parser_uuid;
  context.trace_tags.push_back("right:OBS_MANAGEMENT_CONTROL");
  context.authorization_context.security_context_generation = 1;
  api::EngineMaterializedAuthorizationGrant management_grant;
  management_grant.grant_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object, 36242));
  management_grant.subject_uuid = context.principal_uuid;
  management_grant.subject_kind = "principal";
  management_grant.target_uuid = context.database_uuid;
  management_grant.right = "OBS_MANAGEMENT_CONTROL";
  management_grant.security_epoch = context.security_epoch;
  context.authorization_context.grants.push_back(management_grant);
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
          "003624 live statement receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);
  Require(view.optimizer_stats_drop_executor_availability_generation != 0,
          "003624 receipt omitted optimizer-stats DROP availability");

  bridge::StatementOptimizerStatsDropAuthorityV1 authority;
  result = nullptr;
  const auto bind_status = bridge::BindStatementOptimizerStatsDropAuthorityV1(
      receipt, 1, &authority, &result);
  if (bind_status != SB_ENGINE_STATUS_OK && result != nullptr) {
    std::cerr << "003624 bind observation: status="
              << sb_engine_status_name(bind_status)
              << ";code=" << DiagnosticCode(result)
              << ";key=" << DiagnosticKey(result) << '\n';
  }
  Require(bind_status == SB_ENGINE_STATUS_OK &&
              authority.occurrence == 1 && !authority.effect_uuid.empty() &&
              !authority.canonical_descriptor_bytes.empty(),
          "003624 engine optimizer-stats DROP binding failed");
  if (result != nullptr) (void)sb_engine_result_release(result);

  sblr::SblrOptimizerStatsDropDescriptorV1 descriptor;
  std::string detail;
  Require(sblr::DecodeSblrOptimizerStatsDropDescriptorV1(
              authority.canonical_descriptor_bytes.data(),
              authority.canonical_descriptor_bytes.size(), &descriptor,
              &detail) &&
              descriptor.executor_availability_generation ==
                  view.optimizer_stats_drop_executor_availability_generation &&
              descriptor.expected_statistics_epoch == 1 &&
              descriptor.expected_journal_generation == 0 &&
              descriptor.next_statistics_epoch == 2 &&
              descriptor.proposed_effect_generation == 1,
          "003624 engine OSDD did not bind the initial durable epoch");

  api::EngineRequestContext receipt_context;
  Require(bridge::CopyStatementContextEngineContextV1(
              receipt, &receipt_context, nullptr) == SB_ENGINE_STATUS_OK,
          "003624 private statement context copy failed");
  const auto initial_epoch =
      api::InspectSblrOptimizerStatsEpochV1(receipt_context);
  Require(initial_epoch.ok && initial_epoch.statistics_epoch == 1 &&
              initial_epoch.journal_generation == 0,
          "003624 fresh database statistics epoch was not canonical");

  const auto submission = PackageWithMember(
      fixture, view, parser_uuid,
      OptimizerStatsDropMember(view, parser_uuid,
                               authority.canonical_descriptor_bytes));
  const auto journal_path =
      fixture.database_path.string() + ".sb.sblr_optimizer_stats_epoch.v1";

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle revalidate_reservation;
  auto revalidate_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                   submission, &revalidate_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&revalidate_dispatch,
                                                  &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr && DiagnosticCode(result) ==
                                       "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.optimizer_stats_drop.cancelled_before_revalidation" &&
              probes.load(std::memory_order_relaxed) == 1 &&
              !std::filesystem::exists(journal_path),
          "003624 pre-revalidation cancellation changed durable state");
  (void)sb_engine_result_release(result);

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(2, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle publication_reservation;
  auto publication_dispatch = Admit(fixture, session, view, receipt,
                                    parser_uuid, submission,
                                    &publication_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&publication_dispatch,
                                                  &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr && DiagnosticCode(result) ==
                                       "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.optimizer_stats_drop.cancelled_before_publication" &&
              probes.load(std::memory_order_relaxed) == 2 &&
              !std::filesystem::exists(journal_path),
          "003624 pre-publication cancellation changed durable state");
  (void)sb_engine_result_release(result);
  result = nullptr;
  authority = {};
  Require(bridge::CopyStatementOptimizerStatsDropAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.terminal_result_published &&
              authority.canonical_terminal_result_bytes.empty(),
          "003624 cancellation published a terminal result");
  if (result != nullptr) (void)sb_engine_result_release(result);

  // Advance the database-wide transaction inventory after this statement's
  // strong snapshot was issued.  That global fence change must force one
  // exact slow-path revalidation, but it must not invalidate the still-active
  // owning transaction or mint a replacement DROP descriptor.
  std::atomic<unsigned> unrelated_probes{0};
  auto unrelated_context = BeginTransaction(fixture, &unrelated_probes);
  unrelated_context.query_cancellation_requested = [] { return false; };
  api::EngineCommitTransactionRequest unrelated_commit;
  unrelated_commit.context = unrelated_context;
  Require(api::EngineCommitTransaction(unrelated_commit).ok,
          "003624 unrelated transaction publication failed");
  const auto changed_inventory_fence =
      scratchbird::storage::database::
          RevalidateLocalTransactionInventorySnapshot(
              *receipt_context.statement_transaction_inventory_snapshot);
  Require(!changed_inventory_fence.ok(),
          "003624 unrelated publication did not advance the global fence");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle success_reservation;
  auto success_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                submission, &success_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&success_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003624 retry did not publish optimizer-stats DROP");
  const auto first_result = ResultPayload(result);
  sblr::SblrOptimizerStatsDropResultV1 decoded_result;
  Require(first_result.size() == sblr::kSblrOptimizerStatsDropResultBytes &&
              sblr::DecodeSblrOptimizerStatsDropResultV1(
                  first_result.data(), first_result.size(), &decoded_result,
                  &detail) &&
              decoded_result.effect_uuid == descriptor.effect_uuid &&
              decoded_result.statement_receipt_uuid ==
                  descriptor.statement_receipt_uuid &&
              decoded_result.prior_statistics_epoch == 1 &&
              decoded_result.statistics_epoch == 2 &&
              decoded_result.effect_generation == 1 &&
              decoded_result.cache_invalidation_generation == 2 &&
              decoded_result.publication_barrier_generation == 1 &&
              probes.load(std::memory_order_relaxed) == 2 &&
              std::filesystem::file_size(journal_path) == 472,
          "003624 successful retry omitted durable OSDR authority");
  (void)sb_engine_result_release(result);

  // Exercise restart-style recovery below the receipt cache: the journal is
  // opened and decoded again and must return the same result without append.
  const auto journal_replay =
      api::PublishSblrOptimizerStatsDropV1(receipt_context, descriptor);
  Require(journal_replay.ok && journal_replay.exact_replay &&
              journal_replay.canonical_result_bytes == first_result &&
              std::filesystem::file_size(journal_path) == 472,
          "003624 durable journal did not replay byte-identically");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle replay_reservation;
  auto replay_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               submission, &replay_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&replay_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr && ResultPayload(result) == first_result &&
              probes.load(std::memory_order_relaxed) == 0 &&
              std::filesystem::file_size(journal_path) == 472,
          "003624 terminal replay was not cancellation-stable and idempotent");
  (void)sb_engine_result_release(result);

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "003624 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "003624 fixture transaction rollback failed");

  // A fresh statement sees the physical history generation but not a rolled-
  // back transaction's epoch. This is the MGA visibility boundary used by
  // subsequent READ and DROP descriptors.
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  auto next_context = BeginTransaction(fixture, &probes);
  const auto next_parser_uuid =
      Text(NewUuid(platform::UuidKind::object, 36241));
  next_context.current_package_uuid.canonical = next_parser_uuid;
  next_context.trace_tags.push_back("right:OBS_MANAGEMENT_CONTROL");
  next_context.authorization_context.security_context_generation = 1;
  api::EngineMaterializedAuthorizationGrant next_management_grant;
  next_management_grant.grant_uuid.canonical =
      Text(NewUuid(platform::UuidKind::object, 36243));
  next_management_grant.subject_uuid = next_context.principal_uuid;
  next_management_grant.subject_kind = "principal";
  next_management_grant.target_uuid = next_context.database_uuid;
  next_management_grant.right = "OBS_MANAGEMENT_CONTROL";
  next_management_grant.security_epoch = next_context.security_epoch;
  next_context.authorization_context.grants.push_back(
      std::move(next_management_grant));
  next_context.query_cancellation_requested = [] { return false; };
  bridge::StatementContextAcquireRequest next_acquire;
  next_acquire.engine_context = &next_context;
  next_acquire.exact_transaction_uuid =
      next_context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle next_receipt;
  bridge::StatementContextReceiptView next_view;
  result = nullptr;
  Require(bridge::AcquireStatementContextReceipt(
              session.session, &next_acquire, &next_receipt, &next_view,
              &result) == SB_ENGINE_STATUS_OK,
          "003624 fresh statement after rollback could not attach");
  if (result != nullptr) (void)sb_engine_result_release(result);
  api::EngineRequestContext next_receipt_context;
  Require(bridge::CopyStatementContextEngineContextV1(
              next_receipt, &next_receipt_context, nullptr) ==
              SB_ENGINE_STATUS_OK,
          "003624 fresh private context copy failed");
  const auto rolled_back_epoch =
      api::InspectSblrOptimizerStatsEpochV1(next_receipt_context);
  Require(rolled_back_epoch.ok &&
              rolled_back_epoch.statistics_epoch == 1 &&
              rolled_back_epoch.journal_generation == 1 &&
              rolled_back_epoch.unresolved_other_transaction_count == 0,
          "003624 rolled-back DROP remained visible to a new statement");
  bridge::StatementOptimizerStatsDropAuthorityV1 next_authority;
  result = nullptr;
  Require(bridge::BindStatementOptimizerStatsDropAuthorityV1(
              next_receipt, 1, &next_authority, &result) ==
              SB_ENGINE_STATUS_OK,
          "003624 replacement DROP descriptor could not be issued");
  if (result != nullptr) (void)sb_engine_result_release(result);
  sblr::SblrOptimizerStatsDropDescriptorV1 next_descriptor;
  Require(sblr::DecodeSblrOptimizerStatsDropDescriptorV1(
              next_authority.canonical_descriptor_bytes.data(),
              next_authority.canonical_descriptor_bytes.size(),
              &next_descriptor, &detail) &&
              next_descriptor.expected_statistics_epoch == 1 &&
              next_descriptor.expected_journal_generation == 1 &&
              next_descriptor.proposed_effect_generation == 2 &&
              next_descriptor.next_statistics_epoch == 2,
          "003624 next descriptor did not retain rollback-safe epoch lineage");
  Require(bridge::ReleaseStatementContextReceipt(next_receipt) ==
              SB_ENGINE_STATUS_OK,
          "003624 replacement receipt cleanup failed");
  api::EngineRollbackTransactionRequest next_rollback;
  next_rollback.context = next_context;
  Require(api::EngineRollbackTransaction(next_rollback).ok,
          "003624 replacement transaction rollback failed");
  return EXIT_SUCCESS;
}
