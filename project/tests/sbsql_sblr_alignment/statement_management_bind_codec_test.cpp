#include "wire/parser_server_ipc/sbps_statement_management_bind_codec.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace wire = scratchbird::wire::sbps_statement_management;

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

wire::Uuid Uuid(std::uint8_t seed) {
  wire::Uuid value{};
  for (std::size_t index = 0; index != value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

wire::Hash256 Hash(std::uint8_t seed) {
  wire::Hash256 value{};
  for (std::size_t index = 0; index != value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

}  // namespace

int main() {
  wire::PrepareBindRequestV1 prepare;
  prepare.authenticated_receipt_uuid = Uuid(1);
  prepare.occurrence = 1;
  prepare.statement_name = "report_one";
  prepare.canonical_container_bytes = {1, 2, 3, 4};
  prepare.canonical_execution_envelope_bytes = {5, 6, 7};
  std::vector<std::uint8_t> bytes;
  std::string detail;
  Require(wire::EncodePrepareBindRequestV1(prepare, &bytes, &detail),
          "prepare bind request did not encode");
  wire::PrepareBindRequestV1 decoded_prepare;
  Require(wire::DecodePrepareBindRequestV1(bytes.data(), bytes.size(),
                                           &decoded_prepare, &detail) &&
              decoded_prepare.statement_name == prepare.statement_name &&
              decoded_prepare.canonical_container_bytes ==
                  prepare.canonical_container_bytes &&
              decoded_prepare.canonical_execution_envelope_bytes ==
                  prepare.canonical_execution_envelope_bytes,
          "prepare bind request did not round trip");
  auto mutation = bytes;
  mutation.back() ^= 1;
  Require(!wire::DecodePrepareBindRequestV1(
              mutation.data(), mutation.size(), &decoded_prepare, &detail),
          "prepare bind request admitted a body hash mismatch");
  mutation = bytes;
  mutation.push_back(0);
  Require(!wire::DecodePrepareBindRequestV1(
              mutation.data(), mutation.size(), &decoded_prepare, &detail),
          "prepare bind request admitted a trailing byte");

  wire::PrepareBindAckV1 prepare_ack;
  prepare_ack.authenticated_receipt_uuid = prepare.authenticated_receipt_uuid;
  prepare_ack.occurrence = 1;
  prepare_ack.binding_uuid = Uuid(31);
  prepare_ack.binding_generation = 1;
  prepare_ack.statement_name_uuid = Uuid(61);
  prepare_ack.descriptor_sha256 = Hash(91);
  prepare_ack.request_evidence_sha256 = decoded_prepare.request_evidence_sha256;
  Require(wire::EncodePrepareBindAckV1(prepare_ack, &bytes, &detail),
          "prepare bind acknowledgement did not encode");
  wire::PrepareBindAckV1 decoded_prepare_ack;
  Require(wire::DecodePrepareBindAckV1(bytes.data(), bytes.size(),
                                       &decoded_prepare_ack, &detail),
          "prepare bind acknowledgement did not round trip");

  wire::ExecuteDirectBindRequestV1 direct;
  direct.authenticated_receipt_uuid = Uuid(3);
  direct.occurrence = 1;
  direct.canonical_container_bytes = {9, 8, 7, 6};
  direct.canonical_execution_envelope_bytes = {5, 4, 3};
  Require(wire::EncodeExecuteDirectBindRequestV1(direct, &bytes, &detail),
          "execute-direct bind request did not encode");
  wire::ExecuteDirectBindRequestV1 decoded_direct;
  Require(wire::DecodeExecuteDirectBindRequestV1(
              bytes.data(), bytes.size(), &decoded_direct, &detail) &&
              decoded_direct.canonical_container_bytes ==
                  direct.canonical_container_bytes &&
              decoded_direct.canonical_execution_envelope_bytes ==
                  direct.canonical_execution_envelope_bytes &&
              decoded_direct.canonical_parameter_bytes.empty(),
          "execute-direct bind request did not round trip");
  mutation = bytes;
  mutation[52] = 1;
  Require(!wire::DecodeExecuteDirectBindRequestV1(
              mutation.data(), mutation.size(), &decoded_direct, &detail),
          "execute-direct bind request admitted nonzero reserved bytes");
  mutation = bytes;
  mutation.back() ^= 1;
  Require(!wire::DecodeExecuteDirectBindRequestV1(
              mutation.data(), mutation.size(), &decoded_direct, &detail),
          "execute-direct bind request admitted a body hash mismatch");

  wire::ExecuteDirectBindAckV1 direct_ack;
  direct_ack.authenticated_receipt_uuid = direct.authenticated_receipt_uuid;
  direct_ack.occurrence = 1;
  direct_ack.binding_uuid = Uuid(33);
  direct_ack.binding_generation = 1;
  direct_ack.result_descriptor_uuid = Uuid(63);
  direct_ack.descriptor_sha256 = Hash(93);
  direct_ack.request_evidence_sha256 =
      decoded_direct.request_evidence_sha256;
  Require(wire::EncodeExecuteDirectBindAckV1(direct_ack, &bytes, &detail),
          "execute-direct bind acknowledgement did not encode");
  wire::ExecuteDirectBindAckV1 decoded_direct_ack;
  Require(wire::DecodeExecuteDirectBindAckV1(
              bytes.data(), bytes.size(), &decoded_direct_ack, &detail) &&
              decoded_direct_ack.binding_uuid == direct_ack.binding_uuid &&
              decoded_direct_ack.result_descriptor_uuid ==
                  direct_ack.result_descriptor_uuid,
          "execute-direct bind acknowledgement did not round trip");

  wire::FreeBindRequestV1 free_request;
  free_request.authenticated_receipt_uuid = Uuid(2);
  free_request.occurrence = 1;
  free_request.statement_name = "report_one";
  Require(wire::EncodeFreeBindRequestV1(free_request, &bytes, &detail),
          "free bind request did not encode");
  wire::FreeBindRequestV1 decoded_free;
  Require(wire::DecodeFreeBindRequestV1(bytes.data(), bytes.size(),
                                        &decoded_free, &detail) &&
              decoded_free.statement_name == free_request.statement_name,
          "free bind request did not round trip");
  mutation = bytes;
  mutation[40] = 2;
  Require(!wire::DecodeFreeBindRequestV1(
              mutation.data(), mutation.size(), &decoded_free, &detail),
          "free bind request admitted an unknown flag");

  wire::FreeBindAckV1 free_ack;
  free_ack.authenticated_receipt_uuid = free_request.authenticated_receipt_uuid;
  free_ack.occurrence = 1;
  free_ack.binding_uuid = Uuid(32);
  free_ack.binding_generation = 1;
  free_ack.statement_uuid = Uuid(62);
  free_ack.statement_name_uuid = Uuid(92);
  free_ack.prepared_generation = 1;
  free_ack.descriptor_sha256 = Hash(122);
  free_ack.request_evidence_sha256 = decoded_free.request_evidence_sha256;
  Require(wire::EncodeFreeBindAckV1(free_ack, &bytes, &detail),
          "free bind acknowledgement did not encode");
  wire::FreeBindAckV1 decoded_free_ack;
  Require(wire::DecodeFreeBindAckV1(bytes.data(), bytes.size(),
                                    &decoded_free_ack, &detail) &&
              decoded_free_ack.prepared_generation == 1,
          "free bind acknowledgement did not round trip");
  mutation = bytes;
  mutation[200] = 1;
  Require(!wire::DecodeFreeBindAckV1(
              mutation.data(), mutation.size(), &decoded_free_ack, &detail),
          "free bind acknowledgement admitted nonzero reserved bytes");

  wire::CancelBindRequestV1 cancel_request;
  cancel_request.authenticated_receipt_uuid = Uuid(4);
  cancel_request.occurrence = 1;
  cancel_request.statement_name = "report_one";
  cancel_request.reason = 1;
  cancel_request.mode = 1;
  Require(wire::EncodeCancelBindRequestV1(cancel_request, &bytes, &detail),
          "cancel bind request did not encode");
  wire::CancelBindRequestV1 decoded_cancel;
  Require(wire::DecodeCancelBindRequestV1(bytes.data(), bytes.size(),
                                          &decoded_cancel, &detail) &&
              decoded_cancel.statement_name == cancel_request.statement_name &&
              decoded_cancel.reason == 1 && decoded_cancel.mode == 1,
          "cancel bind request did not round trip");
  mutation = bytes;
  mutation[48] = 0;
  Require(!wire::DecodeCancelBindRequestV1(
              mutation.data(), mutation.size(), &decoded_cancel, &detail),
          "cancel bind request admitted an invalid reason");

  wire::CancelBindAckV1 cancel_ack;
  cancel_ack.authenticated_receipt_uuid =
      cancel_request.authenticated_receipt_uuid;
  cancel_ack.occurrence = 1;
  cancel_ack.binding_uuid = Uuid(34);
  cancel_ack.binding_generation = 1;
  cancel_ack.target_execution_uuid = Uuid(64);
  cancel_ack.target_statement_uuid = Uuid(84);
  cancel_ack.target_statement_receipt_uuid = Uuid(104);
  cancel_ack.cancel_operation_uuid = Uuid(124);
  cancel_ack.target_transaction_uuid = Uuid(144);
  cancel_ack.target_execution_generation = 1;
  cancel_ack.reason = 1;
  cancel_ack.mode = 1;
  cancel_ack.executor_availability_generation = 1;
  cancel_ack.descriptor_sha256 = Hash(164);
  cancel_ack.request_evidence_sha256 =
      decoded_cancel.request_evidence_sha256;
  Require(wire::EncodeCancelBindAckV1(cancel_ack, &bytes, &detail),
          "cancel bind acknowledgement did not encode");
  wire::CancelBindAckV1 decoded_cancel_ack;
  Require(wire::DecodeCancelBindAckV1(bytes.data(), bytes.size(),
                                      &decoded_cancel_ack, &detail) &&
              decoded_cancel_ack.target_execution_uuid ==
                  cancel_ack.target_execution_uuid &&
              decoded_cancel_ack.cancel_operation_uuid ==
                  cancel_ack.cancel_operation_uuid,
          "cancel bind acknowledgement did not round trip");
  mutation = bytes;
  mutation[268] = 1;
  Require(!wire::DecodeCancelBindAckV1(
              mutation.data(), mutation.size(), &decoded_cancel_ack, &detail),
          "cancel bind acknowledgement admitted nonzero reserved bytes");

  wire::ParameterBindRequestV1 parameter_bind;
  parameter_bind.authenticated_receipt_uuid = Uuid(5);
  parameter_bind.occurrence = 1;
  parameter_bind.statement_name =
      "019d0000-0000-7000-8000-000000001234";
  parameter_bind.quoted = true;
  parameter_bind.prepared_statement_uuid = Uuid(35);
  parameter_bind.prepared_generation = 1;
  parameter_bind.parameter_set_uuid = Uuid(65);
  parameter_bind.parameter_set_generation = 1;
  parameter_bind.ordered_slot_table_sha256 = Hash(95);
  parameter_bind.value_count = 1;
  parameter_bind.canonical_value_vector = {0x53, 0x42, 0x50, 0x56, 1, 2, 3};
  Require(wire::EncodeParameterBindRequestV1(parameter_bind, &bytes, &detail),
          "parameter bind private request did not encode");
  wire::ParameterBindRequestV1 decoded_parameter_bind;
  Require(wire::DecodeParameterBindRequestV1(
              bytes.data(), bytes.size(), &decoded_parameter_bind, &detail) &&
              decoded_parameter_bind.statement_name ==
                  parameter_bind.statement_name &&
              decoded_parameter_bind.canonical_value_vector ==
                  parameter_bind.canonical_value_vector &&
              decoded_parameter_bind.value_count == 1,
          "parameter bind private request did not round trip");
  mutation = bytes;
  mutation[248] = 1;
  Require(!wire::DecodeParameterBindRequestV1(
              mutation.data(), mutation.size(), &decoded_parameter_bind,
              &detail),
          "parameter bind private request admitted reserved bytes");
  mutation = bytes;
  mutation.back() ^= 1;
  Require(!wire::DecodeParameterBindRequestV1(
              mutation.data(), mutation.size(), &decoded_parameter_bind,
              &detail),
          "parameter bind private request admitted value hash drift");
  parameter_bind.batch_generation = 1;
  Require(!wire::EncodeParameterBindRequestV1(parameter_bind, &bytes, &detail),
          "parameter bind private request admitted a split batch identity");
  return 0;
}
