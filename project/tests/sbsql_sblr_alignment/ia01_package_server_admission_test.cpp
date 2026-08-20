// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SCRATCHBIRD_IA01_PACKAGE_FIXTURE_ONLY
#include "ia01_package_cancellation_fault_test.cpp"

int main() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> cancellation_probes{0};
  auto context = BeginTransaction(fixture, &cancellation_probes);

  bridge::StatementContextAcquireRequest acquire;
  acquire.engine_context = &context;
  acquire.exact_transaction_uuid = context.transaction_uuid.canonical;
  bridge::StatementContextReceiptHandle receipt;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t acquire_result = nullptr;
  Require(bridge::AcquireStatementContextReceipt(
              session.session, &acquire, &receipt, &view, &acquire_result) ==
              SB_ENGINE_STATUS_OK,
          "live statement receipt acquisition failed");
  if (acquire_result) (void)sb_engine_result_release(acquire_result);

  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 702));
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
          "engine package pre-admission reservation failed");
  if (reservation_result) (void)sb_engine_result_release(reservation_result);

  server::ServerSblrAdmissionRequest request;
  request.encoded_sblr_container = submission.container;
  request.encoded_execution_envelope = submission.ingress;
  request.admitted_parser_package_uuid = parser_uuid;
  request.admitted_parser_package_version_major = 1;
  request.admitted_registry_snapshot_uuid = view.catalog_epoch_uuid;
  request.authenticated_principal_uuid = Text(fixture.principal_uuid);
  request.catalog_snapshot_uuid = view.statement_metadata_snapshot_uuid;
  request.engine_mga_statement_uuid = view.statement_uuid;
  request.engine_mga_snapshot_uuid = view.statement_snapshot_uuid;
  request.catalog_epoch = view.catalog_generation_id;
  request.security_epoch = view.security_epoch;
  request.resource_epoch = view.resource_epoch;
  request.route_snapshot_uuid = view.optimizer_route_snapshot_uuid;
  request.route_epoch = view.optimizer_route_epoch;
  request.route_generation = view.optimizer_route_generation;
  request.security_snapshot_uuid = view.security_context_uuid;
  request.security_observation_generation = view.security_epoch;
  request.route_snapshot_engine_owned = true;
  request.security_snapshot_engine_owned = true;
  request.package_reservation_handle = reservation_handle.opaque_id;
  request.reserved_payload_kind = server::ServerSblrPayloadKind::opcode_stream;
  request.reserved_payload_size = reservation_view.payload_size;
  request.reserved_record_count = reservation_view.record_count;
  request.reserved_resource_policy_generation =
      reservation_view.resource_policy_generation;
  request.reserved_payload_sha256 = reservation_view.payload_sha256;
  server::BindServerSblrGatewayReceiptObservation(view, &request);

  const auto admitted = server::AdmitServerSblrEnvelope(request);
  Require(admitted.admitted && admitted.admission_token &&
              admitted.admission_token->opcode_stream &&
              admitted.admission_token->gateway_evidence.source ==
                  server::ServerSblrGatewayEvidenceSource::local_observed &&
              admitted.admission_token->gateway_evidence.disposition ==
                  server::ServerSblrGatewayDisposition::pass_through &&
              !admitted.admission_token->gateway_evidence.cluster_context_active &&
              !admitted.admission_token->gateway_evidence.cluster_transaction_active &&
              !admitted.admission_token->gateway_evidence.route_fence_present,
          "live receipt package admission did not produce exact local gateway evidence");

  for (unsigned predicate = 0; predicate != 3; ++predicate) {
    auto active_view = view;
    active_view.cluster_context_active = predicate == 0;
    active_view.cluster_transaction_active = predicate == 1;
    active_view.route_fence_present = predicate == 2;
    auto clustered = request;
    server::BindServerSblrGatewayReceiptObservation(active_view, &clustered);
    const auto rejected = server::AdmitServerSblrEnvelope(clustered);
    Require(!rejected.admitted && !rejected.diagnostics.empty() &&
                rejected.diagnostics.front().code ==
                    "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
            "active engine receipt predicate did not fail closed");
  }

  auto unreserved = request;
  unreserved.package_reservation_handle = 0;
  Require(!server::AdmitServerSblrEnvelope(unreserved).admitted,
          "package admission accepted missing engine reservation");
  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "live receipt cleanup failed");
  return 0;
}
