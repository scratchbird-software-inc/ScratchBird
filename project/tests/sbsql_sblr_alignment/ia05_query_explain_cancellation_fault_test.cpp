// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission fixture.  No public ABI,
// package gateway, executor registry, or query binding service is mocked.
#define SCRATCHBIRD_IA01_MISSING_EXECUTOR_FIXTURE_ONLY
#include "ia01_package_missing_executor_integration_test.cpp"

#include "engine/sblr/sblr_query_explain_runtime.hpp"
#include "ia01_literal_live_fixture.hpp"
#include "wire/parser_server_ipc/sbps_statement_management_bind_codec.hpp"

#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

namespace bind = scratchbird::wire::sbps_statement_management;

std::string UuidText(const std::array<std::uint8_t, 16>& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text;
  text.reserve(36);
  for (std::size_t index = 0; index != value.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      text.push_back('-');
    }
    text.push_back(kHex[value[index] >> 4U]);
    text.push_back(kHex[value[index] & 0x0fU]);
  }
  return text;
}

sblr::SblrOperand TypedQueryOperand(std::uint32_t ordinal, std::string type,
                                    std::string name,
                                    std::string_view value) {
  sblr::SblrOperand operand;
  operand.ordinal = ordinal;
  operand.type = std::move(type);
  operand.name = std::move(name);
  operand.value_kind = sblr::SblrValueKind::literal_typed;
  const auto carrier_type_uuid =
      RawUuid("019d0000-0000-7000-8000-00000000d712");
  operand.value_body.assign(carrier_type_uuid.begin(),
                            carrier_type_uuid.end());
  U64(&operand.value_body, value.size());
  operand.value_body.insert(operand.value_body.end(), value.begin(),
                            value.end());
  return operand;
}

sblr::SblrOperationEnvelope SourceFreeValuesQueryMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const literal_fixture::Binding& literal_binding) {
  const auto descriptor_uuid = UuidText(literal_binding.descriptor_uuid);
  const std::string descriptor_record =
      descriptor_uuid + "|019d0000-0000-7000-8000-00000000d712|" +
      std::to_string(literal_binding.descriptor_generation) +
      "|-|-|-|-|-";

  auto member = sblr::MakeSblrEnvelope(
      "query.execute", "SBLR_QUERY_EXECUTE",
      "ia05.query_explain.source_values");
  member.opcode_code = 4615;
  member.result_shape = "query_execute_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;

  std::uint32_t ordinal = 1;
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uint16", "relational_wire_version", "2"));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uuid", "relational_bound_sblr_tree_uuid",
      view.bound_ast_uuid));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uuid", "relational_catalog_epoch_uuid",
      view.catalog_epoch_uuid));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uuid", "relational_security_context_uuid",
      view.security_context_uuid));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uuid", "relational_statement_uuid", view.statement_uuid));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uuid", "relational_owning_transaction_uuid",
      view.owning_transaction_uuid));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uuid", "relational_statement_snapshot_uuid",
      view.statement_snapshot_uuid));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uuid", "relational_statement_metadata_snapshot_uuid",
      view.statement_metadata_snapshot_uuid));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uint64", "relational_local_transaction_id",
      std::to_string(view.owning_local_transaction_id)));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uint64",
      "relational_snapshot_visible_through_local_transaction_id",
      std::to_string(view.visible_committed_high_watermark)));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "uint32", "relational_root_node_id", "1"));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "relational_descriptor_v1", "slot_1",
      descriptor_record));

  const auto table_sha = scratchbird::core::hash::ComputeSha256Digest(
      literal_binding.sbxn);
  Require(table_sha.ok(), "003608 literal table hash failed");
  sblr::SblrOperand reference;
  reference.ordinal = ordinal++;
  reference.type = "relational_expression_v1";
  reference.name = "1";
  reference.value_kind = sblr::SblrValueKind::expression_node_ref;
  U16(&reference.value_body, 1);
  U16(&reference.value_body, 0);
  U32(&reference.value_body, 1);
  U64(&reference.value_body, 7);
  reference.value_body.insert(reference.value_body.end(),
                              table_sha.digest.begin(),
                              table_sha.digest.end());
  reference.value_body.insert(reference.value_body.end(),
                              literal_binding.descriptor_uuid.begin(),
                              literal_binding.descriptor_uuid.end());
  U64(&reference.value_body, literal_binding.descriptor_generation);
  member.operands.push_back(std::move(reference));

  member.operands.push_back(TypedQueryOperand(
      ordinal++, "relational_output_v1", "slot_1",
      "1|1|1|1|0|76616c7565"));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "relational_values_row_v1", "slot_1", "1"));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "relational_node_v1", "slot_1", "13|0|-|1|1"));
  member.operands.push_back(TypedQueryOperand(
      ordinal++, "relational_node_binding_v1", "slot_1",
      "76616c7565732e6c69746572616c2d7461626c652e7631|1|-|-|-"));

  sblr::SblrOperand table;
  table.ordinal = ordinal++;
  table.type = "expression.node_table.v1";
  table.name = "expression_nodes";
  table.value_kind = sblr::SblrValueKind::expression_node_table;
  table.value_body = literal_binding.sbxn;
  member.operands.push_back(std::move(table));
  Require(ordinal == 19, "003608 source VALUES operand count drifted");
  return member;
}

