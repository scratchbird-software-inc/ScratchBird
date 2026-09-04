#pragma once

#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include "ia01_literal_live_fixture.hpp"

inline int RunLiteralRefusalScenario(bool cancel_after_admission) {
  struct FixtureCleanupEvidence {
    std::filesystem::path path;
    ~FixtureCleanupEvidence() {
      if (!path.empty() && std::filesystem::exists(path)) {
        Fail("literal fixture cleanup failed; state is quarantined from reuse");
      }
    }
  } cleanup_evidence;
  auto fixture = CreateFixture();
  cleanup_evidence.path = fixture.directory;
  Require(fixture.database_uuid.value != fixture.filespace_uuid.value &&
              fixture.database_uuid.value != fixture.principal_uuid.value &&
              fixture.database_uuid.value != fixture.session_uuid.value,
          "literal fixture UUID scopes are contaminated");
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  auto context = BeginTransaction(fixture, &probes);
  if (!cancel_after_admission) {
    context.trace_tags.push_back(
        "right:SBLR_EXECUTOR_AVAILABILITY_ADMIN");
  }
  // The literal binding phase is intentionally allowed to complete before
  // this scenario revokes the executor.  Bootstrap the exact literal row
  // through the durable availability registry; the later revoke remains the
  // missing-evidence condition under test.
  const api::SblrExecutorAvailabilityRowIdentity literal_identity{
      api::kSblrLiteralExecutorId,
      api::kSblrLiteralOpcodeCode,
      api::kSblrLiteralOpcodeVersion,
      api::kSblrLiteralOperandDescriptorId,
      api::kSblrLiteralResultDescriptorId,
      api::kSblrLiteralResultDescriptorVersion};
  const auto literal_bootstrap =
      api::LoadSblrExecutorAvailabilitySnapshot(context, literal_identity);
  Require(literal_bootstrap.ok && literal_bootstrap.snapshot.installed,
          "literal executor availability bootstrap failed");
  auto literal_admin_context = context;
  literal_admin_context.trace_tags.push_back(
      "right:SBLR_EXECUTOR_AVAILABILITY_ADMIN");
  api::SblrExecutorAvailabilitySetRequest literal_install;
  literal_install.database_uuid = context.database_uuid.canonical;
  literal_install.expected_snapshot_uuid =
      literal_bootstrap.snapshot.snapshot_uuid;
  literal_install.expected_generation = literal_bootstrap.snapshot.generation;
  literal_install.exact_row_identity = literal_identity;
  literal_install.requested_state =
      api::SblrExecutorAvailabilityState::installed;
  literal_install.reason_code = "test.literal_binding_bootstrap";
  const auto installed = api::SetSblrExecutorAvailability(
      literal_admin_context, literal_install);
  Require(installed.ok && installed.snapshot.installed,
          "literal executor availability install failed");
  context.query_cancellation_requested = [&probes, cancel_after_admission] {
    probes.fetch_add(1, std::memory_order_relaxed);
    return cancel_after_admission;
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
          cancel_after_admission ? "CSC-TEST-002328 receipt acquisition failed"
                                 : "CSC-TEST-002327 receipt acquisition failed");
  if (result) (void)sb_engine_result_release(result);
  auto literal = literal_fixture::FinalizeLiteral(receipt, view);
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 8327));
  const auto submission = literal_fixture::BuildLiteralSubmission(
      fixture, view, parser_uuid, &literal);

  bridge::StatementPackageAdmissionReservationRequest reservation_request;
  reservation_request.receipt = receipt;
  reservation_request.canonical_payload_bytes = submission.stream.data();
  reservation_request.canonical_payload_size = submission.stream.size();
  reservation_request.payload_kind = bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationHandle reservation;
  bridge::StatementPackageAdmissionReservationView reservation_view;
  result = nullptr;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &reservation_request, &reservation, &reservation_view, &result) ==
              SB_ENGINE_STATUS_OK,
          "literal reservation acquisition failed");
  if (result) (void)sb_engine_result_release(result);

  server::ServerSblrAdmissionRequest admission;
  admission.encoded_sblr_container = submission.container;
  admission.encoded_execution_envelope = submission.ingress;
  admission.admitted_parser_package_uuid = parser_uuid;
  admission.admitted_parser_package_version_major = 1;
  admission.admitted_registry_snapshot_uuid = view.catalog_epoch_uuid;
  admission.authenticated_principal_uuid = Text(fixture.principal_uuid);
  admission.catalog_snapshot_uuid = view.statement_metadata_snapshot_uuid;
  admission.engine_mga_statement_uuid = view.statement_uuid;
  admission.engine_mga_snapshot_uuid = view.statement_snapshot_uuid;
  admission.catalog_epoch = view.catalog_generation_id;
  admission.security_epoch = view.security_epoch;
  admission.resource_epoch = view.resource_epoch;
  admission.route_snapshot_uuid = view.optimizer_route_snapshot_uuid;
  admission.route_epoch = view.optimizer_route_epoch;
  admission.route_generation = view.optimizer_route_generation;
  admission.security_snapshot_uuid = view.security_context_uuid;
  admission.security_observation_generation = view.security_epoch;
  admission.route_snapshot_engine_owned = true;
  admission.security_snapshot_engine_owned = true;
  admission.package_reservation_handle = reservation.opaque_id;
  admission.reserved_payload_kind = server::ServerSblrPayloadKind::opcode_stream;
  admission.reserved_payload_size = reservation_view.payload_size;
  admission.reserved_record_count = reservation_view.record_count;
  admission.reserved_resource_policy_generation =
      reservation_view.resource_policy_generation;
  admission.reserved_payload_sha256 = reservation_view.payload_sha256;
  const auto admitted = server::AdmitServerSblrEnvelope(admission);
  Require(admitted.admitted && admitted.admission_token,
          "literal server admission failed");
  auto dispatch = DispatchRequest(receipt, session.session, reservation,
                                  admitted.admission_token);
  dispatch.literal_execution_binding = literal.sbel;
  if (!cancel_after_admission) {
    const auto installed =
        api::LoadSblrExecutorAvailabilitySnapshot(context);
    Require(installed.ok && installed.snapshot.installed,
            "CSC-TEST-002327 installed executor snapshot missing");
    api::SblrExecutorAvailabilitySetRequest revoke;
    revoke.database_uuid = context.database_uuid.canonical;
    revoke.expected_snapshot_uuid = installed.snapshot.snapshot_uuid;
    revoke.expected_generation = installed.snapshot.generation;
    revoke.requested_state =
        api::SblrExecutorAvailabilityState::revoked;
    revoke.reason_code = "CSC-TEST-002327";
    const auto revoked = api::SetSblrExecutorAvailability(context, revoke);
    Require(revoked.ok && !revoked.snapshot.installed &&
                revoked.snapshot.generation == installed.snapshot.generation + 1,
            "CSC-TEST-002327 executor revoke was not durably published");
  }

  result = nullptr;
  const auto status = bridge::DispatchStatementContextReceipt(&dispatch, &result);
  Require(result != nullptr, "literal refusal result was absent");
  sb_engine_diagnostic_set_view_t diagnostics{};
  Require(sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK &&
              diagnostics.diagnostic_count == 1,
          "literal refusal diagnostic set drifted");
  const std::string_view code(diagnostics.diagnostics[0].symbolic_code.data,
                              diagnostics.diagnostics[0].symbolic_code.size_bytes);
  const auto wanted = cancel_after_admission
      ? std::string_view{"PROCESS.CANCELLED"}
      : std::string_view{"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"};
  Require(code == wanted, std::string(cancel_after_admission
                              ? "CSC-TEST-002328 exact cancellation diagnostic drifted: "
                              : "CSC-TEST-002327 exact missing-evidence diagnostic drifted: ") +
                              std::string(code));
  Require(status == (cancel_after_admission ? SB_ENGINE_STATUS_TIMEOUT
                                            : SB_ENGINE_STATUS_UNSUPPORTED),
          "literal refusal status drifted");
  if (cancel_after_admission) {
    Require(probes.load(std::memory_order_relaxed) == 1,
            "CSC-TEST-002328 cancellation did not occur at the sole pre-node boundary");
  }
  sb_engine_execution_summary_view_v1_t summary{};
  sb_engine_command_completion_view_v1_t completion{};
  Require(sb_engine_result_summary(result, &summary) != SB_ENGINE_STATUS_OK ||
              summary.rows_produced == 0,
          "literal refusal published parent rows");
  Require(sb_engine_result_completion(result, &completion) != SB_ENGINE_STATUS_OK ||
              (completion.operation_id.size_bytes == 0 &&
               completion.affected_rows == 0),
          "literal refusal published parent completion");
  (void)sb_engine_result_release(result);

  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&dispatch, &result) ==
              SB_ENGINE_STATUS_INVALID_HANDLE,
          "literal refusal reservation replay was admitted");
  if (cancel_after_admission) {
    Require(probes.load(std::memory_order_relaxed) == 1,
            "CSC-TEST-002328 replay triggered an automatic retry");
  }
  if (result) (void)sb_engine_result_release(result);
  Require(session.End() == SB_ENGINE_STATUS_OK,
          "literal refusal session cleanup failed");
  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_ALREADY_RELEASED,
          "literal refusal receipt cleanup drifted");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "literal refusal MGA rollback failed");
  return EXIT_SUCCESS;
}
