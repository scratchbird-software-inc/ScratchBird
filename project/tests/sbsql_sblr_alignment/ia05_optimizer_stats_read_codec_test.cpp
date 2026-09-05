// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_optimizer_stats_read_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "CSC-TEST-003618: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <std::size_t N>
std::array<std::uint8_t, N> Bytes(std::uint8_t seed) {
  std::array<std::uint8_t, N> out{};
  for (std::size_t index = 0; index != N; ++index) {
    out[index] = static_cast<std::uint8_t>(seed + index);
  }
  return out;
}

void RefuseRequest(std::vector<std::uint8_t> bytes,
                   std::string_view message) {
  sblr::SblrOptimizerStatsReadRequestV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrOptimizerStatsReadRequestV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseDescriptor(std::vector<std::uint8_t> bytes,
                      std::string_view message) {
  sblr::SblrOptimizerStatsReadDescriptorV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrOptimizerStatsReadDescriptorV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseResult(std::vector<std::uint8_t> bytes,
                  std::string_view message) {
  sblr::SblrOptimizerStatsReadResultV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrOptimizerStatsReadResultV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

}  // namespace

int main() {
  sblr::SblrOptimizerStatsReadRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 7;
  request.catalog_generation = 11;
  request.security_epoch = 13;
  request.resource_epoch = 17;
  const auto request_bytes =
      sblr::EncodeSblrOptimizerStatsReadRequestV1(request);
  sblr::SblrOptimizerStatsReadRequestV1 decoded_request;
  std::string detail;
  Require(request_bytes.size() == sblr::kSblrOptimizerStatsReadRequestBytes &&
              sblr::DecodeSblrOptimizerStatsReadRequestV1(
                  request_bytes.data(), request_bytes.size(),
                  &decoded_request, &detail) &&
              sblr::EncodeSblrOptimizerStatsReadRequestV1(decoded_request) ==
                  request_bytes,
          "canonical OSRQ did not round trip byte-identically");

  auto malformed = request_bytes;
  malformed[0] = 'X';
  RefuseRequest(malformed, "OSRQ with wrong magic was accepted");
  malformed = request_bytes;
  malformed[4] = 2;
  RefuseRequest(malformed, "OSRQ with wrong version was accepted");
  malformed = request_bytes;
  malformed[12] = 1;
  RefuseRequest(malformed, "OSRQ with nonzero flags was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 16, malformed.begin() + 32, 0);
  RefuseRequest(malformed, "OSRQ with nil receipt was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 32, malformed.begin() + 40, 0);
  RefuseRequest(malformed, "OSRQ with zero occurrence was accepted");
  malformed = request_bytes;
  malformed.pop_back();
  RefuseRequest(malformed, "truncated OSRQ was accepted");
  malformed = request_bytes;
  malformed.push_back(0);
  RefuseRequest(malformed, "OSRQ trailing bytes were accepted");

  sblr::SblrOptimizerStatsReadDescriptorV1 descriptor;
  descriptor.statistics_snapshot_uuid = Bytes<16>(21);
  descriptor.statement_receipt_uuid = request.statement_receipt_uuid;
  descriptor.statement_uuid = Bytes<16>(41);
  descriptor.statement_snapshot_uuid = Bytes<16>(61);
  descriptor.catalog_epoch_uuid = Bytes<16>(81);
  descriptor.catalog_generation = request.catalog_generation;
  descriptor.security_context_uuid = Bytes<16>(101);
  descriptor.security_epoch = request.security_epoch;
  descriptor.resource_admission_uuid = Bytes<16>(121);
  descriptor.resource_epoch = request.resource_epoch;
  descriptor.owning_transaction_uuid = Bytes<16>(141);
  descriptor.owning_local_transaction_id = 19;
  descriptor.inventory_generation = 23;
  descriptor.parser_package_uuid = Bytes<16>(161);
  descriptor.executor_availability_generation = 29;
  descriptor.optimizer_statistics_epoch = 31;
  const auto descriptor_bytes =
      sblr::EncodeSblrOptimizerStatsReadDescriptorV1(descriptor);
  sblr::SblrOptimizerStatsReadDescriptorV1 decoded_descriptor;
  Require(
      descriptor_bytes.size() ==
              sblr::kSblrOptimizerStatsReadDescriptorBytes &&
          sblr::DecodeSblrOptimizerStatsReadDescriptorV1(
              descriptor_bytes.data(), descriptor_bytes.size(),
              &decoded_descriptor, &detail) &&
          decoded_descriptor.descriptor_sha256 !=
              sblr::SblrOptimizerStatsReadSha256V1{} &&
          sblr::EncodeSblrOptimizerStatsReadDescriptorV1(
              decoded_descriptor) == descriptor_bytes,
      "canonical OSRD did not round trip byte-identically");

  auto invalid_descriptor = descriptor;
  invalid_descriptor.statistics_snapshot_uuid = {};
  Require(sblr::EncodeSblrOptimizerStatsReadDescriptorV1(invalid_descriptor)
              .empty(),
          "OSRD with nil statistics snapshot was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.flags = 0;
  Require(sblr::EncodeSblrOptimizerStatsReadDescriptorV1(invalid_descriptor)
              .empty(),
          "OSRD with incomplete catalog scope was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.executor_availability_generation = 0;
  Require(sblr::EncodeSblrOptimizerStatsReadDescriptorV1(invalid_descriptor)
              .empty(),
          "OSRD with no executor generation was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.optimizer_statistics_epoch = 0;
  Require(sblr::EncodeSblrOptimizerStatsReadDescriptorV1(invalid_descriptor)
              .empty(),
          "OSRD with no statistics epoch was encoded");

  malformed = descriptor_bytes;
  malformed[12] = 1;
  RefuseDescriptor(malformed, "OSRD with nonzero header flags was accepted");
  malformed = descriptor_bytes;
  malformed[188] = 1;
  RefuseDescriptor(malformed, "OSRD with nonzero reserved field was accepted");
  malformed = descriptor_bytes;
  malformed[192] ^= 1;
  RefuseDescriptor(malformed, "OSRD descriptor evidence tamper was accepted");
  malformed = descriptor_bytes;
  std::fill(malformed.begin() + 224, malformed.begin() + 240, 0);
  RefuseDescriptor(malformed, "OSRD with nil parser package was accepted");
  malformed = descriptor_bytes;
  std::fill(malformed.begin() + 248, malformed.begin() + 256, 0);
  RefuseDescriptor(malformed, "OSRD with zero statistics epoch was accepted");
  malformed = descriptor_bytes;
  malformed.pop_back();
  RefuseDescriptor(malformed, "truncated OSRD was accepted");
  malformed = descriptor_bytes;
  malformed.push_back(0);
  RefuseDescriptor(malformed, "OSRD trailing bytes were accepted");

  sblr::SblrOptimizerStatsReadResultV1 result;
  result.statistics_snapshot_uuid = descriptor.statistics_snapshot_uuid;
  result.statement_receipt_uuid = descriptor.statement_receipt_uuid;
  result.statement_snapshot_uuid = descriptor.statement_snapshot_uuid;
  result.catalog_generation = descriptor.catalog_generation;
  result.security_epoch = descriptor.security_epoch;
  result.resource_epoch = descriptor.resource_epoch;
  result.inventory_generation = descriptor.inventory_generation;
  result.visible_row_estimate = 31;
  result.retained_row_version_count = 37;
  result.row_store_bytes = 100;
  result.index_store_bytes = 50;
  result.table_size_bytes = 150;
  result.executor_availability_generation =
      descriptor.executor_availability_generation;
  result.optimizer_statistics_epoch = descriptor.optimizer_statistics_epoch;
  const auto result_bytes =
      sblr::EncodeSblrOptimizerStatsReadResultV1(result);
  sblr::SblrOptimizerStatsReadResultV1 decoded_result;
  Require(result_bytes.size() == sblr::kSblrOptimizerStatsReadResultBytes &&
              sblr::DecodeSblrOptimizerStatsReadResultV1(
                  result_bytes.data(), result_bytes.size(), &decoded_result,
                  &detail) &&
              decoded_result.result_material_sha256 !=
                  sblr::SblrOptimizerStatsReadSha256V1{} &&
              decoded_result.executor_evidence_sha256 !=
                  sblr::SblrOptimizerStatsReadSha256V1{} &&
              sblr::EncodeSblrOptimizerStatsReadResultV1(decoded_result) ==
                  result_bytes,
          "canonical OSRR did not round trip byte-identically");

  auto invalid_result = result;
  invalid_result.table_size_bytes = 149;
  Require(sblr::EncodeSblrOptimizerStatsReadResultV1(invalid_result).empty(),
          "OSRR with inconsistent table byte total was encoded");
  invalid_result = result;
  invalid_result.row_store_bytes =
      std::numeric_limits<std::uint64_t>::max();
  invalid_result.index_store_bytes = 1;
  invalid_result.table_size_bytes = 0;
  Require(sblr::EncodeSblrOptimizerStatsReadResultV1(invalid_result).empty(),
          "OSRR with overflowing table byte total was encoded");

  malformed = result_bytes;
  malformed[12] = 1;
  RefuseResult(malformed, "OSRR with nonzero header flags was accepted");
  malformed = result_bytes;
  malformed[136] = 0;
  RefuseResult(malformed, "OSRR with incomplete scope flags was accepted");
  malformed = result_bytes;
  malformed[140] = 1;
  RefuseResult(malformed, "OSRR with nonzero reserved field was accepted");
  malformed = result_bytes;
  malformed[144] ^= 1;
  RefuseResult(malformed, "OSRR material evidence tamper was accepted");
  malformed = result_bytes;
  malformed[176] ^= 1;
  RefuseResult(malformed, "OSRR executor evidence tamper was accepted");
  malformed = result_bytes;
  std::fill(malformed.begin() + 216, malformed.begin() + 224, 0);
  RefuseResult(malformed, "OSRR with zero statistics epoch was accepted");
  malformed = result_bytes;
  malformed[128] ^= 1;
  RefuseResult(malformed, "OSRR table byte total tamper was accepted");
  malformed = result_bytes;
  malformed.pop_back();
  RefuseResult(malformed, "truncated OSRR was accepted");
  malformed = result_bytes;
  malformed.push_back(0);
  RefuseResult(malformed, "OSRR trailing bytes were accepted");
  return EXIT_SUCCESS;
}
