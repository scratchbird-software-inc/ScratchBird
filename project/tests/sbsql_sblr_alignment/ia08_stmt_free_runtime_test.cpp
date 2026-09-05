#include "engine/sblr/sblr_stmt_free_runtime.hpp"

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
  sblr::SblrStmtFreeRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 1;
  request.catalog_generation = 2;
  request.security_epoch = 3;
  request.resource_epoch = 4;
  auto encoded_request = sblr::EncodeSblrStmtFreeRequestV1(request);
  sblr::SblrStmtFreeRequestV1 decoded_request;
  if (encoded_request.size() != 64 ||
      !sblr::DecodeSblrStmtFreeRequestV1(
          encoded_request.data(), encoded_request.size(), &decoded_request,
          &detail)) return EXIT_FAILURE;

  sblr::SblrStmtFreeDescriptorV1 descriptor;
  descriptor.statement_uuid = Bytes<16>(21);
  descriptor.statement_name_uuid = Bytes<16>(41);
  descriptor.statement_receipt_uuid = request.statement_receipt_uuid;
  descriptor.catalog_snapshot_uuid = Bytes<16>(61);
  descriptor.catalog_generation = request.catalog_generation;
  descriptor.security_epoch = request.security_epoch;
  descriptor.resource_epoch = request.resource_epoch;
  descriptor.mga_snapshot_uuid = Bytes<16>(81);
  descriptor.prepared_generation = 1;
  descriptor.executor_availability_generation = 5;
  auto encoded_descriptor = sblr::EncodeSblrStmtFreeDescriptorV1(descriptor);
  sblr::SblrStmtFreeDescriptorV1 decoded_descriptor;
  if (encoded_descriptor.size() != 176 ||
      !sblr::DecodeSblrStmtFreeDescriptorV1(
          encoded_descriptor.data(), encoded_descriptor.size(),
          &decoded_descriptor, &detail)) return EXIT_FAILURE;
  auto malformed_descriptor = encoded_descriptor;
  malformed_descriptor[136] ^= 1;
  if (sblr::DecodeSblrStmtFreeDescriptorV1(
          malformed_descriptor.data(), malformed_descriptor.size(),
          &decoded_descriptor, &detail)) return EXIT_FAILURE;

  sblr::SblrStmtFreeResultV1 result;
  result.statement_uuid = descriptor.statement_uuid;
  result.terminal_prepared_generation = 2;
  result.cleanup_evidence_uuid = Bytes<16>(101);
  result.executor_availability_generation =
      descriptor.executor_availability_generation;
  result.catalog_generation = request.catalog_generation;
  result.security_epoch = request.security_epoch;
  result.resource_epoch = request.resource_epoch;
  auto encoded_result = sblr::EncodeSblrStmtFreeResultV1(result);
  sblr::SblrStmtFreeResultV1 decoded_result;
  if (encoded_result.size() != 128 ||
      !sblr::DecodeSblrStmtFreeResultV1(
          encoded_result.data(), encoded_result.size(), &decoded_result,
          &detail)) return EXIT_FAILURE;
  auto malformed_result = encoded_result;
  malformed_result[92] ^= 1;
  return sblr::DecodeSblrStmtFreeResultV1(
             malformed_result.data(), malformed_result.size(), &decoded_result,
             &detail)
             ? EXIT_FAILURE
             : EXIT_SUCCESS;
}
