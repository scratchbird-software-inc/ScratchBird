// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the production-boundary fixture and canonical package builders without
// introducing a mock server, engine, MGA, gateway, or resource service.
#define SCRATCHBIRD_IA01_PACKAGE_FIXTURE_ONLY
#include "ia01_package_cancellation_fault_test.cpp"

#include "hash_digest.hpp"

namespace {
std::array<std::uint8_t, 32> Binding(
    const bridge::StatementContextDispatchRequest& request) {
  Bytes bytes;
  constexpr std::string_view domain = "ScratchBird.SBLR.AdmissionToken.V1";
  bytes.insert(bytes.end(), domain.begin(), domain.end());
  bytes.insert(bytes.end(), request.container_sha256.begin(),
               request.container_sha256.end());
  bytes.insert(bytes.end(), request.execution_envelope_sha256.begin(),
               request.execution_envelope_sha256.end());
  bytes.insert(bytes.end(), request.operation_sha256.begin(),
               request.operation_sha256.end());
  for (const auto* value : {&request.authenticated_principal_uuid,
                            &request.catalog_snapshot_uuid,
                            &request.engine_mga_statement_uuid,
                            &request.engine_mga_snapshot_uuid}) {
    bytes.insert(bytes.end(), value->begin(), value->end());
    bytes.push_back(0);
  }
  wire::SblrAppendU64(bytes, request.catalog_epoch);
  wire::SblrAppendU64(bytes, request.security_epoch);
  wire::SblrAppendU64(bytes, request.resource_epoch);
  wire::SblrAppendU64(bytes,
                      request.package_admission_reservation.opaque_id);
  bytes.push_back(static_cast<std::uint8_t>(request.admitted_payload_kind));
  wire::SblrAppendU64(bytes, request.canonical_operation_bytes.size());
  wire::SblrAppendU32(bytes, 3);
  wire::SblrAppendU64(bytes, request.resource_epoch);
  bytes.push_back(static_cast<std::uint8_t>(request.gateway_evidence.source));
  bytes.push_back(
      static_cast<std::uint8_t>(request.gateway_evidence.disposition));
  wire::SblrAppendU64(
      bytes, request.gateway_evidence.provider_observation_generation);
  bytes.insert(bytes.end(),
               request.gateway_evidence.canonical_payload_sha256.begin(),
               request.gateway_evidence.canonical_payload_sha256.end());
  for (const auto* value : {&request.gateway_evidence.route_snapshot_uuid,
                            &request.gateway_evidence.security_snapshot_uuid}) {
    bytes.insert(bytes.end(), value->begin(), value->end());
    bytes.push_back(0);
  }
  wire::SblrAppendU64(bytes, request.gateway_evidence.route_epoch);
  wire::SblrAppendU64(bytes, request.gateway_evidence.route_generation);
  wire::SblrAppendU64(bytes, request.gateway_evidence.security_epoch);
  wire::SblrAppendU64(
      bytes, request.gateway_evidence.security_observation_generation);
  bytes.push_back(request.gateway_evidence.cluster_context_active ? 1 : 0);
  bytes.push_back(request.gateway_evidence.cluster_transaction_active ? 1 : 0);
  bytes.push_back(request.gateway_evidence.route_fence_present ? 1 : 0);
  for (const auto* value : {
           &request.package_executor_evidence.begin_executor_id,
           &request.package_executor_evidence.end_executor_id,
           &request.package_executor_evidence.registry_snapshot_uuid}) {
    bytes.insert(bytes.end(), value->begin(), value->end());
    bytes.push_back(0);
  }
  wire::SblrAppendU64(
      bytes,
      request.package_executor_evidence.executor_evidence_generation);
  bytes.insert(
      bytes.end(),
      request.package_executor_evidence.canonical_payload_sha256.begin(),
      request.package_executor_evidence.canonical_payload_sha256.end());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes);
  Require(digest.ok(), "admission binding SHA-256 failed");
  return digest.digest;
}

