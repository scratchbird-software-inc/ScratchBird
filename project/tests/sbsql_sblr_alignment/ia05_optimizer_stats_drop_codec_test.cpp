// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_optimizer_stats_drop_runtime.hpp"

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
  std::cerr << "CSC-TEST-003622: " << message << '\n';
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
  sblr::SblrOptimizerStatsDropRequestV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrOptimizerStatsDropRequestV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseDescriptor(std::vector<std::uint8_t> bytes,
                      std::string_view message) {
  sblr::SblrOptimizerStatsDropDescriptorV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrOptimizerStatsDropDescriptorV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseResult(std::vector<std::uint8_t> bytes,
                  std::string_view message) {
  sblr::SblrOptimizerStatsDropResultV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrOptimizerStatsDropResultV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

}  // namespace

int main() {
  sblr::SblrOptimizerStatsDropRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 7;
  request.catalog_generation = 11;
  request.security_epoch = 13;
  request.resource_epoch = 17;
  const auto request_bytes =
      sblr::EncodeSblrOptimizerStatsDropRequestV1(request);
  sblr::SblrOptimizerStatsDropRequestV1 decoded_request;
  std::string detail;
  Require(request_bytes.size() == sblr::kSblrOptimizerStatsDropRequestBytes &&
              sblr::DecodeSblrOptimizerStatsDropRequestV1(
                  request_bytes.data(), request_bytes.size(),
                  &decoded_request, &detail) &&
              sblr::EncodeSblrOptimizerStatsDropRequestV1(decoded_request) ==
                  request_bytes,
          "canonical OSDQ did not round trip byte-identically");

  auto malformed = request_bytes;
  malformed[0] = 'X';
  RefuseRequest(malformed, "OSDQ with wrong magic was accepted");
  malformed = request_bytes;
  malformed[4] = 2;
  RefuseRequest(malformed, "OSDQ with wrong version was accepted");
  malformed = request_bytes;
  malformed[12] = 1;
  RefuseRequest(malformed, "OSDQ with nonzero header flags was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 16, malformed.begin() + 32, 0);
  RefuseRequest(malformed, "OSDQ with nil receipt was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 32, malformed.begin() + 40, 0);
  RefuseRequest(malformed, "OSDQ with zero occurrence was accepted");
  malformed = request_bytes;
  malformed.pop_back();
  RefuseRequest(malformed, "truncated OSDQ was accepted");
  malformed = request_bytes;
  malformed.push_back(0);
  RefuseRequest(malformed, "OSDQ trailing bytes were accepted");

  sblr::SblrOptimizerStatsDropDescriptorV1 descriptor;
  descriptor.effect_uuid = Bytes<16>(21);
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
  descriptor.expected_statistics_epoch = 29;
  descriptor.expected_journal_generation = 31;
  descriptor.authorization_authority_uuid = Bytes<16>(161);
  descriptor.authorization_generation = 37;
  descriptor.authorization_policy_epoch = 41;
  descriptor.parser_package_uuid = Bytes<16>(181);
  descriptor.executor_availability_generation = 43;
  descriptor.proposed_effect_generation = 32;
  descriptor.next_statistics_epoch = 30;
  const auto descriptor_bytes =
      sblr::EncodeSblrOptimizerStatsDropDescriptorV1(descriptor);
  sblr::SblrOptimizerStatsDropDescriptorV1 decoded_descriptor;
  Require(descriptor_bytes.size() ==
              sblr::kSblrOptimizerStatsDropDescriptorBytes &&
              sblr::DecodeSblrOptimizerStatsDropDescriptorV1(
                  descriptor_bytes.data(), descriptor_bytes.size(),
                  &decoded_descriptor, &detail) &&
              decoded_descriptor.descriptor_sha256 !=
                  sblr::SblrOptimizerStatsDropSha256V1{} &&
              sblr::EncodeSblrOptimizerStatsDropDescriptorV1(
                  decoded_descriptor) == descriptor_bytes,
          "canonical OSDD did not round trip byte-identically");

  auto invalid_descriptor = descriptor;
  invalid_descriptor.effect_uuid = {};
  Require(sblr::EncodeSblrOptimizerStatsDropDescriptorV1(invalid_descriptor)
              .empty(),
          "OSDD with nil effect identity was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.flags = 0;
  Require(sblr::EncodeSblrOptimizerStatsDropDescriptorV1(invalid_descriptor)
              .empty(),
          "OSDD with incomplete scope was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.proposed_effect_generation = 33;
  Require(sblr::EncodeSblrOptimizerStatsDropDescriptorV1(invalid_descriptor)
              .empty(),
          "OSDD with a non-successor journal generation was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.next_statistics_epoch = 31;
  Require(sblr::EncodeSblrOptimizerStatsDropDescriptorV1(invalid_descriptor)
              .empty(),
          "OSDD with a non-successor statistics epoch was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.expected_journal_generation =
      std::numeric_limits<std::uint64_t>::max();
  invalid_descriptor.proposed_effect_generation = 0;
  Require(sblr::EncodeSblrOptimizerStatsDropDescriptorV1(invalid_descriptor)
              .empty(),
          "OSDD accepted journal-generation overflow");

  malformed = descriptor_bytes;
  malformed[12] = 1;
  RefuseDescriptor(malformed, "OSDD with nonzero header flags was accepted");
  malformed = descriptor_bytes;
  malformed[236] = 1;
  RefuseDescriptor(malformed, "OSDD with nonzero reserved field was accepted");
  malformed = descriptor_bytes;
  malformed[240] ^= 1;
  RefuseDescriptor(malformed, "OSDD descriptor evidence tamper was accepted");
  malformed = descriptor_bytes;
  std::fill(malformed.begin() + 296, malformed.begin() + 304, 0);
  RefuseDescriptor(malformed, "OSDD with zero effect generation was accepted");
  malformed = descriptor_bytes;
  malformed.pop_back();
  RefuseDescriptor(malformed, "truncated OSDD was accepted");
  malformed = descriptor_bytes;
  malformed.push_back(0);
  RefuseDescriptor(malformed, "OSDD trailing bytes were accepted");

  sblr::SblrOptimizerStatsDropResultV1 result;
  result.effect_uuid = descriptor.effect_uuid;
  result.statement_receipt_uuid = descriptor.statement_receipt_uuid;
  result.durable_publication_uuid = Bytes<16>(201);
  result.prior_statistics_epoch = descriptor.expected_statistics_epoch;
  result.statistics_epoch = descriptor.next_statistics_epoch;
  result.effect_generation = descriptor.proposed_effect_generation;
  result.catalog_generation = descriptor.catalog_generation;
  result.security_epoch = descriptor.security_epoch;
  result.resource_epoch = descriptor.resource_epoch;
  result.inventory_generation = descriptor.inventory_generation;
  result.cache_invalidation_generation = descriptor.next_statistics_epoch;
  result.executor_availability_generation =
      descriptor.executor_availability_generation;
  result.publication_barrier_generation =
      descriptor.proposed_effect_generation;
  const auto result_bytes =
      sblr::EncodeSblrOptimizerStatsDropResultV1(result);
  sblr::SblrOptimizerStatsDropResultV1 decoded_result;
  Require(result_bytes.size() == sblr::kSblrOptimizerStatsDropResultBytes &&
              sblr::DecodeSblrOptimizerStatsDropResultV1(
                  result_bytes.data(), result_bytes.size(), &decoded_result,
                  &detail) &&
              decoded_result.result_material_sha256 !=
                  sblr::SblrOptimizerStatsDropSha256V1{} &&
              decoded_result.executor_evidence_sha256 !=
                  sblr::SblrOptimizerStatsDropSha256V1{} &&
              sblr::EncodeSblrOptimizerStatsDropResultV1(decoded_result) ==
                  result_bytes,
          "canonical OSDR did not round trip byte-identically");

  auto invalid_result = result;
  invalid_result.durable_publication_uuid = descriptor.effect_uuid;
  Require(sblr::EncodeSblrOptimizerStatsDropResultV1(invalid_result).empty(),
          "OSDR reused its effect identity as publication identity");
  invalid_result = result;
  invalid_result.statistics_epoch = result.prior_statistics_epoch + 2;
  Require(sblr::EncodeSblrOptimizerStatsDropResultV1(invalid_result).empty(),
          "OSDR with non-successor statistics epoch was encoded");
  invalid_result = result;
  invalid_result.cache_invalidation_generation = result.statistics_epoch - 1;
  Require(sblr::EncodeSblrOptimizerStatsDropResultV1(invalid_result).empty(),
          "OSDR without matching cache invalidation was encoded");

  malformed = result_bytes;
  malformed[12] = 1;
  RefuseResult(malformed, "OSDR with nonzero header flags was accepted");
  malformed = result_bytes;
  malformed[136] ^= 1;
  RefuseResult(malformed, "OSDR material evidence tamper was accepted");
  malformed = result_bytes;
  malformed[168] ^= 1;
  RefuseResult(malformed, "OSDR executor evidence tamper was accepted");
  malformed = result_bytes;
  std::fill(malformed.begin() + 208, malformed.begin() + 216, 0);
  RefuseResult(malformed, "OSDR without publication barrier was accepted");
  malformed = result_bytes;
  malformed.pop_back();
  RefuseResult(malformed, "truncated OSDR was accepted");
  malformed = result_bytes;
  malformed.push_back(0);
  RefuseResult(malformed, "OSDR trailing bytes were accepted");
  return EXIT_SUCCESS;
}
