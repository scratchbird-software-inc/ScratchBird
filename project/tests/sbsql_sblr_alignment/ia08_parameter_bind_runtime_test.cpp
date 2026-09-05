#include "engine/sblr/sblr_parameter_bind_runtime.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

template <std::size_t N>
std::array<std::uint8_t, N> Bytes(std::uint8_t seed) {
  std::array<std::uint8_t, N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = static_cast<std::uint8_t>(seed + i);
  }
  return out;
}

std::vector<std::uint8_t> Le64(std::uint64_t value) {
  std::vector<std::uint8_t> out;
  out.reserve(8);
  for (std::size_t i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
  }
  return out;
}

}  // namespace

int main() {
  namespace sblr = scratchbird::engine::sblr;
  std::string detail;

  sblr::SblrParameterBindRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 1;
  request.catalog_generation = 2;
  request.security_epoch = 3;
  request.resource_epoch = 4;
  const auto encoded_request =
      sblr::EncodeSblrParameterBindRequestV1(request);
  sblr::SblrParameterBindRequestV1 decoded_request;
  if (encoded_request.size() != 64 ||
      !sblr::DecodeSblrParameterBindRequestV1(
          encoded_request.data(), encoded_request.size(), &decoded_request,
          &detail)) {
    return EXIT_FAILURE;
  }
  auto malformed_request = encoded_request;
  malformed_request[12] = 1;
  if (sblr::DecodeSblrParameterBindRequestV1(
          malformed_request.data(), malformed_request.size(),
          &decoded_request, &detail)) {
    return EXIT_FAILURE;
  }
  malformed_request = encoded_request;
  malformed_request.push_back(0);
  if (sblr::DecodeSblrParameterBindRequestV1(
          malformed_request.data(), malformed_request.size(),
          &decoded_request, &detail)) {
    return EXIT_FAILURE;
  }

  sblr::SblrParameterValueSetV1 value_set;
  value_set.parameter_set_descriptor_uuid = Bytes<16>(21);
  value_set.descriptor_generation = 5;
  value_set.execution_uuid = Bytes<16>(41);
  value_set.statement_receipt_uuid = request.statement_receipt_uuid;
  sblr::SblrParameterValueRecordV1 record;
  record.slot_ordinal = 0;
  record.slot_uuid = Bytes<16>(61);
  record.datatype_descriptor_uuid = Bytes<16>(81);
  record.datatype_descriptor_generation = 6;
  record.direction = sblr::SblrParameterDirectionV1::in;
  record.state = sblr::SblrParameterValueStateV1::value;
  record.canonical_value_bytes = Le64(7);
  value_set.records.push_back(record);
  const auto encoded_values = sblr::EncodeSblrParameterValueSetV1(value_set);
  if (encoded_values.empty()) return EXIT_FAILURE;

  sblr::SblrParameterBindDescriptorV1 descriptor;
  descriptor.execution_uuid = value_set.execution_uuid;
  descriptor.statement_receipt_uuid = value_set.statement_receipt_uuid;
  descriptor.prepared_statement_uuid = Bytes<16>(101);
  descriptor.prepared_generation = 7;
  descriptor.parameter_set_uuid = value_set.parameter_set_descriptor_uuid;
  descriptor.parameter_set_generation = value_set.descriptor_generation;
  descriptor.ordered_slot_table_sha256 = Bytes<32>(121);
  descriptor.catalog_snapshot_uuid = Bytes<16>(161);
  descriptor.catalog_generation = request.catalog_generation;
  descriptor.security_epoch = request.security_epoch;
  descriptor.resource_epoch = request.resource_epoch;
  descriptor.mga_snapshot_uuid = Bytes<16>(181);
  descriptor.executor_availability_generation = 8;
  descriptor.canonical_value_vector = encoded_values;
  const auto encoded_descriptor =
      sblr::EncodeSblrParameterBindDescriptorV1(descriptor);
  sblr::SblrParameterBindDescriptorV1 decoded_descriptor;
  if (encoded_descriptor.size() != 256 + encoded_values.size() ||
      !sblr::DecodeSblrParameterBindDescriptorV1(
          encoded_descriptor.data(), encoded_descriptor.size(),
          &decoded_descriptor, &detail) ||
      decoded_descriptor.canonical_value_vector != encoded_values) {
    return EXIT_FAILURE;
  }
  auto malformed_descriptor = encoded_descriptor;
  malformed_descriptor[248] = 1;
  if (sblr::DecodeSblrParameterBindDescriptorV1(
          malformed_descriptor.data(), malformed_descriptor.size(),
          &decoded_descriptor, &detail)) {
    return EXIT_FAILURE;
  }
  malformed_descriptor = encoded_descriptor;
  malformed_descriptor[236] = 2;
  if (sblr::DecodeSblrParameterBindDescriptorV1(
          malformed_descriptor.data(), malformed_descriptor.size(),
          &decoded_descriptor, &detail)) {
    return EXIT_FAILURE;
  }
  malformed_descriptor = encoded_descriptor;
  malformed_descriptor.push_back(0);
  if (sblr::DecodeSblrParameterBindDescriptorV1(
          malformed_descriptor.data(), malformed_descriptor.size(),
          &decoded_descriptor, &detail)) {
    return EXIT_FAILURE;
  }
  auto gapped_value_set = value_set;
  gapped_value_set.records.front().slot_ordinal = 1;
  if (!sblr::EncodeSblrParameterValueSetV1(gapped_value_set).empty()) {
    return EXIT_FAILURE;
  }

  sblr::SblrParameterBindResultV1 result;
  result.execution_uuid = descriptor.execution_uuid;
  result.prepared_statement_uuid = descriptor.prepared_statement_uuid;
  result.prepared_generation = descriptor.prepared_generation;
  result.parameter_set_uuid = descriptor.parameter_set_uuid;
  result.parameter_set_generation = descriptor.parameter_set_generation;
  result.ordered_slot_table_sha256 = descriptor.ordered_slot_table_sha256;
  result.status = 1;
  result.publication_barrier = 1;
  result.bind_evidence_uuid = Bytes<16>(201);
  const auto encoded_result = sblr::EncodeSblrParameterBindResultV1(result);
  sblr::SblrParameterBindResultV1 decoded_result;
  if (encoded_result.size() != 192 ||
      !sblr::DecodeSblrParameterBindResultV1(
          encoded_result.data(), encoded_result.size(), &decoded_result,
          &detail)) {
    return EXIT_FAILURE;
  }
  auto malformed_result = encoded_result;
  malformed_result[188] = 1;
  if (sblr::DecodeSblrParameterBindResultV1(
          malformed_result.data(), malformed_result.size(), &decoded_result,
          &detail)) {
    return EXIT_FAILURE;
  }
  malformed_result = encoded_result;
  malformed_result[156] ^= 1;
  return sblr::DecodeSblrParameterBindResultV1(
             malformed_result.data(), malformed_result.size(),
             &decoded_result, &detail)
             ? EXIT_FAILURE
             : EXIT_SUCCESS;
}