bridge::StatementContextDispatchRequest DispatchRequest(
    bridge::StatementContextReceiptHandle receipt,
    sb_engine_session_t session,
    bridge::StatementPackageAdmissionReservationHandle reservation,
    const server::ServerSblrAdmissionToken& token) {
  bridge::StatementContextDispatchRequest request;
  request.receipt = receipt;
  request.engine_session = session;
  request.canonical_container_bytes = token->canonical_container_bytes;
  request.canonical_execution_envelope_bytes =
      token->canonical_execution_envelope_bytes;
  request.canonical_operation_bytes = token->canonical_operation_bytes;
  request.container_sha256 = token->container_sha256;
  request.execution_envelope_sha256 = token->execution_envelope_sha256;
  request.operation_sha256 = token->operation_sha256;
  request.authenticated_principal_uuid = token->authenticated_principal_uuid;
  request.catalog_snapshot_uuid = token->catalog_snapshot_uuid;
  request.engine_mga_statement_uuid = token->engine_mga_statement_uuid;
  request.engine_mga_snapshot_uuid = token->engine_mga_snapshot_uuid;
  request.catalog_epoch = token->catalog_epoch;
  request.security_epoch = token->security_epoch;
  request.resource_epoch = token->resource_epoch;
  request.package_admission_reservation = reservation;
  request.admitted_payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  request.gateway_evidence.source =
      bridge::StatementGatewayEvidenceSource::kLocalObserved;
  request.gateway_evidence.disposition =
      bridge::StatementGatewayDisposition::kPassThrough;
  request.gateway_evidence.provider_observation_generation =
      token->gateway_evidence.provider_observation_generation;
  request.gateway_evidence.canonical_payload_sha256 =
      token->gateway_evidence.canonical_payload_sha256;
  request.gateway_evidence.route_snapshot_uuid =
      token->gateway_evidence.route_snapshot_uuid;
  request.gateway_evidence.route_epoch = token->gateway_evidence.route_epoch;
  request.gateway_evidence.route_generation =
      token->gateway_evidence.route_generation;
  request.gateway_evidence.security_snapshot_uuid =
      token->gateway_evidence.security_snapshot_uuid;
  request.gateway_evidence.security_epoch =
      token->gateway_evidence.security_epoch;
  request.gateway_evidence.security_observation_generation =
      token->gateway_evidence.security_observation_generation;
  request.gateway_evidence.cluster_context_active =
      token->gateway_evidence.cluster_context_active;
  request.gateway_evidence.cluster_transaction_active =
      token->gateway_evidence.cluster_transaction_active;
  request.gateway_evidence.route_fence_present =
      token->gateway_evidence.route_fence_present;
  request.package_executor_evidence.begin_executor_id =
      token->package_executor_evidence.begin_executor_id;
  request.package_executor_evidence.end_executor_id =
      token->package_executor_evidence.end_executor_id;
  request.package_executor_evidence.registry_snapshot_uuid =
      token->package_executor_evidence.registry_snapshot_uuid;
  request.package_executor_evidence.executor_evidence_generation =
      token->package_executor_evidence.executor_evidence_generation;
  request.package_executor_evidence.canonical_payload_sha256 =
      token->package_executor_evidence.canonical_payload_sha256;
  request.admission_binding_sha256 = token->admission_binding_sha256;
  return request;
}
}  // namespace

