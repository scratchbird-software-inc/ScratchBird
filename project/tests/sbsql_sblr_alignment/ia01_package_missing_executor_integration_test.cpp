// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the production-boundary fixture and canonical package builders without
// introducing a mock server, engine, MGA, gateway, or resource service.
#define SCRATCHBIRD_IA01_PACKAGE_FIXTURE_ONLY
#include "ia01_package_cancellation_fault_test.cpp"

#include "engine/internal_api/sblr_executor_availability_registry.hpp"
#include "engine/sblr/sblr_ddl_alter_type_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_index_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_type_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_type_runtime.hpp"
#include "hash_digest.hpp"

namespace {
Submission BuildCreateIndexSubmission(
    const Fixture& fixture,
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid) {
  auto submission = BuildSubmission(fixture, view, parser_uuid);
  const auto package = RawUuid(view.bound_ast_uuid);

  sblr::SblrDdlCreateIndexDescriptorV1 descriptor;
  descriptor.body[0] = 1;
  descriptor.availability = 1;
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.ddl_create_index", "SBLR_DDL_CREATE_INDEX",
      "ia01.public_abi.create_index");
  member.opcode_code = 1540;
  member.result_shape = "ddl_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "create_index_descriptor";
  operand.name = "index";
  operand.value_kind = sblr::SblrValueKind::create_index_descriptor;
  operand.value_body =
      sblr::EncodeSblrDdlCreateIndexDescriptorV1(descriptor, true);
  member.operands.push_back(std::move(operand));

  sblr::SblrOpcodeStream package_stream;
  package_stream.package_descriptor_uuid = view.bound_ast_uuid;
  package_stream.registry_snapshot_uuid = view.catalog_epoch_uuid;
  package_stream.operations.push_back(
      Frame(true, parser_uuid, view.catalog_epoch_uuid, package));
  package_stream.operations.push_back(std::move(member));
  package_stream.operations.push_back(
      Frame(false, parser_uuid, view.catalog_epoch_uuid, package));
  submission.stream = sblr::EncodeSblrOpcodeStream(package_stream);
  Require(!submission.stream.empty(),
          "canonical CREATE INDEX SBOS encoding failed");

  auto container = wire::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(submission.container.data()),
      submission.container.size());
  Require(container.status == wire::SblrCodecStatus::ok,
          "base CREATE INDEX container decoding failed");
  container.container.canonical_anchor[100] = 1;
  container.container.canonical_anchor[101] = 0;
  container.container.operation_payload = submission.stream;
  const auto outer = wire::EncodeSblrContainer(container.container);
  Require(!outer.empty(), "CREATE INDEX container encoding failed");

  auto ingress = wire::DecodeSblrExecutionEnvelopeV1Bytes(
      reinterpret_cast<const std::uint8_t*>(submission.ingress.data()),
      submission.ingress.size());
  Require(ingress.status == wire::SblrCodecStatus::ok,
          "base CREATE INDEX execution envelope decoding failed");
  auto& fields = ingress.envelope.fields;
  fields[4] = V16(1);
  fields[5] = {1};
  U64(&fields[5], submission.stream.size());
  fields[5].insert(fields[5].end(), submission.stream.begin(),
                   submission.stream.end());
  fields[6] = {0};
  fields[7] = {1};
  U32(&fields[7], wire::SblrCrc32c(submission.stream.data(),
                                   submission.stream.size()));
  fields[8] = V64(submission.stream.size());
  const auto execution = wire::EncodeSblrExecutionEnvelopeV1(ingress.envelope);
  Require(!execution.empty(), "CREATE INDEX execution envelope encoding failed");

  submission.container.assign(outer.begin(), outer.end());
  submission.ingress.assign(execution.begin(), execution.end());
  return submission;
}

struct TypeDdlProfile {
  const char* operation_id;
  const char* opcode;
  std::uint16_t opcode_code;
  const char* operand_type;
  sblr::SblrValueKind value_kind;
};

