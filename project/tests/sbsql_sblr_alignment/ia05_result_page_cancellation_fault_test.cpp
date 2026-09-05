// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Reuse the real database/session/receipt/admission fixture.  This test does
// not mock the public ABI, canonical package gateway, executor registry, or
// query result handle.
#define SCRATCHBIRD_IA01_MISSING_EXECUTOR_FIXTURE_ONLY
#include "ia01_package_missing_executor_integration_test.cpp"

#include "engine/sblr/sblr_result_page_runtime.hpp"
#include "ia01_literal_live_fixture.hpp"

#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

Submission PackageWithMember(
    const Fixture& fixture,
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    sblr::SblrOperationEnvelope member) {
  auto submission = BuildSubmission(fixture, view, parser_uuid);
  const auto validation = sblr::ValidateSblrEnvelope(member);
  if (!validation.ok) {
    std::cerr << "003600 member validation failed operation="
              << member.operation_id << '\n';
    for (const auto& diagnostic : validation.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.message
                << '\n';
    }
  }
  Require(validation.ok, "003600 package member validation failed");
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
          "003600 canonical opcode-stream encoding failed");

  auto container = wire::DecodeSblrContainerBytes(
      reinterpret_cast<const std::uint8_t*>(submission.container.data()),
      submission.container.size());
  Require(container.status == wire::SblrCodecStatus::ok,
          "003600 base container decoding failed");
  container.container.canonical_anchor[100] = 1;
  container.container.canonical_anchor[101] = 0;
  container.container.operation_payload = submission.stream;
  const auto encoded_container =
      wire::EncodeSblrContainer(container.container);
  Require(!encoded_container.empty(),
          "003600 canonical container encoding failed");

  auto ingress = wire::DecodeSblrExecutionEnvelopeV1Bytes(
      reinterpret_cast<const std::uint8_t*>(submission.ingress.data()),
      submission.ingress.size());
  Require(ingress.status == wire::SblrCodecStatus::ok,
          "003600 base execution-envelope decoding failed");
  auto& fields = ingress.envelope.fields;
  fields[4] = V16(1);
  fields[5] = {1};
  U64(&fields[5], submission.stream.size());
  fields[5].insert(fields[5].end(), submission.stream.begin(),
                   submission.stream.end());
  fields[6] = {0};
  fields[7] = {1};
  U32(&fields[7],
      wire::SblrCrc32c(submission.stream.data(), submission.stream.size()));
  fields[8] = V64(submission.stream.size());
  const auto encoded_ingress =
      wire::EncodeSblrExecutionEnvelopeV1(ingress.envelope);
  Require(!encoded_ingress.empty(),
          "003600 canonical execution-envelope encoding failed");
  submission.container.assign(encoded_container.begin(),
                              encoded_container.end());
  submission.ingress.assign(encoded_ingress.begin(), encoded_ingress.end());
  return submission;
}

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

sblr::SblrOperationEnvelope ValuesQueryMember(
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
      "ia05.result_page.source_values");
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
  Require(table_sha.ok(), "003600 literal table hash failed");
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
  Require(ordinal == 19, "003600 source VALUES operand count drifted");
  return member;
}

sblr::SblrOperationEnvelope ResultPageMember(
    const bridge::StatementContextReceiptView& view,
    std::string_view parser_uuid,
    const bridge::StatementQueryExecuteResultHandleView& source_handle,
    const std::array<std::uint8_t, 16>& cursor_uuid) {
  sblr::SblrResultPageDescriptorV1 descriptor;
  descriptor.cursor_uuid = cursor_uuid;
  descriptor.cursor_generation = 1;
  descriptor.statement_receipt_uuid = RawUuid(view.receipt_uuid);
  descriptor.result_set_handle_uuid = RawUuid(source_handle.result_set_uuid);
  descriptor.result_set_handle_generation = 1;
  descriptor.snapshot_uuid = RawUuid(source_handle.snapshot_uuid);
  descriptor.row_descriptor_uuid = RawUuid(source_handle.row_descriptor_uuid);
  descriptor.row_descriptor_generation = 1;
  descriptor.maximum_rows = 1;
  descriptor.maximum_bytes = 1U << 20U;
  descriptor.redaction_profile_uuid =
      RawUuid(view.result_page_redaction_profile_uuid);
  descriptor.redaction_generation = view.result_page_redaction_generation;
  descriptor.policy_snapshot_uuid =
      RawUuid(view.result_page_policy_snapshot_uuid);
  descriptor.policy_generation = view.result_page_policy_generation;
  descriptor.resource_budget_uuid =
      RawUuid(view.result_page_resource_budget_uuid);
  descriptor.resource_budget_generation =
      view.result_page_resource_budget_generation;
  descriptor.executor_availability_generation =
      view.result_page_executor_availability_generation;
  const auto descriptor_bytes =
      sblr::EncodeSblrResultPageDescriptorV1(descriptor);
  Require(descriptor_bytes.size() == 288,
          "003600 canonical result-page descriptor encoding failed");

  auto member = sblr::MakeSblrEnvelope(
      "engine.op.result_page", "SBLR_RESULT_PAGE",
      "ia05.result_page.cancellation_atomicity");
  member.opcode_code = 4614;
  member.result_shape = "result_page_data";
  member.diagnostic_shape = "diagnostic_vector";
  member.parser_package_uuid = parser_uuid;
  member.registry_snapshot_uuid = view.catalog_epoch_uuid;
  member.requires_security_context = true;
  member.requires_transaction_context = true;
  member.parser_resolved_names_to_uuids = true;
  sblr::SblrOperand operand;
  operand.ordinal = 1;
  operand.type = "result_page_descriptor.v1";
  operand.name = "result_page";
  operand.value_kind = sblr::SblrValueKind::result_page_descriptor;
  operand.value_body = descriptor_bytes;
  member.operands.push_back(std::move(operand));
  return member;
}

