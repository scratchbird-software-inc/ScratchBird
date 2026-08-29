// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SCRATCHBIRD_IA01_MISSING_EXECUTOR_FIXTURE_ONLY
#include "ia01_package_missing_executor_integration_test.cpp"
#include "ia01_variable_live_fixture.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>

namespace {

struct CancellationCheckpoint {
  unsigned probe_ordinal;
  const char* message_key;
  const char* trace_name;
};

void SetDispatchTrace(const std::filesystem::path& path) {
#if defined(_WIN32)
  Require(_putenv_s("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE",
                    path.string().c_str()) == 0,
          "002336 trace environment setup failed");
#else
  Require(setenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE",
                 path.string().c_str(), 1) == 0,
          "002336 trace environment setup failed");
#endif
}

void ClearDispatchTrace() {
#if defined(_WIN32)
  Require(_putenv_s("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE", "") == 0,
          "002336 trace environment cleanup failed");
#else
  Require(unsetenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE") == 0,
          "002336 trace environment cleanup failed");
#endif
}

std::string ReadRequiredFile(const std::filesystem::path& path) {
  Require(std::filesystem::is_regular_file(path),
          "002336 expected state file is absent");
  std::ifstream input(path, std::ios::binary);
  Require(input.good(), "002336 state file open failed");
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::string ReadOptionalFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::map<std::string, std::string> SnapshotDurableState(
    const Fixture& fixture) {
  std::map<std::string, std::string> snapshot;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(fixture.directory)) {
    if (!entry.is_regular_file() || entry.path().extension() == ".trace") {
      continue;
    }
    snapshot.emplace(
        std::filesystem::relative(entry.path(), fixture.directory)
            .generic_string(),
        ReadRequiredFile(entry.path()));
  }
  const auto database_name = fixture.database_path.filename().generic_string();
  Require(snapshot.contains(database_name) &&
              snapshot.contains(database_name +
                                ".sb.sblr_variable_registry.v1") &&
              snapshot.contains(database_name +
                                ".sb.sblr_variable_frame_coordinator.v1") &&
              snapshot.contains(
                  database_name +
                  ".sb.sblr_executor_availability_registry.v1.variable"),
          "002336 exact variable state journals are absent");
  return snapshot;
}

bridge::StatementContextDispatchRequest PrepareDispatch(
    const Fixture& fixture,
    PublicSession& session,
    const variable_fixture::Live& live,
    bridge::StatementPackageAdmissionReservationHandle* reservation) {
  bridge::StatementPackageAdmissionReservationRequest reservation_request;
  reservation_request.receipt = live.receipt;
  reservation_request.canonical_payload_bytes = live.submission.stream.data();
  reservation_request.canonical_payload_size = live.submission.stream.size();
  reservation_request.payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationView reservation_view;
  sb_engine_result_t result = nullptr;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &reservation_request, reservation, &reservation_view, &result) ==
              SB_ENGINE_STATUS_OK,
          "002336 reservation acquisition failed");
  if (result) (void)sb_engine_result_release(result);

  server::ServerSblrAdmissionRequest admission;
  admission.encoded_sblr_container = live.submission.container;
  admission.encoded_execution_envelope = live.submission.ingress;
  admission.admitted_parser_package_uuid = live.parser_uuid;
  admission.admitted_parser_package_version_major = 1;
  admission.admitted_registry_snapshot_uuid = live.view.catalog_epoch_uuid;
  admission.authenticated_principal_uuid = Text(fixture.principal_uuid);
  admission.catalog_snapshot_uuid = live.view.statement_metadata_snapshot_uuid;
  admission.engine_mga_statement_uuid = live.view.statement_uuid;
  admission.engine_mga_snapshot_uuid = live.view.statement_snapshot_uuid;
  admission.catalog_epoch = live.view.catalog_generation_id;
  admission.security_epoch = live.view.security_epoch;
  admission.resource_epoch = live.view.resource_epoch;
  admission.route_snapshot_uuid = live.view.optimizer_route_snapshot_uuid;
  admission.route_epoch = live.view.optimizer_route_epoch;
  admission.route_generation = live.view.optimizer_route_generation;
  admission.security_snapshot_uuid = live.view.security_context_uuid;
  admission.security_observation_generation = live.view.security_epoch;
  admission.route_snapshot_engine_owned = true;
  admission.security_snapshot_engine_owned = true;
  admission.package_reservation_handle = reservation->opaque_id;
  admission.reserved_payload_kind =
      server::ServerSblrPayloadKind::opcode_stream;
  admission.reserved_payload_size = reservation_view.payload_size;
  admission.reserved_record_count = reservation_view.record_count;
  admission.reserved_resource_policy_generation =
      reservation_view.resource_policy_generation;
  admission.reserved_payload_sha256 = reservation_view.payload_sha256;
  const auto admitted = server::AdmitServerSblrEnvelope(admission);
  Require(admitted.admitted && admitted.admission_token,
          "002336 server admission failed");

  auto dispatch = DispatchRequest(live.receipt, session.session, *reservation,
                                  admitted.admission_token);
  dispatch.variable_execution_binding = live.sbve;
  return dispatch;
}

