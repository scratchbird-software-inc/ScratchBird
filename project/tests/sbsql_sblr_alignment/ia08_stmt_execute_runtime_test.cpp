#include "engine/sblr/sblr_stmt_execute_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

template <std::size_t N>
std::array<std::uint8_t, N> Filled(std::uint8_t seed) {
  std::array<std::uint8_t, N> value{};
  for (std::size_t i = 0; i < value.size(); ++i) {
    value[i] = static_cast<std::uint8_t>(seed + i);
  }
  return value;
}

}  // namespace

int main() {
  namespace sblr = scratchbird::engine::sblr;

  sblr::SblrStmtExecuteRequestV1 request;
  request.statement_receipt_uuid = Filled<16>(1);
  request.occurrence = 7;
  request.catalog_generation = 11;
  request.security_epoch = 13;
  request.resource_epoch = 17;
  request.statement_name = "prepared_one";
  const auto encoded_request = sblr::EncodeSblrStmtExecuteRequestV1(request);
  sblr::SblrStmtExecuteRequestV1 decoded_request;
  std::string detail;
  if (encoded_request.empty() ||
      !sblr::DecodeSblrStmtExecuteRequestV1(
          encoded_request.data(), encoded_request.size(), &decoded_request,
          &detail) ||
      decoded_request.statement_name != request.statement_name ||
      decoded_request.occurrence != request.occurrence) {
    std::cerr << "stmt execute request round trip failed: " << detail << '\n';
    return 1;
  }

  sblr::SblrStmtExecuteDescriptorV1 descriptor;
  descriptor.execution_uuid = Filled<16>(21);
  descriptor.statement_uuid = Filled<16>(41);
  descriptor.statement_name_uuid = Filled<16>(61);
  descriptor.statement_receipt_uuid = request.statement_receipt_uuid;
  descriptor.catalog_snapshot_uuid = Filled<16>(81);
  descriptor.catalog_generation = request.catalog_generation;
  descriptor.security_epoch = request.security_epoch;
  descriptor.resource_epoch = request.resource_epoch;
  descriptor.mga_snapshot_uuid = Filled<16>(101);
  descriptor.prepared_generation = 1;
  descriptor.prepared_descriptor_sha256 = Filled<32>(121);
  descriptor.result_descriptor_uuid = Filled<16>(161);
  descriptor.parser_package_uuid = Filled<16>(181);
  descriptor.executor_availability_generation = 19;
  const auto encoded_descriptor =
      sblr::EncodeSblrStmtExecuteDescriptorV1(descriptor);
  sblr::SblrStmtExecuteDescriptorV1 decoded_descriptor;
  if (encoded_descriptor.empty() || encoded_descriptor.size() != 320 ||
      !sblr::DecodeSblrStmtExecuteDescriptorV1(
          encoded_descriptor.data(), encoded_descriptor.size(),
          &decoded_descriptor, &detail) ||
      decoded_descriptor.statement_uuid != descriptor.statement_uuid ||
      decoded_descriptor.prepared_generation != 1 ||
      decoded_descriptor.executor_availability_generation != 19) {
    std::cerr << "stmt execute descriptor round trip failed: " << detail
              << '\n';
    return 2;
  }
  auto corrupted = encoded_descriptor;
  corrupted[280] ^= 0x01;
  if (sblr::DecodeSblrStmtExecuteDescriptorV1(
          corrupted.data(), corrupted.size(), &decoded_descriptor, &detail)) {
    std::cerr << "stmt execute descriptor accepted corrupt evidence\n";
    return 3;
  }
  if (sblr::DecodeSblrStmtExecuteDescriptorV1(
          encoded_descriptor.data(), encoded_descriptor.size() - 1,
          &decoded_descriptor, &detail)) {
    std::cerr << "stmt execute descriptor accepted truncated bytes\n";
    return 4;
  }

  sblr::SblrStmtExecuteResultV1 result;
  result.execution_uuid = descriptor.execution_uuid;
  result.statement_receipt_uuid = descriptor.statement_receipt_uuid;
  result.result_descriptor_uuid = descriptor.result_descriptor_uuid;
  result.result_handle_uuid = Filled<16>(201);
  result.mga_snapshot_uuid = descriptor.mga_snapshot_uuid;
  result.catalog_generation = descriptor.catalog_generation;
  result.security_epoch = descriptor.security_epoch;
  result.resource_epoch = descriptor.resource_epoch;
  result.executor_availability_generation =
      descriptor.executor_availability_generation;
  result.operation_evidence_uuid = Filled<16>(221);
  const auto encoded_result = sblr::EncodeSblrStmtExecuteResultV1(result);
  sblr::SblrStmtExecuteResultV1 decoded_result;
  if (encoded_result.size() != 192 ||
      !sblr::DecodeSblrStmtExecuteResultV1(
          encoded_result.data(), encoded_result.size(), &decoded_result,
          &detail) ||
      decoded_result.result_handle_uuid != result.result_handle_uuid) {
    std::cerr << "stmt execute result round trip failed: " << detail << '\n';
    return 5;
  }
  return 0;
}