bridge::StatementContextDispatchRequest Admit(
    const Fixture& fixture,
    PublicSession& session,
    const bridge::StatementContextReceiptView& view,
    bridge::StatementContextReceiptHandle receipt,
    std::string_view parser_uuid,
    const Submission& submission,
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
          "003600 package reservation acquisition failed");
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
  admission.reserved_payload_kind =
      server::ServerSblrPayloadKind::opcode_stream;
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
          "003600 canonical server admission failed");
  return DispatchRequest(receipt, session.session, *reservation,
                         admitted.admission_token);
}

std::string DiagnosticCode(sb_engine_result_t result) {
  sb_engine_diagnostic_set_view_t diagnostics{};
  Require(sb_engine_result_diagnostics(result, &diagnostics) ==
                  SB_ENGINE_STATUS_OK &&
              diagnostics.diagnostic_count == 1,
          "003600 exact cancellation diagnostic was absent");
  return {diagnostics.diagnostics[0].symbolic_code.data,
          diagnostics.diagnostics[0].symbolic_code.size_bytes};
}

std::string DiagnosticKey(sb_engine_result_t result) {
  sb_engine_diagnostic_set_view_t diagnostics{};
  Require(sb_engine_result_diagnostics(result, &diagnostics) ==
                  SB_ENGINE_STATUS_OK &&
              diagnostics.diagnostic_count == 1,
          "003600 exact cancellation diagnostic was absent");
  return {diagnostics.diagnostics[0].message_key.data,
          diagnostics.diagnostics[0].message_key.size_bytes};
}

std::vector<std::uint8_t> ResultPayload(sb_engine_result_t result) {
  sb_engine_string_view_t payload{};
  Require(sb_engine_result_payload(result, &payload) == SB_ENGINE_STATUS_OK &&
              payload.data != nullptr,
          "003600 canonical result payload was absent");
  return {reinterpret_cast<const std::uint8_t*>(payload.data),
          reinterpret_cast<const std::uint8_t*>(payload.data) +
              payload.size_bytes};
}

}  // namespace

