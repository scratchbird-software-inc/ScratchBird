#include "engine/sblr/sblr_stmt_execute_direct_runtime.hpp"

#include <array>
#include <cstdlib>
#include <string>

namespace {
template <std::size_t N>
std::array<std::uint8_t, N> Bytes(std::uint8_t seed) {
  std::array<std::uint8_t, N> out{};
  for (std::size_t i = 0; i < N; ++i) out[i] = seed + i;
  return out;
}
}

int main() {
  namespace sblr = scratchbird::engine::sblr;
  std::string detail;
  sblr::SblrStmtExecuteDirectRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 1;
  request.catalog_generation = 2;
  request.security_epoch = 3;
  request.resource_epoch = 4;
  auto encoded_request = sblr::EncodeSblrStmtExecuteDirectRequestV1(request);
  sblr::SblrStmtExecuteDirectRequestV1 decoded_request;
  if (encoded_request.size() != 64 ||
      !sblr::DecodeSblrStmtExecuteDirectRequestV1(
          encoded_request.data(), encoded_request.size(), &decoded_request,
          &detail)) return EXIT_FAILURE;

  sblr::SblrStmtExecuteDirectDescriptorV1 descriptor;
  descriptor.execution_uuid = Bytes<16>(21);
  descriptor.statement_receipt_uuid = request.statement_receipt_uuid;
  descriptor.catalog_snapshot_uuid = Bytes<16>(41);
  descriptor.catalog_generation = request.catalog_generation;
  descriptor.security_epoch = request.security_epoch;
  descriptor.resource_epoch = request.resource_epoch;
  descriptor.mga_snapshot_uuid = Bytes<16>(61);
  descriptor.result_descriptor_uuid = Bytes<16>(81);
  descriptor.parser_package_uuid = Bytes<16>(101);
  descriptor.executor_availability_generation = 5;
  descriptor.canonical_sblr_bytes = {'S', 'B', 'O', 'S', 1};
  descriptor.canonical_parameter_bytes = {1, 2, 3};
  auto encoded_descriptor =
      sblr::EncodeSblrStmtExecuteDirectDescriptorV1(descriptor);
  sblr::SblrStmtExecuteDirectDescriptorV1 decoded_descriptor;
  if (encoded_descriptor.size() != 264 ||
      !sblr::DecodeSblrStmtExecuteDirectDescriptorV1(
          encoded_descriptor.data(), encoded_descriptor.size(),
          &decoded_descriptor, &detail) ||
      decoded_descriptor.canonical_parameter_bytes !=
          descriptor.canonical_parameter_bytes) return EXIT_FAILURE;
  auto malformed_descriptor = encoded_descriptor;
  malformed_descriptor[200] ^= 1;
  if (sblr::DecodeSblrStmtExecuteDirectDescriptorV1(
          malformed_descriptor.data(), malformed_descriptor.size(),
          &decoded_descriptor, &detail)) return EXIT_FAILURE;

  sblr::SblrStmtExecuteDirectResultV1 result;
  result.execution_uuid = descriptor.execution_uuid;
  result.statement_receipt_uuid = request.statement_receipt_uuid;
  result.result_descriptor_uuid = descriptor.result_descriptor_uuid;
  result.result_handle_uuid = Bytes<16>(121);
  result.mga_snapshot_uuid = descriptor.mga_snapshot_uuid;
  result.catalog_generation = request.catalog_generation;
  result.security_epoch = request.security_epoch;
  result.resource_epoch = request.resource_epoch;
  result.executor_availability_generation =
      descriptor.executor_availability_generation;
  result.operation_evidence_uuid = Bytes<16>(141);
  auto encoded_result = sblr::EncodeSblrStmtExecuteDirectResultV1(result);
  sblr::SblrStmtExecuteDirectResultV1 decoded_result;
  if (encoded_result.size() != 192 ||
      !sblr::DecodeSblrStmtExecuteDirectResultV1(
          encoded_result.data(), encoded_result.size(), &decoded_result,
          &detail)) return EXIT_FAILURE;
  auto malformed_result = encoded_result;
  malformed_result[132] ^= 1;
  return sblr::DecodeSblrStmtExecuteDirectResultV1(
             malformed_result.data(), malformed_result.size(), &decoded_result,
             &detail)
             ? EXIT_FAILURE
             : EXIT_SUCCESS;
}