Submission PackageWithMember(
    const Fixture& fixture,
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid, sblr::SblrOperationEnvelope member) {
  auto submission = BuildSubmission(fixture, view, parser_uuid);
  const auto validation = sblr::ValidateSblrEnvelope(member);
  if (!validation.ok) {
    for (const auto& diagnostic : validation.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
    }
  }
  Require(validation.ok, "003608 query-explain member validation failed");
  const auto package_uuid = RawUuid(view.bound_ast_uuid);
  sblr::SblrOpcodeStream stream;
  stream.package_descriptor_uuid = view.bound_ast_uuid;
  stream.registry_snapshot_uuid = view.catalog_epoch_uuid;
  stream.operations = {
      Frame(true, parser_uuid, view.catalog_epoch_uuid, package_uuid),
      std::move(member),
      Frame(false, parser_uuid, view.catalog_epoch_uuid, package_uuid)};
  submission.stream = sblr::EncodeSblrOpcodeStream(stream);
  Require(!submission.stream.empty(),
          "003608 canonical query-explain SBOS encoding failed");

  auto container = wire::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(submission.container.data()),
      submission.container.size());
  Require(container.status == wire::SblrCodecStatus::ok,
          "003608 base container decoding failed");
  container.container.operation_payload = submission.stream;
  const auto encoded_container = wire::EncodeSblrContainer(container.container);
  Require(!encoded_container.empty(),
          "003608 query-explain container encoding failed");

  auto ingress = wire::DecodeSblrExecutionEnvelopeV1Bytes(
      reinterpret_cast<const std::uint8_t*>(submission.ingress.data()),
      submission.ingress.size());
  Require(ingress.status == wire::SblrCodecStatus::ok,
          "003608 base execution-envelope decoding failed");
  auto& fields = ingress.envelope.fields;
  fields[5] = {1};
  U64(&fields[5], submission.stream.size());
  fields[5].insert(fields[5].end(), submission.stream.begin(),
                   submission.stream.end());
  fields[7] = {1};
  U32(&fields[7],
      wire::SblrCrc32c(submission.stream.data(), submission.stream.size()));
  fields[8] = V64(submission.stream.size());
  const auto encoded_ingress =
      wire::EncodeSblrExecutionEnvelopeV1(ingress.envelope);
  Require(!encoded_ingress.empty(),
          "003608 query-explain execution-envelope encoding failed");
  submission.container.assign(encoded_container.begin(),
                              encoded_container.end());
  submission.ingress.assign(encoded_ingress.begin(), encoded_ingress.end());
  return submission;
}

