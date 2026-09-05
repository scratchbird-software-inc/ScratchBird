#include "engine/sblr/sblr_stmt_cancel_runtime.hpp"

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
}  // namespace

int main() {
  namespace sblr = scratchbird::engine::sblr;
  std::string detail;
  sblr::SblrStmtCancelRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 1;
  request.catalog_generation = 2;
  request.security_epoch = 3;
  request.resource_epoch = 4;
  auto encoded_request = sblr::EncodeSblrStmtCancelRequestV1(request);
  sblr::SblrStmtCancelRequestV1 decoded_request;
  if (encoded_request.size() != 64 ||
      !sblr::DecodeSblrStmtCancelRequestV1(
          encoded_request.data(), encoded_request.size(), &decoded_request,
          &detail)) {
    return EXIT_FAILURE;
  }
  auto malformed_request = encoded_request;
  malformed_request.push_back(0);
  if (sblr::DecodeSblrStmtCancelRequestV1(
          malformed_request.data(), malformed_request.size(), &decoded_request,
          &detail)) {
    return EXIT_FAILURE;
  }

  sblr::SblrStmtCancelDescriptorV1 descriptor;
  descriptor.target_execution_uuid = Bytes<16>(21);
  descriptor.target_statement_uuid = Bytes<16>(41);
  descriptor.target_statement_receipt_uuid = Bytes<16>(61);
  descriptor.cancel_operation_uuid = Bytes<16>(81);
  descriptor.target_transaction_uuid = Bytes<16>(101);
  descriptor.target_execution_generation = 1;
  descriptor.reason = 1;
  descriptor.mode = 1;
  descriptor.executor_availability_generation = 5;
  auto encoded_descriptor =
      sblr::EncodeSblrStmtCancelDescriptorV1(descriptor);
  sblr::SblrStmtCancelDescriptorV1 decoded_descriptor;
  if (encoded_descriptor.size() != 176 ||
      !sblr::DecodeSblrStmtCancelDescriptorV1(
          encoded_descriptor.data(), encoded_descriptor.size(),
          &decoded_descriptor, &detail)) {
    return EXIT_FAILURE;
  }
  auto malformed_descriptor = encoded_descriptor;
  malformed_descriptor[104] = 0;
  if (sblr::DecodeSblrStmtCancelDescriptorV1(
          malformed_descriptor.data(), malformed_descriptor.size(),
          &decoded_descriptor, &detail)) {
    return EXIT_FAILURE;
  }
  malformed_descriptor = encoded_descriptor;
  malformed_descriptor[124] ^= 1;
  if (sblr::DecodeSblrStmtCancelDescriptorV1(
          malformed_descriptor.data(), malformed_descriptor.size(),
          &decoded_descriptor, &detail)) {
    return EXIT_FAILURE;
  }

  sblr::SblrStmtCancelResultV1 result;
  result.target_execution_uuid = descriptor.target_execution_uuid;
  result.cancel_operation_uuid = descriptor.cancel_operation_uuid;
  result.state = 3;
  result.finality = 1;
  result.publication_barrier = 1;
  result.cancellation_evidence_uuid = Bytes<16>(121);
  result.target_execution_generation = descriptor.target_execution_generation;
  result.executor_availability_generation =
      descriptor.executor_availability_generation;
  auto encoded_result = sblr::EncodeSblrStmtCancelResultV1(result);
  sblr::SblrStmtCancelResultV1 decoded_result;
  if (encoded_result.size() != 128 ||
      !sblr::DecodeSblrStmtCancelResultV1(
          encoded_result.data(), encoded_result.size(), &decoded_result,
          &detail)) {
    return EXIT_FAILURE;
  }
  auto malformed_result = encoded_result;
  malformed_result[50] = 0;
  if (sblr::DecodeSblrStmtCancelResultV1(
          malformed_result.data(), malformed_result.size(), &decoded_result,
          &detail)) {
    return EXIT_FAILURE;
  }
  malformed_result = encoded_result;
  malformed_result[84] ^= 1;
  return sblr::DecodeSblrStmtCancelResultV1(
             malformed_result.data(), malformed_result.size(), &decoded_result,
             &detail)
             ? EXIT_FAILURE
             : EXIT_SUCCESS;
}