int main() {
  auto fixture = CreateFixture();
  PublicSession session(fixture);
  std::atomic<unsigned> probes{0};
  std::atomic<unsigned> cancel_on_probe{0};
  auto context = BeginTransaction(fixture, &probes);
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
          "003600 live statement receipt acquisition failed");
  if (result != nullptr) (void)sb_engine_result_release(result);

  const auto parser_uuid =
      Text(NewUuid(platform::UuidKind::object, 36003));
  auto literal_binding = literal_fixture::FinalizeLiteral(receipt, view);
  const auto query_submission = PackageWithMember(
      fixture, view, parser_uuid,
      ValuesQueryMember(view, parser_uuid, literal_binding));
  const auto query_stream_sha =
      scratchbird::core::hash::ComputeSha256Digest(query_submission.stream);
  Require(query_stream_sha.ok() && literal_binding.sbel.size() == 176,
          "003600 source query binding evidence is malformed");
  std::copy(query_stream_sha.digest.begin(), query_stream_sha.digest.end(),
            literal_binding.sbel.begin() + 144);
  bridge::StatementPackageAdmissionReservationHandle query_reservation;
  auto query_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                              query_submission, &query_reservation);
  query_dispatch.literal_execution_binding = literal_binding.sbel;
  result = nullptr;
  const auto query_status =
      bridge::DispatchStatementContextReceipt(&query_dispatch, &result);
  if (query_status != SB_ENGINE_STATUS_OK && result != nullptr) {
    sb_engine_diagnostic_set_view_t diagnostics{};
    if (sb_engine_result_diagnostics(result, &diagnostics) ==
        SB_ENGINE_STATUS_OK) {
      for (std::uint64_t index = 0; index != diagnostics.diagnostic_count;
           ++index) {
        std::cerr.write(diagnostics.diagnostics[index].symbolic_code.data,
                        diagnostics.diagnostics[index]
                            .symbolic_code.size_bytes);
        std::cerr << ':';
        std::cerr.write(diagnostics.diagnostics[index].message_key.data,
                        diagnostics.diagnostics[index]
                            .message_key.size_bytes);
        std::cerr << ':';
        std::cerr.write(diagnostics.diagnostics[index].safe_detail.data,
                        diagnostics.diagnostics[index]
                            .safe_detail.size_bytes);
        std::cerr << '\n';
      }
    }
  }
  Require(query_status == SB_ENGINE_STATUS_OK && result != nullptr,
          "003600 source VALUES query did not execute");
  sb_engine_result_t source_result = result;
  bridge::StatementQueryExecuteResultHandleView source_handle;
  Require(bridge::ReadStatementQueryExecuteResultHandle(
              source_result, &source_handle) == SB_ENGINE_STATUS_OK,
          "003600 source query did not publish an engine result handle");

  const auto cursor_uuid = RawUuid(
      Text(NewUuid(platform::UuidKind::object, 36004)));
  const auto page_submission = PackageWithMember(
      fixture, view, parser_uuid,
      ResultPageMember(view, parser_uuid, source_handle, cursor_uuid));

  // RESULT_PAGE validates the complete canonical request before observing
  // cancellation.  Its sole prepublication checkpoint is immediately before
  // source-row access or replay publication.
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle cancel_reservation;
  auto cancel_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               page_submission, &cancel_reservation);
  cancel_dispatch.result_page_source_result = source_result;
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&cancel_dispatch, &result) ==
                  SB_ENGINE_STATUS_TIMEOUT &&
              result != nullptr,
          "003600 prepublication cancellation did not stop RESULT_PAGE");
  const auto cancelled_code = DiagnosticCode(result);
  const auto cancelled_key = DiagnosticKey(result);
  const auto cancellation_probe_count =
      probes.load(std::memory_order_relaxed);
  if (cancelled_code != "PROCESS.CANCELLED" ||
      cancelled_key != "sblr.result_page.cancelled_before_source_access" ||
      cancellation_probe_count != 1) {
    std::cerr << "003600 cancellation=" << cancelled_code << ':'
              << cancelled_key << ":probes=" << cancellation_probe_count
              << '\n';
  }
  Require(cancelled_code == "PROCESS.CANCELLED" &&
              cancelled_key ==
                  "sblr.result_page.cancelled_before_source_access" &&
              cancellation_probe_count == 1,
          "003600 prepublication cancellation boundary drifted");
  std::vector<std::uint8_t> cancelled_material;
  Require(bridge::ReadStatementResultPageMaterial(
              result, &cancelled_material) != SB_ENGINE_STATUS_OK &&
              cancelled_material.empty(),
          "003600 cancelled RESULT_PAGE published row material");
  (void)sb_engine_result_release(result);

  // Retrying with a fresh one-shot admission token must return the first row.
  // That result proves the cancelled attempt did not advance the source.
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(0, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle success_reservation;
  auto success_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                                page_submission, &success_reservation);
  success_dispatch.result_page_source_result = source_result;
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&success_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003600 RESULT_PAGE retry did not publish the first page");
  const auto first_result = ResultPayload(result);
  sblr::SblrResultPageResultV1 decoded_page;
  std::string detail;
  Require(first_result.size() == 256 &&
              sblr::DecodeSblrResultPageResultV1(
                  first_result.data(), first_result.size(), &decoded_page,
                  &detail) &&
              decoded_page.returned_row_count == 1 &&
              decoded_page.next_row_offset == 1 &&
              decoded_page.terminal_state == 1,
          "003600 successful retry did not return the unconsumed source row");
  std::vector<std::uint8_t> first_material;
  Require(bridge::ReadStatementResultPageMaterial(result, &first_material) ==
                  SB_ENGINE_STATUS_OK &&
              !first_material.empty(),
          "003600 successful RESULT_PAGE omitted canonical row material");
  (void)sb_engine_result_release(result);

  // Once the result and barrier exist, exact descriptor replay must win over
  // a newly observed cancellation and return the byte-identical publication.
  probes.store(0, std::memory_order_relaxed);
  cancel_on_probe.store(1, std::memory_order_relaxed);
  bridge::StatementPackageAdmissionReservationHandle replay_reservation;
  auto replay_dispatch = Admit(fixture, session, view, receipt, parser_uuid,
                               page_submission, &replay_reservation);
  replay_dispatch.result_page_source_result = source_result;
  result = nullptr;
  Require(bridge::DispatchStatementContextReceipt(&replay_dispatch, &result) ==
                  SB_ENGINE_STATUS_OK &&
              result != nullptr,
          "003600 postpublication cancellation retracted exact replay");
  const auto replay_result = ResultPayload(result);
  std::vector<std::uint8_t> replay_material;
  Require(replay_result == first_result &&
              bridge::ReadStatementResultPageMaterial(
                  result, &replay_material) == SB_ENGINE_STATUS_OK &&
              replay_material == first_material &&
              probes.load(std::memory_order_relaxed) == 0,
          "003600 exact replay was not byte-identical and cancellation-stable");
  (void)sb_engine_result_release(result);
  (void)sb_engine_result_release(source_result);

  Require(bridge::ReleaseStatementContextReceipt(receipt) ==
              SB_ENGINE_STATUS_OK,
          "003600 statement receipt cleanup failed");
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  Require(api::EngineRollbackTransaction(rollback).ok,
          "003600 fixture transaction rollback failed");
  return EXIT_SUCCESS;
}