void RequireNoVariablePublication(sb_engine_result_t result,
                                  const std::filesystem::path& trace_path) {
  sb_engine_result_class_t result_class = SB_ENGINE_RESULT_NONE;
  Require(sb_engine_result_class(result, &result_class) == SB_ENGINE_STATUS_OK &&
              result_class == SB_ENGINE_RESULT_DIAGNOSTIC_ONLY,
          "002336 cancellation published a non-diagnostic result");

  sb_engine_execution_summary_view_v1_t summary{};
  Require(sb_engine_result_summary(result, &summary) == SB_ENGINE_STATUS_OK &&
              summary.rows_produced == 0 && summary.diagnostics_count == 1,
          "002336 cancellation published rows or an extra diagnostic");

  sb_engine_string_view_t payload{};
  Require(sb_engine_result_payload(result, &payload) == SB_ENGINE_STATUS_OK &&
              payload.size_bytes == 0,
          "002336 cancellation published a typed value or evidence payload");

  sb_engine_result_descriptor_view_v1_t descriptor{};
  descriptor.struct_size = sizeof(descriptor);
  descriptor.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  Require(sb_engine_result_descriptor_v1(result, &descriptor) ==
              SB_ENGINE_STATUS_CONFLICT,
          "002336 cancellation published a typed-value descriptor");

  bridge::StatementQueryExecuteResultHandleView query_handle;
  Require(bridge::ReadStatementQueryExecuteResultHandle(result, &query_handle) ==
              SB_ENGINE_STATUS_CONFLICT &&
              query_handle.execution_uuid.empty() &&
              query_handle.result_set_uuid.empty() &&
              query_handle.row_descriptor_uuid.empty() &&
              query_handle.snapshot_uuid.empty(),
          "002336 cancellation published a query result handle");

  const auto trace = ReadOptionalFile(trace_path);
  Require(trace.find("layer=variable_executor") == std::string::npos &&
              trace.find("result_descriptor_id=typed_value") ==
                  std::string::npos &&
              trace.find("executor_evidence_sha256") == std::string::npos &&
              trace.find("parent_success_barrier=passed") == std::string::npos,
          "002336 cancellation published variable evidence or parent success");
}

void RunCheckpoint(const CancellationCheckpoint& checkpoint) {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  auto context = BeginTransaction(fixture, &probes);
  context.query_cancellation_requested = [&probes, &checkpoint] {
    return probes.fetch_add(1, std::memory_order_relaxed) + 1 ==
           checkpoint.probe_ordinal;
  };
  auto live = variable_fixture::Build(fixture, session, &context);
  const auto state_before = SnapshotDurableState(fixture);

  bridge::StatementPackageAdmissionReservationHandle reservation;
  auto dispatch = PrepareDispatch(fixture, session, live, &reservation);
  const auto trace_path = fixture.directory / checkpoint.trace_name;
  SetDispatchTrace(trace_path);

  sb_engine_result_t result = nullptr;
  const auto status =
      bridge::DispatchStatementContextReceipt(&dispatch, &result);
  Require(status == SB_ENGINE_STATUS_TIMEOUT && result,
          "002336 cancellation status drifted");
  Require(probes.load(std::memory_order_relaxed) == checkpoint.probe_ordinal,
          "002336 cancellation checkpoint sequence drifted");

  sb_engine_diagnostic_set_view_t diagnostics{};
  Require(sb_engine_result_diagnostics(result, &diagnostics) ==
                  SB_ENGINE_STATUS_OK &&
              diagnostics.diagnostic_count == 1,
          "002336 cancellation diagnostic is absent");
  const auto& diagnostic = diagnostics.diagnostics[0];
  const std::string code(diagnostic.symbolic_code.data,
                         diagnostic.symbolic_code.size_bytes);
  const std::string key(diagnostic.message_key.data,
                        diagnostic.message_key.size_bytes);
  Require(code == "PROCESS.CANCELLED" &&
              diagnostic.severity == SB_ENGINE_DIAGNOSTIC_ERROR &&
              key == checkpoint.message_key,
          "002336 cancellation diagnostic identity drifted");
  RequireNoVariablePublication(result, trace_path);
  Require(SnapshotDurableState(fixture) == state_before,
          "002336 cancellation mutated variable or MGA state");
  (void)sb_engine_result_release(result);

  Require(bridge::ReleaseStatementPackageAdmissionReservation(
              reservation,
              bridge::StatementPackageReservationReleaseReason::kCancel) ==
              SB_ENGINE_STATUS_ALREADY_RELEASED,
          "002336 cancellation did not release its reservation");
  Require(bridge::ReleaseStatementPackageAdmissionReservation(
              reservation,
              bridge::StatementPackageReservationReleaseReason::kCancel) ==
              SB_ENGINE_STATUS_ALREADY_RELEASED,
          "002336 cancellation reservation was released more than once");

  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&dispatch, &result) ==
              SB_ENGINE_STATUS_INVALID_HANDLE,
          "002336 consumed package reservation was replayable");
  if (result) (void)sb_engine_result_release(result);
  Require(probes.load(std::memory_order_relaxed) == checkpoint.probe_ordinal,
          "002336 reservation replay reached cancellation or parent state");
  ClearDispatchTrace();

  Require(session.End() == SB_ENGINE_STATUS_OK,
          "002336 session cleanup failed");
  Require(bridge::ReleaseStatementContextReceipt(live.receipt) ==
              SB_ENGINE_STATUS_ALREADY_RELEASED,
          "002336 receipt cleanup drifted");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "002336 rollback failed");
}

}  // namespace

int main() {
  const std::array<CancellationCheckpoint, 3> checkpoints{{
      {2, "sblr.variable.cancelled_before_node", "before-node.trace"},
      {3, "sblr.variable.cancelled_after_registry_read",
       "after-registry-read.trace"},
      {4, "sblr.variable.cancelled_before_parent", "before-parent.trace"},
  }};
  for (const auto& checkpoint : checkpoints) RunCheckpoint(checkpoint);
  return EXIT_SUCCESS;
}
