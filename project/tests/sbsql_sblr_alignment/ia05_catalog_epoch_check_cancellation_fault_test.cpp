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
    const platform::Uuid& parser_uuid,
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

void RunCleanupOrderingCase(bool cleanup_after_capture) {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  std::atomic<unsigned> cancel_on_probe{0};
  auto cleanup_context = BeginTransaction(fixture, &probes);
  cleanup_context.query_cancellation_requested = {};
  const auto finish_other_transaction = [&] {
    api::EngineRollbackTransactionRequest rollback;
    rollback.context = cleanup_context;
    Require(api::EngineRollbackTransaction(rollback).ok,
            "catalog epoch competing transaction cleanup failed");
  };
  if (!cleanup_after_capture) finish_other_transaction();
  auto context = BeginTransaction(fixture, &probes);
  const auto parser_uuid = Identity(NewUuid(platform::UuidKind::object, 36320));
  context.current_package_uuid = parser_uuid;
  context.query_cancellation_requested = [&] {
    const auto ordinal = probes.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto target = cancel_on_probe.load(std::memory_order_relaxed);
    return target != 0 && ordinal == target;
  };

  auto journal_context = context;
  journal_context.statement_metadata_snapshot_engine_owned = true;
  journal_context.trace_tags.push_back("private_catalog_epoch_check_journal");
  std::string detail;

  // Cancellation latches on the receipt (one-way; never cleared), so each
  // phase gets its own receipt: everything the operation binds lives on it.
  struct Attempt {
    bridge::StatementContextReceiptHandle receipt{};
    bridge::StatementContextReceiptView view{};
    std::vector<std::uint8_t> exact_bind;
    sblr::SblrCatalogEpochCheckDescriptorV1 descriptor;
    api::SblrCatalogEpochCheckJournalKeyV1 journal_key;
    Submission submission;
  };

  const auto prepare = [&](Attempt* out) {
    bridge::StatementContextAcquireRequest acquire;
    acquire.engine_context = &context;
    acquire.exact_transaction_uuid = context.transaction_uuid;
    sb_engine_result_t result = nullptr;
    Require(bridge::AcquireStatementContextReceipt(
                session.session, &acquire, &out->receipt, &out->view,
                &result) == SB_ENGINE_STATUS_OK,
            "003632 live statement receipt acquisition failed");
    if (result != nullptr) (void)sb_engine_result_release(result);
    const auto& view = out->view;
    Require(view.catalog_epoch_check_executor_availability_generation != 0 &&
                !view.catalog_epoch_check_redaction_profile_uuid.is_nil() &&
                view.catalog_epoch_check_redaction_generation != 0 &&
                !view.catalog_epoch_check_policy_snapshot_uuid.is_nil() &&
                view.catalog_epoch_check_policy_generation != 0,
            "003632 receipt omitted catalog-epoch authority");

    bind::CatalogEpochCheckBindRequestV1 public_bind;
    public_bind.authenticated_receipt_uuid = RawUuid(view.receipt_uuid);
    public_bind.occurrence = 1;
    public_bind.object_scoped = false;
    Require(bind::EncodeCatalogEpochCheckBindRequestV1(
                public_bind, &out->exact_bind, &detail),
            "003632 canonical private bind request encoding failed");
    bind::CatalogEpochCheckBindRequestV1 decoded_bind;
    Require(bind::DecodeCatalogEpochCheckBindRequestV1(
                out->exact_bind.data(), out->exact_bind.size(), &decoded_bind,
                &detail),
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
    bind_request.request_evidence_sha256 =
        decoded_bind.request_evidence_sha256;
    bind_request.exact_bind_request_bytes = out->exact_bind;

    bridge::StatementCatalogEpochCheckBindAckV1 bind_ack;
    result = nullptr;
    Require(bridge::BindStatementCatalogEpochCheckAuthorityV1(
                out->receipt, &bind_request, &bind_ack, &result) ==
                SB_ENGINE_STATUS_OK,
            "003632 engine catalog-epoch binding failed");
    if (result != nullptr) (void)sb_engine_result_release(result);

    bridge::StatementCatalogEpochCheckAuthorityV1 authority;
    result = nullptr;
    Require(bridge::CopyStatementCatalogEpochCheckAuthorityV1(
                out->receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
                !authority.canonical_descriptor_bytes.empty() &&
                authority.exact_bind_request_bytes == out->exact_bind &&
                authority.acknowledgement.check_uuid == bind_ack.check_uuid,
            "003632 engine catalog-epoch descriptor was not published");
    if (result != nullptr) (void)sb_engine_result_release(result);

    auto& descriptor = out->descriptor;
    Require(sblr::DecodeSblrCatalogEpochCheckDescriptorV1(
                authority.canonical_descriptor_bytes.data(),
                authority.canonical_descriptor_bytes.size(), &descriptor,
                &detail) &&
                !descriptor.object_scoped &&
                descriptor.statement_receipt_uuid == RawUuid(view.receipt_uuid),
            "003632 engine SECD did not decode");

    auto& journal_key = out->journal_key;
    journal_key.database_uuid = RawUuid(context.database_uuid);
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

    out->submission = PackageWithMember(
        fixture, view, parser_uuid,
        CatalogEpochCheckMember(view, parser_uuid,
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
    bridge::StatementCatalogEpochCheckAuthorityV1 authority;
    sb_engine_result_t result = nullptr;
    Require(bridge::CopyStatementCatalogEpochCheckAuthorityV1(
                attempt.receipt, 1, &authority, &result) ==
                SB_ENGINE_STATUS_OK &&
                !authority.terminal_result_published &&
                authority.canonical_terminal_result_bytes.empty(),
            failure);
    if (result != nullptr) (void)sb_engine_result_release(result);
  };

  // Phase 1: cancellation before observation keeps only the begun identity.
  Attempt first;
  cancel_on_probe.store(0, std::memory_order_relaxed);
  prepare(&first);
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  sb_engine_result_t result = nullptr;
  Require(dispatch(first, &result) == SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr &&
              DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.catalog_epoch_check.cancelled_before_observation" &&
              probes.load(std::memory_order_relaxed) == 1,
          "003632 first cancellation crossed catalog observation");
  (void)sb_engine_result_release(result);
  no_terminal_result(first, "003632 first cancellation published a receipt result");
  const auto begun = api::LookupSblrCatalogEpochCheckJournalV1(
      journal_context, first.journal_key);
  Require(begun.ok && begun.found &&
              begun.snapshot.state ==
                  api::SblrCatalogEpochCheckJournalStateV1::begun &&
              begun.snapshot.journal_generation == 1 &&
              begun.snapshot.canonical_result_bytes.empty(),
          "003632 first cancellation did not retain durable identity");

  // The latch: the cancelled receipt stays cancelled with no new request, and
  // still publishes nothing.
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  Require(dispatch(first, &result) != SB_ENGINE_STATUS_OK && result != nullptr &&
              DiagnosticCode(result) == "PROCESS.CANCELLED",
          "003632 cancelled receipt did not stay cancelled");
  (void)sb_engine_result_release(result);
  no_terminal_result(first, "003632 latched receipt published a receipt result");

  // Phase 2: cancellation before publication on its own receipt keeps only
  // that receipt's begun identity, which is never completed on this receipt.
  Attempt second;
  cancel_on_probe.store(0, std::memory_order_relaxed);
  prepare(&second);
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(2, std::memory_order_relaxed);
  Require(dispatch(second, &result) == SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr &&
              DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.catalog_epoch_check.cancelled_before_publication" &&
              probes.load(std::memory_order_relaxed) == 2,
          "003632 second cancellation crossed the publication barrier");
  (void)sb_engine_result_release(result);
  const auto still_begun = api::LookupSblrCatalogEpochCheckJournalV1(
      journal_context, second.journal_key);
  Require(still_begun.ok && still_begun.found &&
              still_begun.snapshot.state ==
                  api::SblrCatalogEpochCheckJournalStateV1::begun &&
              still_begun.snapshot.journal_generation == 1 &&
              still_begun.snapshot.canonical_result_bytes.empty(),
          "003632 prepublication cancellation changed durable result state");

  // Phase 3: the success receipt is captured BEFORE the competing cleanup so
  // the late-capture interleaving below still lands between its capture and
  // its observation.
  Attempt success;
  cancel_on_probe.store(0, std::memory_order_relaxed);
  prepare(&success);
  const auto& descriptor = success.descriptor;

  // Deterministic interleaving: a different active transaction is rolled back
  // after receipt capture but before observation, just as delayed session
  // cleanup can do. Do not forge an epoch, touch a timestamp, or mock a fence.
  const auto before_cleanup = db::AcquireStrongLocalTransactionInventorySnapshot(
      fixture.database_path.string());
  Require(before_cleanup.ok(), "catalog epoch pre-cleanup inventory snapshot");
  if (cleanup_after_capture) finish_other_transaction();
  const auto cleanup_fence = db::RevalidateLocalTransactionInventorySnapshot(
      *before_cleanup.snapshot);
  Require(cleanup_fence.ok() == !cleanup_after_capture,
          "real transaction cleanup must change only the late-capture fence");
  const auto before_observation = db::AcquireStrongLocalTransactionInventorySnapshot(
      fixture.database_path.string());
  Require(before_observation.ok(), "catalog epoch observation inventory snapshot");

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  Require(dispatch(success, &result) == SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003632 fresh receipt did not publish catalog-epoch result");
  const auto first_result = ResultPayload(result);
  sblr::SblrCatalogEpochCheckResultV1 decoded_result;
  Require(sblr::DecodeSblrCatalogEpochCheckResultV1(
              first_result.data(), first_result.size(), &decoded_result,
              &detail) &&
              decoded_result.check_uuid == descriptor.check_uuid &&
              // MGA snapshot: another transaction's rollback after capture
              // changes the database-wide fence (checked above) but not what
              // this statement's transaction sees, so it stays current.
              decoded_result.status == 1 &&
              decoded_result.visibility == 1 &&
              decoded_result.observed_catalog_epoch_uuid ==
                  descriptor.requested_catalog_epoch_uuid &&
              decoded_result.observed_catalog_generation ==
                  descriptor.requested_catalog_generation &&
              probes.load(std::memory_order_relaxed) == 2,
          "003632 successful dispatch omitted canonical SECR");
  const auto after_observation = db::AcquireStrongLocalTransactionInventorySnapshot(
      fixture.database_path.string());
  Require(after_observation.ok() &&
              after_observation.snapshot->publish_journal_sha256 ==
                  before_observation.snapshot->publish_journal_sha256 &&
              after_observation.snapshot->inventory_root_body_sha256 ==
                  before_observation.snapshot->inventory_root_body_sha256,
          "catalog epoch observation mutated transaction inventory");
  (void)sb_engine_result_release(result);
  {
    bridge::StatementCatalogEpochCheckAuthorityV1 authority;
    result = nullptr;
    Require(bridge::CopyStatementCatalogEpochCheckAuthorityV1(
                success.receipt, 1, &authority, &result) ==
                SB_ENGINE_STATUS_OK &&
                authority.terminal_result_published &&
                authority.canonical_terminal_result_bytes == first_result,
            "003632 terminal result was not retained for exact replay");
    if (result != nullptr) (void)sb_engine_result_release(result);
  }
  const auto published = api::LookupSblrCatalogEpochCheckJournalV1(
      journal_context, success.journal_key);
  Require(published.ok && published.found &&
              published.snapshot.state ==
                  api::SblrCatalogEpochCheckJournalStateV1::published &&
              published.snapshot.journal_generation == 2 &&
              published.snapshot.canonical_result_bytes == first_result,
          "003632 durable journal did not recover exact SECR");
  for (const auto* cancelled : {&first, &second}) {
    const auto left = api::LookupSblrCatalogEpochCheckJournalV1(
        journal_context, cancelled->journal_key);
    Require(left.ok && left.found &&
                left.snapshot.state ==
                    api::SblrCatalogEpochCheckJournalStateV1::begun &&
                left.snapshot.journal_generation == 1 &&
                left.snapshot.canonical_result_bytes.empty(),
            "003632 a cancelled receipt's begun identity changed");
  }

  // A later real inventory change must not rewrite either published outcome.
  auto later_context = BeginTransaction(fixture, &probes);
  later_context.query_cancellation_requested = {};
  api::EngineRollbackTransactionRequest later_rollback;
  later_rollback.context = later_context;
  Require(api::EngineRollbackTransaction(later_rollback).ok,
          "catalog epoch postpublication transaction cleanup failed");

  // Postpublication cancellation does not change the exact replay.
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  Require(dispatch(success, &result) == SB_ENGINE_STATUS_OK &&
              result != nullptr && ResultPayload(result) == first_result &&
              probes.load(std::memory_order_relaxed) == 0,
          "003632 postpublication cancellation retracted exact replay");
  (void)sb_engine_result_release(result);
  cancel_on_probe.store(0, std::memory_order_relaxed);

  for (auto* attempt : {&first, &second, &success})
    Require(bridge::ReleaseStatementContextReceipt(attempt->receipt) ==
                SB_ENGINE_STATUS_OK,
            "003632 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "003632 fixture transaction rollback failed");
  std::cout << "catalog_epoch_cleanup_order="
            << (cleanup_after_capture ? "after_capture" : "before_capture")
            << " status=1"
            << " cancellation_and_exact_replay=PASS\n";
}

int main() {
  RunCleanupOrderingCase(false);
  RunCleanupOrderingCase(true);
  return EXIT_SUCCESS;
}