sblr::SblrOperationEnvelope QueryExplainMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const std::vector<std::uint8_t>& descriptor_bytes) {
  auto member = sblr::MakeSblrEnvelope(
      "engine.op.query_explain", "SBLR_QUERY_EXPLAIN",
      "ia05.query_explain.cancellation_atomicity");
  member.opcode_code = 4616;
  member.result_shape = "explain_result";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "query_explain_descriptor.v1";
  operand.name = "query";
  operand.value_kind = sblr::SblrValueKind::query_explain_descriptor;
  operand.value_body = descriptor_bytes;
  member.operands.push_back(std::move(operand));
  return member;
}

bridge::StatementContextDispatchRequest Admit(
    const Fixture& fixture, PublicSession& session,
    const bridge::StatementContextReceiptView& view,
    bridge::StatementContextReceiptHandle receipt,
    std::string_view parser_uuid, const Submission& submission,
    bridge::StatementPackageAdmissionReservationHandle* reservation) {
  bridge::StatementPackageAdmissionReservationRequest request;
  request.receipt = receipt;
  request.canonical_payload_bytes = submission.stream.data();
  request.canonical_payload_size = submission.stream.size();
  request.payload_kind = bridge::StatementSblrPayloadKind::kOpcodeStream;
  bridge::StatementPackageAdmissionReservationView reservation_view;
  sb_engine_result_t result = nullptr;
  Require(bridge::AcquireStatementPackageAdmissionReservation(
              &request, reservation, &reservation_view, &result) ==
              SB_ENGINE_STATUS_OK,
          "003608 package reservation acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);

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
  admission.package_reservation_handle = reservation->opaque_id;
  admission.reserved_payload_kind = server::ServerSblrPayloadKind::opcode_stream;
  admission.reserved_payload_size = reservation_view.payload_size;
  admission.reserved_record_count = reservation_view.record_count;
  admission.reserved_resource_policy_generation =
      reservation_view.resource_policy_generation;
  admission.reserved_payload_sha256 = reservation_view.payload_sha256;
  const auto admitted = server::AdmitServerSblrEnvelope(admission);
  if (!admitted.admitted) {
    for (const auto& diagnostic : admitted.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
  }
  Require(admitted.admitted && admitted.admission_token,
          "003608 canonical server admission failed");
  return DispatchRequest(receipt, session.session, *reservation,
                         admitted.admission_token);
}

std::string DiagnosticCode(sb_engine_result_t result) {
  sb_engine_diagnostic_set_view_t diagnostics{};
  Require(sb_engine_result_diagnostics(result, &diagnostics) ==
                  SB_ENGINE_STATUS_OK &&
              diagnostics.diagnostic_count == 1,
          "003608 exact cancellation diagnostic was absent");
  return {diagnostics.diagnostics[0].symbolic_code.data,
          diagnostics.diagnostics[0].symbolic_code.size_bytes};
}

std::string DiagnosticKey(sb_engine_result_t result) {
  sb_engine_diagnostic_set_view_t diagnostics{};
  Require(sb_engine_result_diagnostics(result, &diagnostics) ==
                  SB_ENGINE_STATUS_OK &&
              diagnostics.diagnostic_count == 1,
          "003608 exact cancellation diagnostic was absent");
  return {diagnostics.diagnostics[0].message_key.data,
          diagnostics.diagnostics[0].message_key.size_bytes};
}

std::vector<std::uint8_t> ResultPayload(sb_engine_result_t result) {
  sb_engine_string_view_t payload{};
  Require(sb_engine_result_payload(result, &payload) == SB_ENGINE_STATUS_OK &&
              payload.data != nullptr,
          "003608 canonical result payload was absent");
  return {reinterpret_cast<const std::uint8_t*>(payload.data),
          reinterpret_cast<const std::uint8_t*>(payload.data) +
              payload.size_bytes};
}

}  // namespace

#ifndef SCRATCHBIRD_IA05_QUERY_EXPLAIN_FIXTURE_ONLY
int main() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  std::atomic<unsigned> cancel_on_probe{0};
  auto context = BeginTransaction(fixture, &probes);
  const auto parser_uuid = Text(NewUuid(platform::UuidKind::object, 36008));
  context.current_package_uuid.canonical = parser_uuid;
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
          "003608 live statement receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);

  auto literal_binding = literal_fixture::FinalizeLiteral(receipt, view);
  const auto query_submission = PackageWithMember(
      fixture, view, parser_uuid,
      SourceFreeValuesQueryMember(view, parser_uuid, literal_binding));

  bind::QueryExplainBindRequestV1 public_bind;
  public_bind.authenticated_receipt_uuid = RawUuid(view.receipt_uuid);
  public_bind.occurrence = 1;
  public_bind.verbose = true;
  public_bind.format = 2;
  public_bind.canonical_container_bytes.assign(query_submission.container.begin(),
                                               query_submission.container.end());
  public_bind.canonical_execution_envelope_bytes.assign(
      query_submission.ingress.begin(), query_submission.ingress.end());
  std::vector<std::uint8_t> exact_bind;
  std::string detail;
  Require(bind::EncodeQueryExplainBindRequestV1(
              public_bind, &exact_bind, &detail),
          "003608 canonical private bind request encoding failed");
  bind::QueryExplainBindRequestV1 decoded_bind;
  Require(bind::DecodeQueryExplainBindRequestV1(
              exact_bind.data(), exact_bind.size(), &decoded_bind, &detail),
          "003608 canonical private bind request decoding failed");

  bridge::StatementQueryExplainBindRequestV1 bind_request;
  bind_request.authenticated_receipt_uuid = view.receipt_uuid;
  bind_request.occurrence = decoded_bind.occurrence;
  bind_request.verbose = decoded_bind.verbose;
  bind_request.format = decoded_bind.format;
  bind_request.canonical_container_bytes =
      decoded_bind.canonical_container_bytes;
  bind_request.canonical_execution_envelope_bytes =
      decoded_bind.canonical_execution_envelope_bytes;
  bind_request.request_evidence_sha256 = decoded_bind.request_evidence_sha256;
  bind_request.exact_bind_request_bytes = exact_bind;
  bridge::StatementQueryExplainBindAckV1 bind_ack;
  result = nullptr;
  const auto bind_status = bridge::BindStatementQueryExplainAuthorityV1(
      receipt, &bind_request, &bind_ack, &result);
  if (bind_status != SB_ENGINE_STATUS_OK && result != nullptr) {
    std::cerr << DiagnosticCode(result) << ':' << DiagnosticKey(result) << ':'
              << bind_ack.failure_detail << '\n';
  }
  Require(bind_status == SB_ENGINE_STATUS_OK,
          "003608 engine query-explain binding failed");
  if (result != nullptr) (void)sb_engine_result_release(result);

  bridge::StatementQueryExplainAuthorityV1 authority;
  Require(bridge::CopyStatementQueryExplainAuthorityV1(
              receipt, 1, &authority, &result) == SB_ENGINE_STATUS_OK &&
              !authority.canonical_descriptor_bytes.empty(),
          "003608 engine query-explain descriptor was not published");
  if (result != nullptr) (void)sb_engine_result_release(result);
  const auto explain_submission = PackageWithMember(
      fixture, view, parser_uuid,
      QueryExplainMember(view, parser_uuid,
                         authority.canonical_descriptor_bytes));

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle cancel_reservation;
  auto cancel_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               explain_submission, &cancel_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&cancel_dispatch, &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr,
          "003608 prepublication cancellation did not stop QUERY_EXPLAIN");
  const auto cancellation_code = DiagnosticCode(result);
  const auto cancellation_key = DiagnosticKey(result);
  const auto cancellation_probes = probes.load(std::memory_order_relaxed);
  if (cancellation_code != "PROCESS.CANCELLED" ||
      cancellation_key != "sblr.query_explain.cancelled_before_planning" ||
      cancellation_probes != 1) {
    std::cerr << "003608 cancellation observation: code="
              << cancellation_code << ";key=" << cancellation_key
              << ";probes=" << cancellation_probes << '\n';
  }
  Require(cancellation_code == "PROCESS.CANCELLED" &&
              cancellation_key ==
                  "sblr.query_explain.cancelled_before_planning" &&
              cancellation_probes == 1,
          "003608 preplanning cancellation boundary drifted");
  std::vector<std::uint8_t> cancelled_material;
  Require(bridge::ReadStatementQueryExplainMaterial(
              result, &cancelled_material) != SB_ENGINE_STATUS_OK &&
              cancelled_material.empty(),
          "003608 cancelled QUERY_EXPLAIN published plan material");
  (void)sb_engine_result_release(result);

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(4, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle publication_reservation;
  auto publication_dispatch = Admit(
      fixture, session, view, receipt, parser_uuid, explain_submission,
      &publication_reservation);
  result = nullptr;
  const auto publication_status =
      bridge::DispatchStatementContextReceipt(&publication_dispatch, &result);
  if (publication_status != SB_ENGINE_STATUS_TIMEOUT || result == nullptr) {
    std::cerr << "003608 prepublication observation: status="
              << sb_engine_status_name(publication_status)
              << ";probes=" << probes.load(std::memory_order_relaxed);
    if (result != nullptr) {
      std::cerr << ";code=" << DiagnosticCode(result)
                << ";key=" << DiagnosticKey(result);
    }
    std::cerr << '\n';
  }
  Require(publication_status == SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr,
          "003608 prepublication cancellation did not stop QUERY_EXPLAIN");
  Require(DiagnosticCode(result) == "PROCESS.CANCELLED" &&
              DiagnosticKey(result) ==
                  "sblr.query_explain.cancelled_before_publication" &&
              probes.load(std::memory_order_relaxed) == 4,
          "003608 prepublication cancellation boundary drifted");
  cancelled_material.clear();
  Require(bridge::ReadStatementQueryExplainMaterial(
              result, &cancelled_material) != SB_ENGINE_STATUS_OK &&
              cancelled_material.empty(),
          "003608 prepublication cancellation published plan material");
  (void)sb_engine_result_release(result);

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle success_reservation;
  auto success_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                explain_submission, &success_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&success_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003608 QUERY_EXPLAIN retry did not publish a plan");
  const auto first_result = ResultPayload(result);
  sblr::SblrQueryExplainResultV1 decoded_result;
  Require(first_result.size() == 256 &&
              sblr::DecodeSblrQueryExplainResultV1(
                  first_result.data(), first_result.size(), &decoded_result,
                  &detail),
          "003608 successful retry omitted canonical SBXR");
  std::vector<std::uint8_t> first_material;
  Require(bridge::ReadStatementQueryExplainMaterial(
              result, &first_material) == SB_ENGINE_STATUS_OK &&
              !first_material.empty(),
          "003608 successful retry omitted redacted plan material");
  (void)sb_engine_result_release(result);

  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle replay_reservation;
  auto replay_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               explain_submission, &replay_reservation);
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&replay_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003608 postpublication cancellation retracted exact replay");
  const auto replay_result = ResultPayload(result);
  std::vector<std::uint8_t> replay_material;
  Require(replay_result == first_result &&
              bridge::ReadStatementQueryExplainMaterial(
                  result, &replay_material) == SB_ENGINE_STATUS_OK &&
              replay_material == first_material &&
              probes.load(std::memory_order_relaxed) == 0,
          "003608 exact replay was not byte-identical and cancellation-stable");
  (void)sb_engine_result_release(result);

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "003608 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "003608 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
#endif
