// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_query_explain_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "CSC-TEST-003606: " << message << '\n';
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
  sblr::SblrQueryExplainRequestV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrQueryExplainRequestV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseDescriptor(std::vector<std::uint8_t> bytes,
                      std::string_view message) {
  sblr::SblrQueryExplainDescriptorV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrQueryExplainDescriptorV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseResult(std::vector<std::uint8_t> bytes,
                  std::string_view message) {
  sblr::SblrQueryExplainResultV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrQueryExplainResultV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

}  // namespace

int main() {
  sblr::SblrQueryExplainRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 7;
  request.catalog_generation = 11;
  request.security_epoch = 13;
  request.resource_epoch = 17;
  const auto request_bytes = sblr::EncodeSblrQueryExplainRequestV1(request);
  sblr::SblrQueryExplainRequestV1 decoded_request;
  std::string detail;
  Require(request_bytes.size() == 64 &&
              sblr::DecodeSblrQueryExplainRequestV1(
                  request_bytes.data(), request_bytes.size(), &decoded_request,
                  &detail) &&
              sblr::EncodeSblrQueryExplainRequestV1(decoded_request) ==
                  request_bytes,
          "canonical SBEQ did not round trip byte-identically");

  auto malformed = request_bytes;
  malformed[0] = 'X';
  RefuseRequest(malformed, "SBEQ with wrong magic was accepted");
  malformed = request_bytes;
  malformed[4] = 2;
  RefuseRequest(malformed, "SBEQ with wrong version was accepted");
  malformed = request_bytes;
  malformed[12] = 1;
  RefuseRequest(malformed, "SBEQ with nonzero flags was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 16, malformed.begin() + 32, 0);
  RefuseRequest(malformed, "SBEQ with nil receipt was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 32, malformed.begin() + 40, 0);
  RefuseRequest(malformed, "SBEQ with zero occurrence was accepted");
  malformed = request_bytes;
  malformed.pop_back();
  RefuseRequest(malformed, "truncated SBEQ was accepted");
  malformed = request_bytes;
  malformed.push_back(0);
  RefuseRequest(malformed, "SBEQ trailing bytes were accepted");

  sblr::SblrQueryExplainDescriptorV1 descriptor;
  descriptor.explain_uuid = Bytes<16>(21);
  descriptor.statement_receipt_uuid = request.statement_receipt_uuid;
  descriptor.query_snapshot_uuid = Bytes<16>(41);
  descriptor.catalog_snapshot_uuid = Bytes<16>(61);
  descriptor.catalog_generation = request.catalog_generation;
  descriptor.security_context_uuid = Bytes<16>(81);
  descriptor.policy_snapshot_uuid = Bytes<16>(101);
  descriptor.policy_generation = 19;
  descriptor.plan_policy_uuid = Bytes<16>(121);
  descriptor.resource_budget_uuid = Bytes<16>(141);
  descriptor.resource_budget_generation = request.resource_epoch;
  descriptor.redaction_profile_uuid = Bytes<16>(161);
  descriptor.verbose = true;
  descriptor.format = 2;
  descriptor.canonical_query_sblr_bytes = {0x53, 0x42, 0x4c, 0x52, 1, 2, 3};
  descriptor.executor_availability_generation = 23;
  descriptor.parser_package_uuid = Bytes<16>(181);
  descriptor.language_profile_uuid = Bytes<16>(201);
  const auto descriptor_bytes =
      sblr::EncodeSblrQueryExplainDescriptorV1(descriptor);
  sblr::SblrQueryExplainDescriptorV1 decoded_descriptor;
  Require(descriptor_bytes.size() == 327 &&
              sblr::DecodeSblrQueryExplainDescriptorV1(
                  descriptor_bytes.data(), descriptor_bytes.size(),
                  &decoded_descriptor, &detail) &&
              sblr::EncodeSblrQueryExplainDescriptorV1(decoded_descriptor) ==
                  descriptor_bytes &&
              decoded_descriptor.verbose && decoded_descriptor.format == 2 &&
              decoded_descriptor.canonical_query_sblr_bytes ==
                  descriptor.canonical_query_sblr_bytes,
          "canonical SBXD did not round trip byte-identically");

  auto invalid_descriptor = descriptor;
  invalid_descriptor.format = 3;
  Require(sblr::EncodeSblrQueryExplainDescriptorV1(invalid_descriptor).empty(),
          "SBXD with unknown format was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.parameter_set_generation = 1;
  Require(sblr::EncodeSblrQueryExplainDescriptorV1(invalid_descriptor).empty(),
          "SBXD with incomplete parameter-set identity was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.canonical_query_sblr_bytes.clear();
  Require(sblr::EncodeSblrQueryExplainDescriptorV1(invalid_descriptor).empty(),
          "SBXD with empty canonical query was encoded");

  malformed = descriptor_bytes;
  malformed[12] = 1;
  RefuseDescriptor(malformed, "SBXD with nonzero flags was accepted");
  malformed = descriptor_bytes;
  malformed[209] = 3;
  RefuseDescriptor(malformed, "SBXD with unknown format was accepted");
  malformed = descriptor_bytes;
  malformed[216] ^= 1;
  RefuseDescriptor(malformed, "SBXD query hash tamper was accepted");
  malformed = descriptor_bytes;
  malformed[256] ^= 1;
  RefuseDescriptor(malformed, "SBXD descriptor hash tamper was accepted");
  malformed = descriptor_bytes;
  malformed.back() ^= 1;
  RefuseDescriptor(malformed, "SBXD query-body tamper was accepted");
  malformed = descriptor_bytes;
  malformed.pop_back();
  RefuseDescriptor(malformed, "truncated SBXD was accepted");
  malformed = descriptor_bytes;
  malformed.push_back(0);
  RefuseDescriptor(malformed, "SBXD trailing bytes were accepted");

  sblr::SblrQueryExplainResultV1 result;
  result.explain_uuid = descriptor.explain_uuid;
  result.plan_uuid = Bytes<16>(31);
  result.plan_descriptor_uuid = Bytes<16>(51);
  result.plan_descriptor_generation = 29;
  result.query_snapshot_uuid = descriptor.query_snapshot_uuid;
  result.deterministic_snapshot_uuid = Bytes<16>(71);
  result.catalog_generation = descriptor.catalog_generation;
  result.policy_generation = descriptor.policy_generation;
  result.redaction_profile_uuid = descriptor.redaction_profile_uuid;
  result.plan_material_sha256 = Bytes<32>(91);
  result.executor_evidence_sha256 = Bytes<32>(131);
  result.publication_barrier_uuid = Bytes<16>(171);
  result.result_evidence_uuid = Bytes<16>(211);
  const auto result_bytes = sblr::EncodeSblrQueryExplainResultV1(result);
  sblr::SblrQueryExplainResultV1 decoded_result;
  Require(result_bytes.size() == 256 &&
              sblr::DecodeSblrQueryExplainResultV1(
                  result_bytes.data(), result_bytes.size(), &decoded_result,
                  &detail) &&
              sblr::EncodeSblrQueryExplainResultV1(decoded_result) ==
                  result_bytes,
          "canonical SBXR did not round trip byte-identically");

  auto invalid_result = result;
  invalid_result.completion_state = 0;
  Require(sblr::EncodeSblrQueryExplainResultV1(invalid_result).empty(),
          "incomplete SBXR was encoded");
  invalid_result = result;
  invalid_result.plan_state = 2;
  Require(sblr::EncodeSblrQueryExplainResultV1(invalid_result).empty(),
          "SBXR with unknown plan state was encoded");

  malformed = result_bytes;
  malformed[138] = 1;
  RefuseResult(malformed, "SBXR with nonzero reserved bytes was accepted");
  malformed = result_bytes;
  std::fill(malformed.begin() + 140, malformed.begin() + 172, 0);
  RefuseResult(malformed, "SBXR with nil plan hash was accepted");
  malformed = result_bytes;
  malformed.pop_back();
  RefuseResult(malformed, "truncated SBXR was accepted");
  malformed = result_bytes;
  malformed.push_back(0);
  RefuseResult(malformed, "SBXR trailing bytes were accepted");
  return EXIT_SUCCESS;
}
