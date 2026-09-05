#include "engine/sblr/sblr_stmt_prepare_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

template <std::size_t N>
std::array<std::uint8_t, N> Bytes(std::uint8_t first) {
  std::array<std::uint8_t, N> value{};
  for (std::size_t index = 0; index < N; ++index) {
    value[index] = static_cast<std::uint8_t>(first + index);
  }
  return value;
}

bool Require(bool condition) { return condition; }

}  // namespace

int main() {
  namespace sblr = scratchbird::engine::sblr;
  std::string detail;

  sblr::SblrStmtPrepareRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 7;
  request.catalog_generation = 11;
  request.security_epoch = 13;
  request.resource_epoch = 17;
  const auto encoded_request = sblr::EncodeSblrStmtPrepareRequestV1(request);
  if (!Require(encoded_request.size() == 64 &&
               std::equal(encoded_request.begin(), encoded_request.begin() + 4,
                          std::array<std::uint8_t, 4>{'S', 'B', 'P', 'Q'}.begin()))) {
    return EXIT_FAILURE;
  }
  sblr::SblrStmtPrepareRequestV1 decoded_request;
  if (!sblr::DecodeSblrStmtPrepareRequestV1(
          encoded_request.data(), encoded_request.size(), &decoded_request,
          &detail) ||
      decoded_request.statement_receipt_uuid != request.statement_receipt_uuid ||
      decoded_request.occurrence != request.occurrence ||
      decoded_request.resource_epoch != request.resource_epoch) {
    return EXIT_FAILURE;
  }

  sblr::SblrStmtPrepareDescriptorV1 descriptor;
  descriptor.statement_uuid = Bytes<16>(21);
  descriptor.statement_name_uuid = Bytes<16>(41);
  descriptor.statement_receipt_uuid = request.statement_receipt_uuid;
  descriptor.catalog_snapshot_uuid = Bytes<16>(61);
  descriptor.catalog_generation = request.catalog_generation;
  descriptor.security_epoch = request.security_epoch;
  descriptor.resource_epoch = request.resource_epoch;
  descriptor.mga_snapshot_uuid = Bytes<16>(81);
  descriptor.statement_kind = 1;
  descriptor.parameter_mode = 0;
  descriptor.result_mode = 1;
  descriptor.result_descriptor_uuid = Bytes<16>(101);
  descriptor.executor_availability_generation = 19;
  descriptor.parser_package_uuid = Bytes<16>(121);
  descriptor.canonical_sblr_bytes = {'S', 'B', 'O', 'S', 1, 0, 0, 0};

  const auto encoded_descriptor =
      sblr::EncodeSblrStmtPrepareDescriptorV1(descriptor);
  if (!Require(encoded_descriptor.size() == 264 &&
               encoded_descriptor[0] == 'S' && encoded_descriptor[1] == 'B' &&
               encoded_descriptor[2] == 'P' && encoded_descriptor[3] == 'D')) {
    return EXIT_FAILURE;
  }
  sblr::SblrStmtPrepareDescriptorV1 decoded_descriptor;
  if (!sblr::DecodeSblrStmtPrepareDescriptorV1(
          encoded_descriptor.data(), encoded_descriptor.size(),
          &decoded_descriptor, &detail) ||
      decoded_descriptor.statement_uuid != descriptor.statement_uuid ||
      decoded_descriptor.statement_name_uuid != descriptor.statement_name_uuid ||
      decoded_descriptor.canonical_sblr_bytes !=
          descriptor.canonical_sblr_bytes ||
      decoded_descriptor.result_descriptor_uuid !=
          descriptor.result_descriptor_uuid) {
    return EXIT_FAILURE;
  }

  auto malformed_descriptor = encoded_descriptor;
  malformed_descriptor[208] ^= 1;
  if (sblr::DecodeSblrStmtPrepareDescriptorV1(
          malformed_descriptor.data(), malformed_descriptor.size(),
          &decoded_descriptor, &detail)) {
    return EXIT_FAILURE;
  }
  malformed_descriptor = encoded_descriptor;
  malformed_descriptor.push_back(0);
  if (sblr::DecodeSblrStmtPrepareDescriptorV1(
          malformed_descriptor.data(), malformed_descriptor.size(),
          &decoded_descriptor, &detail)) {
    return EXIT_FAILURE;
  }

  sblr::SblrStmtPrepareResultV1 result;
  result.statement_uuid = descriptor.statement_uuid;
  result.prepared_generation = 1;
  result.result_descriptor_uuid = descriptor.result_descriptor_uuid;
  result.catalog_generation = descriptor.catalog_generation;
  result.security_epoch = descriptor.security_epoch;
  result.resource_epoch = descriptor.resource_epoch;
  result.mga_snapshot_uuid = descriptor.mga_snapshot_uuid;
  result.executor_availability_generation =
      descriptor.executor_availability_generation;
  const auto encoded_result = sblr::EncodeSblrStmtPrepareResultV1(result);
  if (!Require(encoded_result.size() == 160 && encoded_result[0] == 'S' &&
               encoded_result[1] == 'B' && encoded_result[2] == 'P' &&
               encoded_result[3] == 'R')) {
    return EXIT_FAILURE;
  }
  sblr::SblrStmtPrepareResultV1 decoded_result;
  if (!sblr::DecodeSblrStmtPrepareResultV1(
          encoded_result.data(), encoded_result.size(), &decoded_result,
          &detail) ||
      decoded_result.statement_uuid != result.statement_uuid ||
      decoded_result.prepared_generation != 1 ||
      decoded_result.publication_barrier != 1) {
    return EXIT_FAILURE;
  }
  auto malformed_result = encoded_result;
  malformed_result[124] ^= 1;
  if (sblr::DecodeSblrStmtPrepareResultV1(
          malformed_result.data(), malformed_result.size(), &decoded_result,
          &detail)) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
