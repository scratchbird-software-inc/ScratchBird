// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SCRATCHBIRD_IA01_PACKAGE_FIXTURE_ONLY
#include "ia01_package_cancellation_fault_test.cpp"
#include "engine/sblr/sblr_ddl_create_index_runtime.hpp"
#include "engine/sblr/sblr_plan_import_rows_codec.hpp"

namespace {

Submission BuildCreateIndexSubmission(
    const Fixture& fixture,
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    bool opcode_stream = true,
    bool multi_member = false) {
  auto submission = BuildSubmission(fixture, view, parser_uuid);
  const auto package = RawUuid(view.bound_ast_uuid);

  sblr::SblrDdlCreateIndexDescriptorV1 descriptor;
  descriptor.body[0] = 1;
  descriptor.availability = 1;
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.ddl_create_index", "SBLR_DDL_CREATE_INDEX",
      "ia01.admission.create_index");
  member.opcode_code = 1540;
  member.result_shape = "ddl_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.requires_cluster_authority = false;
  member.contains_sql_text = false;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "create_index_descriptor";
  operand.name = "index";
  operand.value_kind = sblr::SblrValueKind::create_index_descriptor;
  operand.value_body =
      sblr::EncodeSblrDdlCreateIndexDescriptorV1(descriptor, true);
  member.operands.push_back(std::move(operand));

  if (opcode_stream) {
    sblr::SblrOpcodeStream package_stream;
    package_stream.package_descriptor_uuid = view.bound_ast_uuid;
    package_stream.registry_snapshot_uuid = view.catalog_epoch_uuid;
    package_stream.operations.push_back(
        Frame(true, parser_uuid, view.catalog_epoch_uuid, package));
    package_stream.operations.push_back(member);
    if (multi_member) package_stream.operations.push_back(member);
    package_stream.operations.push_back(
        Frame(false, parser_uuid, view.catalog_epoch_uuid, package));
    submission.stream = sblr::EncodeSblrOpcodeStream(package_stream);
  } else {
    const auto sbop = sblr::EncodeSblrEnvelope(member);
    submission.stream.assign(sbop.begin(), sbop.end());
  }
  Require(!submission.stream.empty(),
          "canonical CREATE INDEX SBOS encoding failed");

  auto container = wire::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(submission.container.data()),
      submission.container.size());
  Require(container.status == wire::SblrCodecStatus::ok,
          "base canonical container decoding failed");
  container.container.canonical_anchor[100] = opcode_stream ? 1 : 2;
  container.container.canonical_anchor[101] = 0;
  container.container.operation_payload = submission.stream;
  const auto outer = wire::EncodeSblrContainer(container.container);
  Require(!outer.empty(), "canonical CREATE INDEX container encoding failed");

  auto ingress = wire::DecodeSblrExecutionEnvelopeV1Bytes(
      reinterpret_cast<const std::uint8_t*>(submission.ingress.data()),
      submission.ingress.size());
  Require(ingress.status == wire::SblrCodecStatus::ok,
          "base execution envelope decoding failed");
  auto& fields = ingress.envelope.fields;
  fields[4] = V16(opcode_stream ? 1 : 2);
  fields[5] = {0};
  fields[6] = {0};
  auto& payload_reference = opcode_stream ? fields[5] : fields[6];
  payload_reference = {1};
  U64(&payload_reference, submission.stream.size());
  payload_reference.insert(payload_reference.end(), submission.stream.begin(),
                           submission.stream.end());
  fields[7] = {1};
  U32(&fields[7], wire::SblrCrc32c(submission.stream.data(),
                                   submission.stream.size()));
  fields[8] = V64(submission.stream.size());
  const auto execution = wire::EncodeSblrExecutionEnvelopeV1(ingress.envelope);
  Require(!execution.empty(),
          "canonical CREATE INDEX execution envelope encoding failed");

  submission.container.assign(outer.begin(), outer.end());
  submission.ingress.assign(execution.begin(), execution.end());
  return submission;
}