#ifndef SCRATCHBIRD_IA01_MISSING_EXECUTOR_FIXTURE_ONLY
int main() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> cancellation_probes{0};
  auto context = BeginTransaction(fixture, &cancellation_probes);
  context.query_cancellation_requested = [&cancellation_probes] {
    cancellation_probes.fetch_add(1, std::memory_order_relaxed);
    return false;
  };
  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t acquire_result = nullptr;
  Require(bridge::AcquireStatementContextReceipt(
              session.session, &acquire, &receipt, &view, &acquire_result) ==
              SB_ENGINE_STATUS_OK,
          "CSC-TEST-002319|CSC-TEST-002323 receipt acquisition failed");
  if (acquire_result) (void)sb_engine_result_release(acquire_result);

  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 602));
  const auto submission = BuildSubmission(fixture, view, parser_uuid);
  bridge::StatementPackageAdmissionReservationRequest reservation_request;
  reservation_request.receipt = receipt;
  reservation_request.canonical_payload_bytes = submission.stream.data();
  reservation_request.canonical_payload_size = submission.stream.size();
  reservation_request.payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationHandle reservation_handle;
  bridge::StatementPackageAdmissionReservationView reservation_view;
  sb_engine_result_t reservation_result = nullptr;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &reservation_request, &reservation_handle, &reservation_view,
              &reservation_result) == SB_ENGINE_STATUS_OK,
          "CSC-TEST-002319|CSC-TEST-002323 reservation acquisition failed");
  if (reservation_result) (void)sb_engine_result_release(reservation_result);
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
  admission.package_reservation_handle = reservation_handle.opaque_id;
  admission.reserved_payload_kind = server::ServerSblrPayloadKind::opcode_stream;
  admission.reserved_payload_size = reservation_view.payload_size;
  admission.reserved_record_count = reservation_view.record_count;
  admission.reserved_resource_policy_generation =
      reservation_view.resource_policy_generation;
  admission.reserved_payload_sha256 = reservation_view.payload_sha256;
  const auto admitted = server::AdmitServerSblrEnvelope(admission);
  Require(admitted.admitted && admitted.admission_token,
          "CSC-TEST-002319|CSC-TEST-002323 canonical package admission failed");

  auto dispatch = DispatchRequest(receipt, session.session, reservation_handle,
                                  admitted.admission_token);
  // Model a stale exact executor registry observation, then re-bind the whole
  // immutable token. This proves the executor gate itself, not hash tampering.
  ++dispatch.package_executor_evidence.executor_evidence_generation;
  dispatch.admission_binding_sha256 = Binding(dispatch);
  sb_engine_result_t result = nullptr;
  const auto status =
      bridge::DispatchStatementContextReceipt(&dispatch, &result);
  if (status != SB_ENGINE_STATUS_UNSUPPORTED) {
    std::cerr << "missing-executor status=" << status << '\n';
    if (result != nullptr) {
      sb_engine_diagnostic_set_view_t observed{};
      if (sb_engine_result_diagnostics(result, &observed) ==
          SB_ENGINE_STATUS_OK) {
        for (std::uint64_t index = 0; index != observed.diagnostic_count;
             ++index) {
          std::cerr.write(observed.diagnostics[index].symbolic_code.data,
                          observed.diagnostics[index].symbolic_code.size_bytes);
          std::cerr << '\n';
        }
      }
    }
  }
  Require(status == SB_ENGINE_STATUS_UNSUPPORTED && result != nullptr,
          "CSC-TEST-002319|CSC-TEST-002323 stale executor evidence dispatched");
  sb_engine_diagnostic_set_view_t diagnostics{};
  Require(sb_engine_result_diagnostics(result, &diagnostics) ==
              SB_ENGINE_STATUS_OK &&
              diagnostics.diagnostic_count == 1 &&
              std::string_view(diagnostics.diagnostics[0].symbolic_code.data,
                               diagnostics.diagnostics[0].symbolic_code.size_bytes) ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "CSC-TEST-002319|CSC-TEST-002323 exact diagnostic drifted");
  sb_engine_command_completion_view_v1_t completion{};
  const auto completion_status = sb_engine_result_completion(result, &completion);
  Require(completion_status != SB_ENGINE_STATUS_OK ||
              (completion.operation_id.size_bytes == 0 &&
               completion.affected_rows == 0),
          "CSC-TEST-002319|CSC-TEST-002323 contained result was published");
  Require(cancellation_probes.load(std::memory_order_relaxed) == 1,
          "CSC-TEST-002319|CSC-TEST-002323 passed the predecode boundary");
  (void)sb_engine_result_release(result);
  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "CSC-TEST-002319|CSC-TEST-002323 receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "CSC-TEST-002319|CSC-TEST-002323 MGA cleanup failed");
  return EXIT_SUCCESS;
}
#endif