Submission BuildTypeDdlSubmission(
    const Fixture& fixture,
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const TypeDdlProfile& profile) {
  auto submission = BuildSubmission(fixture, view, parser_uuid);
  auto operation = sblr::MakeSblrEnvelope(
      profile.operation_id, profile.opcode, "ia01.public_abi.ddl_type");
  operation.opcode_code = profile.opcode_code;
  operation.result_shape = "ddl_result";
  operation.diagnostic_shape = "diagnostic_vector";
  operation.parser_package_uuid = parser_uuid;
  operation.registry_snapshot_uuid = view.catalog_epoch_uuid;
  operation.requires_security_context = true;
  operation.requires_transaction_context = true;
  operation.requires_cluster_authority = false;
  operation.parser_resolved_names_to_uuids = true;

  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = profile.operand_type;
  operand.name = "type";
  operand.value_kind = profile.value_kind;
  if (profile.opcode_code == 1569) {
    sblr::SblrDdlCreateTypeDescriptorV1 descriptor;
    descriptor.body[0] = 1;
    descriptor.availability = 1;
    operand.value_body =
        sblr::EncodeSblrDdlCreateTypeDescriptorV1(descriptor, true);
  } else if (profile.opcode_code == 1570) {
    sblr::SblrDdlAlterTypeDescriptorV1 descriptor;
    descriptor.body[0] = 1;
    descriptor.availability = 1;
    operand.value_body =
        sblr::EncodeSblrDdlAlterTypeDescriptorV1(descriptor, true);
  } else {
    sblr::SblrDdlDropTypeDescriptorV1 descriptor;
    descriptor.body[0] = 1;
    descriptor.availability = 1;
    operand.value_body =
        sblr::EncodeSblrDdlDropTypeDescriptorV1(descriptor, true);
  }
  Require(!operand.value_body.empty(), "TYPE DDL descriptor encoding failed");
  operation.operands.push_back(std::move(operand));
  const auto encoded_operation = sblr::EncodeSblrEnvelope(operation);
  Require(!encoded_operation.empty(), "canonical TYPE DDL SBOP encoding failed");
  submission.stream.assign(encoded_operation.begin(), encoded_operation.end());

  auto container = wire::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(submission.container.data()),
      submission.container.size());
  Require(container.status == wire::SblrCodecStatus::ok,
          "base TYPE DDL container decoding failed");
  container.container.canonical_anchor[100] = 2;
  container.container.canonical_anchor[101] = 0;
  container.container.operation_payload = submission.stream;
  const auto outer = wire::EncodeSblrContainer(container.container);
  Require(!outer.empty(), "TYPE DDL container encoding failed");

  auto ingress = wire::DecodeSblrExecutionEnvelopeV1Bytes(
      reinterpret_cast<const std::uint8_t*>(submission.ingress.data()),
      submission.ingress.size());
  Require(ingress.status == wire::SblrCodecStatus::ok,
          "base TYPE DDL execution envelope decoding failed");
  auto& fields = ingress.envelope.fields;
  fields[4] = V16(2);
  fields[5] = {0};
  fields[6] = {1};
  U64(&fields[6], submission.stream.size());
  fields[6].insert(fields[6].end(), submission.stream.begin(),
                   submission.stream.end());
  fields[7] = {1};
  U32(&fields[7], wire::SblrCrc32c(submission.stream.data(),
                                   submission.stream.size()));
  fields[8] = V64(submission.stream.size());
  const auto execution = wire::EncodeSblrExecutionEnvelopeV1(ingress.envelope);
  Require(!execution.empty(), "TYPE DDL execution envelope encoding failed");

  submission.container.assign(outer.begin(), outer.end());
  submission.ingress.assign(execution.begin(), execution.end());
  return submission;
}