Submission BuildPlanImportRowsSubmission(
    const Fixture& fixture,
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    bool opcode_stream = true,
    bool multi_member = false) {
  auto submission = BuildCreateIndexSubmission(
      fixture, view, parser_uuid, opcode_stream, false);

  sblr::PlanImportRowsDescriptorRefV1 descriptor;
  descriptor.descriptor_uuid = RawUuid(view.bound_ast_uuid);
  descriptor.descriptor_generation = 1;
  std::vector<std::uint8_t> descriptor_bytes;
  sblr::PlanImportRowsCodecDiagnosticV1 diagnostic;
  Require(sblr::EncodePlanImportRowsDescriptorRefV1(
              descriptor, &descriptor_bytes, &diagnostic),
          "canonical import-plan descriptor reference encoding failed");

  auto member = sblr::MakeSblrEnvelope(
      "dml.plan_import_rows", "SBLR_DML_PLAN_IMPORT_ROWS",
      "ia01.admission.plan_import_rows");
  member.opcode_code = sblr::kPlanImportRowsOpcodeCodeV1;
  member.result_shape = "import_plan_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.requires_cluster_authority = false;
  member.contains_sql_text = false;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "import_rows_plan_descriptor";
  operand.name = "request";
  operand.value_kind = sblr::SblrValueKind::descriptor_ref;
  operand.value_body = std::move(descriptor_bytes);
  member.operands.push_back(std::move(operand));

  if (opcode_stream) {
    const auto decoded = sblr::DecodeSblrOpcodeStream(std::string_view(
        reinterpret_cast<const char*>(submission.stream.data()),
        submission.stream.size()));
    Require(decoded.ok && decoded.stream.operations.size() == 3,
            "base package decoding for import planning failed");
    auto package_stream = decoded.stream;
    package_stream.operations[1] = member;
    if (multi_member) {
      package_stream.operations.insert(package_stream.operations.end() - 1,
                                       member);
    }
    submission.stream = sblr::EncodeSblrOpcodeStream(package_stream);
  } else {
    const auto sbop = sblr::EncodeSblrEnvelope(member);
    submission.stream.assign(sbop.begin(), sbop.end());
  }
  Require(!submission.stream.empty(),
          "canonical import-plan payload encoding failed");

  auto container = wire::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(submission.container.data()),
      submission.container.size());
  Require(container.status == wire::SblrCodecStatus::ok,
          "base import-plan container decoding failed");
  container.container.canonical_anchor[100] = opcode_stream ? 1 : 2;
  container.container.canonical_anchor[101] = 0;
  container.container.operation_payload = submission.stream;
  const auto outer = wire::EncodeSblrContainer(container.container);
  Require(!outer.empty(), "canonical import-plan container encoding failed");

  auto ingress = wire::DecodeSblrExecutionEnvelopeV1Bytes(
      reinterpret_cast<const std::uint8_t*>(submission.ingress.data()),
      submission.ingress.size());
  Require(ingress.status == wire::SblrCodecStatus::ok,
          "base import-plan execution envelope decoding failed");
  auto& fields = ingress.envelope.fields;
  fields[4] = V16(opcode_stream ? 1 : 2);
  fields[5] = {0};
  fields[6] = {0};
  auto& payload_reference = opcode_stream ? fields[5] : fields[6];
  payload_reference = {1};
  U64(&payload_reference, submission.stream.size());
  payload_reference.insert(payload_reference.end(), submission.stream.begin(),
                           submission.stream.end());
  fields[7] = {1};
  U32(&fields[7], wire::SblrCrc32c(submission.stream.data(),
                                   submission.stream.size()));
  fields[8] = V64(submission.stream.size());
  const auto execution = wire::EncodeSblrExecutionEnvelopeV1(ingress.envelope);
  Require(!execution.empty(),
          "canonical import-plan execution envelope encoding failed");

  submission.container.assign(outer.begin(), outer.end());
  submission.ingress.assign(execution.begin(), execution.end());
  return submission;
}

}  // namespace

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

  const auto create_index_submission =
      BuildCreateIndexSubmission(fixture, view, parser_uuid);
  bridge::StatementPackageAdmissionReservationRequest
      create_index_reservation_request;
  create_index_reservation_request.receipt = receipt;
  create_index_reservation_request.canonical_payload_bytes =
      create_index_submission.stream.data();
  create_index_reservation_request.canonical_payload_size =
      create_index_submission.stream.size();
  create_index_reservation_request.payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationHandle
      create_index_reservation_handle;
  bridge::StatementPackageAdmissionReservationView
      create_index_reservation_view;
  reservation_result = nullptr;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &create_index_reservation_request,
              &create_index_reservation_handle,
              &create_index_reservation_view,
              &reservation_result) == SB_ENGINE_STATUS_OK,
          "CREATE INDEX package pre-admission reservation failed");
  if (reservation_result) (void)sb_engine_result_release(reservation_result);

  auto create_index_request = request;
  create_index_request.encoded_sblr_container =
      create_index_submission.container;
  create_index_request.encoded_execution_envelope =
      create_index_submission.ingress;
  create_index_request.package_reservation_handle =
      create_index_reservation_handle.opaque_id;
  create_index_request.reserved_payload_size =
      create_index_reservation_view.payload_size;
  create_index_request.reserved_record_count =
      create_index_reservation_view.record_count;
  create_index_request.reserved_resource_policy_generation =
      create_index_reservation_view.resource_policy_generation;
  create_index_request.reserved_payload_sha256 =
      create_index_reservation_view.payload_sha256;
  const auto create_index_refused =
      server::AdmitServerSblrEnvelope(create_index_request);
  Require(!create_index_refused.admitted &&
              !create_index_refused.admission_token &&
              !create_index_refused.diagnostics.empty() &&
              create_index_refused.diagnostics.front().code ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "canonical CREATE INDEX admission did not refuse before token publication");
  Require(cancellation_probes.load(std::memory_order_relaxed) == 0,
          "CREATE INDEX admission consulted cancellation before evidence refusal");

  auto deferred_create_index = create_index_request;
  deferred_create_index.package_reservation_deferred = true;
  deferred_create_index.package_reservation_handle = 0;
  const auto deferred_create_index_refused =
      server::AdmitServerSblrEnvelope(deferred_create_index);
  Require(!deferred_create_index_refused.admitted &&
              !deferred_create_index_refused.admission_token &&
              !deferred_create_index_refused.diagnostics.empty() &&
              deferred_create_index_refused.diagnostics.front().code ==
                  "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "deferred CREATE INDEX admission bypassed static evidence refusal");

  const auto multi_create_index_submission =
      BuildCreateIndexSubmission(fixture, view, parser_uuid, true, true);
  auto multi_create_index_request = request;
  multi_create_index_request.encoded_sblr_container =
      multi_create_index_submission.container;
  multi_create_index_request.encoded_execution_envelope =
      multi_create_index_submission.ingress;
  multi_create_index_request.package_reservation_deferred = true;
  multi_create_index_request.package_reservation_handle = 0;
  const auto multi_create_index_refused =
      server::AdmitServerSblrEnvelope(multi_create_index_request);
  Require(!multi_create_index_refused.admitted &&
              !multi_create_index_refused.admission_token &&
              !multi_create_index_refused.diagnostics.empty() &&
              multi_create_index_refused.diagnostics.front().code ==
                  "SBLR.OPERAND_INVALID",
          "deferred multi-member CREATE INDEX bypassed standalone-root refusal");

  const auto inline_create_index_submission =
      BuildCreateIndexSubmission(fixture, view, parser_uuid, false);
  auto inline_create_index_request = request;
  inline_create_index_request.encoded_sblr_container =
      inline_create_index_submission.container;
  inline_create_index_request.encoded_execution_envelope =
      inline_create_index_submission.ingress;
  inline_create_index_request.package_reservation_handle = 0;
  const auto inline_create_index_refused =
      server::AdmitServerSblrEnvelope(inline_create_index_request);
  Require(!inline_create_index_refused.admitted &&
              !inline_create_index_refused.admission_token &&
              !inline_create_index_refused.diagnostics.empty() &&
              inline_create_index_refused.diagnostics.front().code ==
                  "SBLR.OPERAND_INVALID",
          "inline CREATE INDEX admission bypassed package-root placement refusal");

  auto create_index_cluster_view = view;
  create_index_cluster_view.cluster_transaction_active = true;
  auto create_index_clustered = create_index_request;
  server::BindServerSblrGatewayReceiptObservation(create_index_cluster_view,
                                                   &create_index_clustered);
  const auto create_index_cluster_refused =
      server::AdmitServerSblrEnvelope(create_index_clustered);
  Require(!create_index_cluster_refused.admitted &&
              !create_index_cluster_refused.admission_token &&
              !create_index_cluster_refused.diagnostics.empty() &&
              create_index_cluster_refused.diagnostics.front().code ==
                  "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
          "cluster CREATE INDEX refusal did not precede executor evidence");

  const auto plan_import_rows_submission =
      BuildPlanImportRowsSubmission(fixture, view, parser_uuid);
  bridge::StatementPackageAdmissionReservationRequest
      plan_import_rows_reservation_request;
  plan_import_rows_reservation_request.receipt = receipt;
  plan_import_rows_reservation_request.canonical_payload_bytes =
      plan_import_rows_submission.stream.data();
  plan_import_rows_reservation_request.canonical_payload_size =
      plan_import_rows_submission.stream.size();
  plan_import_rows_reservation_request.payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationHandle
      plan_import_rows_reservation_handle;
  bridge::StatementPackageAdmissionReservationView
      plan_import_rows_reservation_view;
  reservation_result = nullptr;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &plan_import_rows_reservation_request,
              &plan_import_rows_reservation_handle,
              &plan_import_rows_reservation_view,
              &reservation_result) == SB_ENGINE_STATUS_OK,
          "import-plan package pre-admission reservation failed");
  if (reservation_result) (void)sb_engine_result_release(reservation_result);

  auto plan_import_rows_request = request;
  plan_import_rows_request.encoded_sblr_container =
      plan_import_rows_submission.container;
  plan_import_rows_request.encoded_execution_envelope =
      plan_import_rows_submission.ingress;
  plan_import_rows_request.package_reservation_handle =
      plan_import_rows_reservation_handle.opaque_id;
  plan_import_rows_request.reserved_payload_size =
      plan_import_rows_reservation_view.payload_size;
  plan_import_rows_request.reserved_record_count =
      plan_import_rows_reservation_view.record_count;
  plan_import_rows_request.reserved_resource_policy_generation =
      plan_import_rows_reservation_view.resource_policy_generation;
  plan_import_rows_request.reserved_payload_sha256 =
      plan_import_rows_reservation_view.payload_sha256;
  const auto plan_import_rows_admitted =
      server::AdmitServerSblrEnvelope(plan_import_rows_request);
  Require(plan_import_rows_admitted.admitted &&
              plan_import_rows_admitted.admission_token &&
              plan_import_rows_admitted.operation_id ==
                  "dml.plan_import_rows" &&
              plan_import_rows_admitted.admission_token->opcode_stream &&
              plan_import_rows_admitted.admission_token->stream.operations.size() ==
                  3,
          "canonical import plan was not admitted as one standalone package root");

  const auto multi_plan_import_rows_submission =
      BuildPlanImportRowsSubmission(fixture, view, parser_uuid, true, true);
  auto multi_plan_import_rows_request = request;
  multi_plan_import_rows_request.encoded_sblr_container =
      multi_plan_import_rows_submission.container;
  multi_plan_import_rows_request.encoded_execution_envelope =
      multi_plan_import_rows_submission.ingress;
  multi_plan_import_rows_request.package_reservation_deferred = true;
  multi_plan_import_rows_request.package_reservation_handle = 0;
  const auto multi_plan_import_rows_refused =
      server::AdmitServerSblrEnvelope(multi_plan_import_rows_request);
  Require(!multi_plan_import_rows_refused.admitted &&
              !multi_plan_import_rows_refused.admission_token &&
              !multi_plan_import_rows_refused.diagnostics.empty() &&
              multi_plan_import_rows_refused.diagnostics.front().code ==
                  "SBLR.OPERAND_INVALID",
          "multi-member import plan bypassed standalone-root refusal");

  const auto inline_plan_import_rows_submission =
      BuildPlanImportRowsSubmission(fixture, view, parser_uuid, false);
  auto inline_plan_import_rows_request = request;
  inline_plan_import_rows_request.encoded_sblr_container =
      inline_plan_import_rows_submission.container;
  inline_plan_import_rows_request.encoded_execution_envelope =
      inline_plan_import_rows_submission.ingress;
  inline_plan_import_rows_request.package_reservation_handle = 0;
  const auto inline_plan_import_rows_refused =
      server::AdmitServerSblrEnvelope(inline_plan_import_rows_request);
  Require(!inline_plan_import_rows_refused.admitted &&
              !inline_plan_import_rows_refused.admission_token &&
              !inline_plan_import_rows_refused.diagnostics.empty() &&
              inline_plan_import_rows_refused.diagnostics.front().code ==
                  "SBLR.OPERAND_INVALID",
          "inline import plan bypassed package-root placement refusal");

  auto plan_import_rows_cluster_view = view;
  plan_import_rows_cluster_view.cluster_transaction_active = true;
  auto plan_import_rows_clustered = plan_import_rows_request;
  server::BindServerSblrGatewayReceiptObservation(
      plan_import_rows_cluster_view, &plan_import_rows_clustered);
  const auto plan_import_rows_cluster_refused =
      server::AdmitServerSblrEnvelope(plan_import_rows_clustered);
  Require(!plan_import_rows_cluster_refused.admitted &&
              !plan_import_rows_cluster_refused.admission_token &&
              !plan_import_rows_cluster_refused.diagnostics.empty() &&
              plan_import_rows_cluster_refused.diagnostics.front().code ==
                  "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN",
          "cluster import plan received a local fallback");

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