std::array<std::uint8_t, 32> Digest(const void* bytes, std::size_t size) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      static_cast<const std::uint8_t*>(bytes), size);
  Require(digest.ok(), "canonical admission SHA-256 failed");
  return digest.digest;
}

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
    const server::ServerSblrAdmissionToken& token,
    bridge::StatementSblrPayloadKind payload_kind =
        bridge::StatementSblrPayloadKind::kOpcodeStream) {
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
  request.admitted_payload_kind = payload_kind;
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
  const api::SblrExecutorAvailabilityRowIdentity create_index_identity{
      api::kSblrDdlCreateIndexExecutorId,
      api::kSblrDdlCreateIndexOpcodeCode,
      api::kSblrDdlCreateIndexOpcodeVersion,
      api::kSblrDdlCreateIndexOperandDescriptorId,
      api::kSblrDdlCreateIndexResultDescriptorId,
      api::kSblrDdlCreateIndexResultDescriptorVersion};
  const auto create_index_bootstrap =
      api::LoadSblrExecutorAvailabilitySnapshot(context,
                                                create_index_identity);
  Require(create_index_bootstrap.ok &&
              create_index_bootstrap.snapshot.installed &&
              create_index_bootstrap.snapshot.generation != 0,
          "CREATE INDEX availability bootstrap failed");
  auto availability_admin_context = context;
  availability_admin_context.trace_tags.push_back(
      "right:SBLR_EXECUTOR_AVAILABILITY_ADMIN");
  api::SblrExecutorAvailabilitySetRequest advance_create_index;
  advance_create_index.database_uuid = context.database_uuid.canonical;
  advance_create_index.expected_snapshot_uuid =
      create_index_bootstrap.snapshot.snapshot_uuid;
  advance_create_index.expected_generation =
      create_index_bootstrap.snapshot.generation;
  advance_create_index.exact_row_identity = create_index_identity;
  advance_create_index.requested_state =
      api::SblrExecutorAvailabilityState::installed;
  advance_create_index.reason_code = "test.statement_context_projection";
  const auto advanced_create_index = api::SetSblrExecutorAvailability(
      availability_admin_context, advance_create_index);
  Require(advanced_create_index.ok &&
              advanced_create_index.snapshot.installed &&
              advanced_create_index.snapshot.generation ==
                  create_index_bootstrap.snapshot.generation + 1,
          "CREATE INDEX availability generation advance failed");
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
  const auto current_create_index =
      api::LoadCurrentSblrExecutorAvailabilitySnapshot(
          context, create_index_identity);
  Require(current_create_index.ok &&
              current_create_index.snapshot.generation ==
                  advanced_create_index.snapshot.generation &&
              view.ddl_create_index_executor_availability_generation ==
                  current_create_index.snapshot.generation &&
              view.ddl_create_index_executor_availability_generation !=
              view.ddl_create_table_executor_availability_generation,
          "statement context aliased CREATE INDEX availability generation");

  const std::array<TypeDdlProfile, 3> type_profiles{{
      {"engine.op.ddl_create_type", "SBLR_DDL_CREATE_TYPE", 1569,
       "create_type_descriptor", sblr::SblrValueKind::create_type_descriptor},
      {"engine.op.ddl_alter_type", "SBLR_DDL_ALTER_TYPE", 1570,
       "alter_type_descriptor", sblr::SblrValueKind::alter_type_descriptor},
      {"engine.op.ddl_drop_type", "SBLR_DDL_DROP_TYPE", 1571,
       "drop_type_descriptor", sblr::SblrValueKind::drop_type_descriptor},
  }};
  for (std::size_t index = 0; index != type_profiles.size(); ++index) {
    const auto type_parser_uuid =
        Text(NewUuid(platform::UuidKind::object, 610 + index));
    const auto type_submission = BuildTypeDdlSubmission(
        fixture, view, type_parser_uuid, type_profiles[index]);
    server::ServerSblrAdmissionRequest type_admission;
    type_admission.encoded_sblr_container = type_submission.container;
    type_admission.encoded_execution_envelope = type_submission.ingress;
    type_admission.admitted_parser_package_uuid = type_parser_uuid;
    type_admission.admitted_parser_package_version_major = 1;
    type_admission.admitted_registry_snapshot_uuid = view.catalog_epoch_uuid;
    type_admission.authenticated_principal_uuid = Text(fixture.principal_uuid);
    type_admission.catalog_snapshot_uuid =
        view.statement_metadata_snapshot_uuid;
    type_admission.engine_mga_statement_uuid = view.statement_uuid;
    type_admission.engine_mga_snapshot_uuid = view.statement_snapshot_uuid;
    type_admission.catalog_epoch = view.catalog_generation_id;
    type_admission.security_epoch = view.security_epoch;
    type_admission.resource_epoch = view.resource_epoch;
    type_admission.route_snapshot_uuid = view.optimizer_route_snapshot_uuid;
    type_admission.route_epoch = view.optimizer_route_epoch;
    type_admission.route_generation = view.optimizer_route_generation;
    type_admission.security_snapshot_uuid = view.security_context_uuid;
    type_admission.security_observation_generation = view.security_epoch;
    type_admission.route_snapshot_engine_owned = true;
    type_admission.security_snapshot_engine_owned = true;
    const auto type_admitted =
        server::AdmitServerSblrEnvelope(type_admission);
    Require(type_admitted.admitted && type_admitted.admission_token,
            "canonical standalone TYPE DDL admission failed");

    const auto type_dispatch = DispatchRequest(
        receipt, session.session, {}, type_admitted.admission_token,
        bridge::StatementSblrPayloadKind::kOperationEnvelope);
    sb_engine_result_t type_result = nullptr;
    const auto type_status = bridge::DispatchStatementContextReceipt(
        &type_dispatch, &type_result);
    Require(type_status == SB_ENGINE_STATUS_UNSUPPORTED &&
                type_result != nullptr,
            "standalone TYPE DDL did not reach shared evidence gate");
    sb_engine_diagnostic_set_view_t type_diagnostics{};
    Require(sb_engine_result_diagnostics(type_result, &type_diagnostics) ==
                SB_ENGINE_STATUS_OK &&
                type_diagnostics.diagnostic_count == 1 &&
                std::string_view(
                    type_diagnostics.diagnostics[0].symbolic_code.data,
                    type_diagnostics.diagnostics[0]
                        .symbolic_code.size_bytes) ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
            "standalone TYPE DDL diagnostic drifted");
    sb_engine_command_completion_view_v1_t type_completion{};
    const auto type_completion_status =
        sb_engine_result_completion(type_result, &type_completion);
    Require(type_completion_status != SB_ENGINE_STATUS_OK ||
                (type_completion.operation_id.size_bytes == 0 &&
                 type_completion.affected_rows == 0),
            "standalone TYPE DDL published command completion");
    bridge::StatementQueryExecuteResultHandleView type_handle;
    Require(bridge::ReadStatementQueryExecuteResultHandle(
                type_result, &type_handle) != SB_ENGINE_STATUS_OK,
            "standalone TYPE DDL published a result handle");
    Require(cancellation_probes.load(std::memory_order_relaxed) == 0,
            "standalone TYPE DDL consulted cancellation before evidence");
    (void)sb_engine_result_release(type_result);
  }

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
          "canonical CREATE INDEX reservation acquisition failed");
  if (reservation_result) (void)sb_engine_result_release(reservation_result);
  Require(create_index_reservation_view.record_count == 3,
          "canonical CREATE INDEX reservation record count drifted");

  bridge::StatementContextDispatchRequest create_index_dispatch;
  create_index_dispatch.receipt = receipt;
  create_index_dispatch.engine_session = session.session;
  create_index_dispatch.canonical_container_bytes.assign(
      create_index_submission.container.begin(),
      create_index_submission.container.end());
  create_index_dispatch.canonical_execution_envelope_bytes.assign(
      create_index_submission.ingress.begin(),
      create_index_submission.ingress.end());
  create_index_dispatch.canonical_operation_bytes =
      create_index_submission.stream;
  create_index_dispatch.container_sha256 = Digest(
      create_index_submission.container.data(),
      create_index_submission.container.size());
  create_index_dispatch.execution_envelope_sha256 = Digest(
      create_index_submission.ingress.data(),
      create_index_submission.ingress.size());
  create_index_dispatch.operation_sha256 = Digest(
      create_index_submission.stream.data(),
      create_index_submission.stream.size());
  create_index_dispatch.authenticated_principal_uuid =
      Text(fixture.principal_uuid);
  create_index_dispatch.catalog_snapshot_uuid =
      view.statement_metadata_snapshot_uuid;
  create_index_dispatch.engine_mga_statement_uuid = view.statement_uuid;
  create_index_dispatch.engine_mga_snapshot_uuid =
      view.statement_snapshot_uuid;
  create_index_dispatch.catalog_epoch = view.catalog_generation_id;
  create_index_dispatch.security_epoch = view.security_epoch;
  create_index_dispatch.resource_epoch = view.resource_epoch;
  create_index_dispatch.package_admission_reservation =
      create_index_reservation_handle;
  create_index_dispatch.admitted_payload_kind =
      bridge::StatementSblrPayloadKind::kOpcodeStream;
  create_index_dispatch.gateway_evidence.source =
      bridge::StatementGatewayEvidenceSource::kLocalObserved;
  create_index_dispatch.gateway_evidence.disposition =
      bridge::StatementGatewayDisposition::kPassThrough;
  create_index_dispatch.gateway_evidence.provider_observation_generation = 1;
  create_index_dispatch.gateway_evidence.canonical_payload_sha256 =
      create_index_dispatch.operation_sha256;
  create_index_dispatch.gateway_evidence.route_snapshot_uuid =
      view.optimizer_route_snapshot_uuid;
  create_index_dispatch.gateway_evidence.route_epoch =
      view.optimizer_route_epoch;
  create_index_dispatch.gateway_evidence.route_generation =
      view.optimizer_route_generation;
  create_index_dispatch.gateway_evidence.security_snapshot_uuid =
      view.security_context_uuid;
  create_index_dispatch.gateway_evidence.security_epoch = view.security_epoch;
  create_index_dispatch.gateway_evidence.security_observation_generation =
      view.security_epoch;
  create_index_dispatch.gateway_evidence.cluster_context_active =
      view.cluster_context_active;
  create_index_dispatch.gateway_evidence.cluster_transaction_active =
      view.cluster_transaction_active;
  create_index_dispatch.gateway_evidence.route_fence_present =
      view.route_fence_present;
  create_index_dispatch.package_executor_evidence.begin_executor_id =
      "engine.op.package_begin";
  create_index_dispatch.package_executor_evidence.end_executor_id =
      "engine.op.package_end";
  create_index_dispatch.package_executor_evidence.registry_snapshot_uuid =
      view.catalog_epoch_uuid;
  create_index_dispatch.package_executor_evidence
      .executor_evidence_generation = 1;
  create_index_dispatch.package_executor_evidence.canonical_payload_sha256 =
      create_index_dispatch.operation_sha256;
  create_index_dispatch.admission_binding_sha256 =
      Binding(create_index_dispatch);

  for (unsigned attempt = 0; attempt != 2; ++attempt) {
    sb_engine_result_t create_index_result = nullptr;
    const auto create_index_status =
        bridge::DispatchStatementContextReceipt(&create_index_dispatch,
                                                &create_index_result);
    Require(create_index_status == SB_ENGINE_STATUS_UNSUPPORTED &&
                create_index_result != nullptr,
            "canonical CREATE INDEX did not refuse static evidence");
    sb_engine_diagnostic_set_view_t create_index_diagnostics{};
    Require(sb_engine_result_diagnostics(create_index_result,
                                         &create_index_diagnostics) ==
                SB_ENGINE_STATUS_OK &&
                create_index_diagnostics.diagnostic_count == 1 &&
                std::string_view(
                    create_index_diagnostics.diagnostics[0]
                        .symbolic_code.data,
                    create_index_diagnostics.diagnostics[0]
                        .symbolic_code.size_bytes) ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
            "canonical CREATE INDEX diagnostic drifted");
    sb_engine_command_completion_view_v1_t create_index_completion{};
    const auto create_index_completion_status = sb_engine_result_completion(
        create_index_result, &create_index_completion);
    Require(create_index_completion_status != SB_ENGINE_STATUS_OK ||
                (create_index_completion.operation_id.size_bytes == 0 &&
                 create_index_completion.affected_rows == 0),
            "canonical CREATE INDEX published command completion");
    bridge::StatementQueryExecuteResultHandleView create_index_handle;
    Require(bridge::ReadStatementQueryExecuteResultHandle(
                create_index_result, &create_index_handle) !=
                SB_ENGINE_STATUS_OK,
            "canonical CREATE INDEX published a query result handle");
    Require(cancellation_probes.load(std::memory_order_relaxed) == 0,
            "canonical CREATE INDEX consulted cancellation before evidence");
    (void)sb_engine_result_release(create_index_result);
  }
  auto mislabeled_create_index = create_index_dispatch;
  mislabeled_create_index.admitted_payload_kind =
      bridge::StatementSblrPayloadKind::kOperationEnvelope;
  mislabeled_create_index.admission_binding_sha256 =
      Binding(mislabeled_create_index);
  sb_engine_result_t mislabeled_result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(
              &mislabeled_create_index, &mislabeled_result) ==
              SB_ENGINE_STATUS_SECURITY_DENIED &&
              mislabeled_result != nullptr,
          "physical CREATE INDEX SBOS bypassed payload classification");
  sb_engine_diagnostic_set_view_t mislabeled_diagnostics{};
  Require(sb_engine_result_diagnostics(mislabeled_result,
                                       &mislabeled_diagnostics) ==
              SB_ENGINE_STATUS_OK &&
              mislabeled_diagnostics.diagnostic_count == 1 &&
              std::string_view(
                  mislabeled_diagnostics.diagnostics[0].symbolic_code.data,
                  mislabeled_diagnostics.diagnostics[0]
                      .symbolic_code.size_bytes) ==
                  "SBLR.INGRESS_REVALIDATION_FAILED" &&
              cancellation_probes.load(std::memory_order_relaxed) == 0,
          "mislabeled CREATE INDEX payload diagnostic drifted");
  (void)sb_engine_result_release(mislabeled_result);
  Require(bridge::ReleaseStatementPackageAdmissionReservation(
              create_index_reservation_handle,
              bridge::StatementPackageReservationReleaseReason::kRelease) ==
              SB_ENGINE_STATUS_OK &&
              bridge::ReleaseStatementPackageAdmissionReservation(
                  create_index_reservation_handle,
                  bridge::StatementPackageReservationReleaseReason::
                      kRelease) == SB_ENGINE_STATUS_ALREADY_RELEASED,
          "CREATE INDEX evidence refusal consumed its reservation");

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
