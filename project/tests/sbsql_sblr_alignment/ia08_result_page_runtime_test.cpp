// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_result_page_runtime.hpp"

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
  std::cerr << "CSC-TEST-003598: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <std::size_t N>
std::array<std::uint8_t, N> Bytes(std::uint8_t seed) {
  std::array<std::uint8_t, N> out{};
  for (std::size_t i = 0; i < N; ++i) {
    out[i] = static_cast<std::uint8_t>(seed + i);
  }
  return out;
}

void RequireRequestRefusal(std::vector<std::uint8_t> bytes,
                           std::string_view message) {
  sblr::SblrResultPageRequestV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrResultPageRequestV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RequireDescriptorRefusal(std::vector<std::uint8_t> bytes,
                              std::string_view message) {
  sblr::SblrResultPageDescriptorV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrResultPageDescriptorV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RequireResultRefusal(std::vector<std::uint8_t> bytes,
                          std::string_view message) {
  sblr::SblrResultPageResultV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrResultPageResultV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

}  // namespace

int main() {
  sblr::SblrResultPageRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 1;
  request.catalog_generation = 2;
  request.security_epoch = 3;
  request.resource_epoch = 4;
  const auto request_bytes = sblr::EncodeSblrResultPageRequestV1(request);
  sblr::SblrResultPageRequestV1 decoded_request;
  std::string detail;
  Require(request_bytes.size() == 64 &&
              sblr::DecodeSblrResultPageRequestV1(
                  request_bytes.data(), request_bytes.size(), &decoded_request,
                  &detail) &&
              sblr::EncodeSblrResultPageRequestV1(decoded_request) ==
                  request_bytes,
          "canonical SRPQ did not round trip byte-identically");

  auto malformed = request_bytes;
  malformed[0] = 'X';
  RequireRequestRefusal(malformed, "SRPQ with the wrong magic was accepted");
  malformed = request_bytes;
  malformed[4] = 2;
  RequireRequestRefusal(malformed,
                        "SRPQ with the wrong version was accepted");
  malformed = request_bytes;
  malformed[12] = 1;
  RequireRequestRefusal(malformed,
                        "SRPQ with nonzero reserved flags was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 16, malformed.begin() + 32, 0);
  RequireRequestRefusal(malformed,
                        "SRPQ with a nil statement receipt was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 32, malformed.begin() + 40, 0);
  RequireRequestRefusal(malformed,
                        "SRPQ with a zero occurrence was accepted");
  malformed = request_bytes;
  malformed.pop_back();
  RequireRequestRefusal(malformed, "truncated SRPQ was accepted");
  malformed = request_bytes;
  malformed.push_back(0);
  RequireRequestRefusal(malformed, "SRPQ trailing bytes were accepted");

  sblr::SblrResultPageDescriptorV1 descriptor;
  descriptor.cursor_uuid = Bytes<16>(21);
  descriptor.cursor_generation = 5;
  descriptor.statement_receipt_uuid = request.statement_receipt_uuid;
  descriptor.result_set_handle_uuid = Bytes<16>(41);
  descriptor.result_set_handle_generation = 6;
  descriptor.snapshot_uuid = Bytes<16>(61);
  descriptor.row_descriptor_uuid = Bytes<16>(81);
  descriptor.row_descriptor_generation = 7;
  descriptor.page_number = 0;
  descriptor.first_row_offset = 0;
  descriptor.maximum_rows = 64;
  descriptor.maximum_bytes = 1U << 20U;
  descriptor.redaction_profile_uuid = Bytes<16>(101);
  descriptor.redaction_generation = 8;
  descriptor.policy_snapshot_uuid = Bytes<16>(121);
  descriptor.policy_generation = 9;
  descriptor.resource_budget_uuid = Bytes<16>(141);
  descriptor.resource_budget_generation = 10;
  descriptor.executor_availability_generation = 11;
  const auto descriptor_bytes =
      sblr::EncodeSblrResultPageDescriptorV1(descriptor);
  sblr::SblrResultPageDescriptorV1 decoded_descriptor;
  Require(descriptor_bytes.size() == 288 &&
              sblr::DecodeSblrResultPageDescriptorV1(
                  descriptor_bytes.data(), descriptor_bytes.size(),
                  &decoded_descriptor, &detail) &&
              sblr::EncodeSblrResultPageDescriptorV1(decoded_descriptor) ==
                  descriptor_bytes,
          "canonical first-page SRPD did not round trip byte-identically");

  auto continuation_descriptor = descriptor;
  continuation_descriptor.page_number = 1;
  continuation_descriptor.first_row_offset = 64;
  continuation_descriptor.continuation_uuid = Bytes<16>(161);
  continuation_descriptor.continuation_generation = 12;
  const auto continuation_bytes =
      sblr::EncodeSblrResultPageDescriptorV1(continuation_descriptor);
  Require(continuation_bytes.size() == 288 &&
              sblr::DecodeSblrResultPageDescriptorV1(
                  continuation_bytes.data(), continuation_bytes.size(),
                  &decoded_descriptor, &detail) &&
              decoded_descriptor.page_number == 1 &&
              decoded_descriptor.first_row_offset == 64,
          "canonical continuation SRPD was refused");

  auto invalid_descriptor = descriptor;
  invalid_descriptor.maximum_rows = 0;
  Require(sblr::EncodeSblrResultPageDescriptorV1(invalid_descriptor).empty(),
          "SRPD with a zero row limit was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.maximum_bytes = 0;
  Require(sblr::EncodeSblrResultPageDescriptorV1(invalid_descriptor).empty(),
          "SRPD with a zero byte limit was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.page_number = 1;
  Require(sblr::EncodeSblrResultPageDescriptorV1(invalid_descriptor).empty(),
          "continuation SRPD without a continuation identity was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.continuation_uuid = Bytes<16>(181);
  invalid_descriptor.continuation_generation = 13;
  Require(sblr::EncodeSblrResultPageDescriptorV1(invalid_descriptor).empty(),
          "first-page SRPD with a continuation identity was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.continuation_generation = 13;
  Require(sblr::EncodeSblrResultPageDescriptorV1(invalid_descriptor).empty(),
          "SRPD with an incomplete continuation pair was encoded");

  malformed = descriptor_bytes;
  malformed[0] = 'X';
  RequireDescriptorRefusal(malformed,
                           "SRPD with the wrong magic was accepted");
  malformed = descriptor_bytes;
  malformed[12] = 1;
  RequireDescriptorRefusal(malformed,
                           "SRPD with nonzero reserved flags was accepted");
  malformed = descriptor_bytes;
  malformed[248] ^= 1;
  RequireDescriptorRefusal(malformed,
                           "SRPD descriptor-evidence tamper was accepted");
  malformed = descriptor_bytes;
  malformed.pop_back();
  RequireDescriptorRefusal(malformed, "truncated SRPD was accepted");
  malformed = descriptor_bytes;
  malformed.push_back(0);
  RequireDescriptorRefusal(malformed, "SRPD trailing bytes were accepted");

  sblr::SblrResultPageResultV1 result;
  result.cursor_uuid = descriptor.cursor_uuid;
  result.cursor_generation = descriptor.cursor_generation;
  result.completion_state = 1;
  result.terminal_state = 1;
  result.result_set_handle_uuid = descriptor.result_set_handle_uuid;
  result.result_set_handle_generation = descriptor.result_set_handle_generation;
  result.row_descriptor_uuid = descriptor.row_descriptor_uuid;
  result.row_descriptor_generation = descriptor.row_descriptor_generation;
  result.returned_row_count = 4;
  result.next_row_offset = 4;
  result.redaction_profile_uuid = descriptor.redaction_profile_uuid;
  result.result_material_sha256 = Bytes<32>(31);
  result.executor_evidence_sha256 = Bytes<32>(71);
  result.executor_availability_generation =
      descriptor.executor_availability_generation;
  result.publication_barrier_uuid = Bytes<16>(201);
  result.result_evidence_uuid = Bytes<16>(221);
  const auto result_bytes = sblr::EncodeSblrResultPageResultV1(result);
  sblr::SblrResultPageResultV1 decoded_result;
  Require(result_bytes.size() == 256 &&
              sblr::DecodeSblrResultPageResultV1(
                  result_bytes.data(), result_bytes.size(), &decoded_result,
                  &detail) &&
              sblr::EncodeSblrResultPageResultV1(decoded_result) ==
                  result_bytes,
          "canonical terminal SRPR did not round trip byte-identically");

  auto continued_result = result;
  continued_result.terminal_state = 0;
  continued_result.next_continuation_uuid = Bytes<16>(241);
  continued_result.next_continuation_generation = 14;
  const auto continued_result_bytes =
      sblr::EncodeSblrResultPageResultV1(continued_result);
  Require(continued_result_bytes.size() == 256 &&
              sblr::DecodeSblrResultPageResultV1(
                  continued_result_bytes.data(), continued_result_bytes.size(),
                  &decoded_result, &detail),
          "canonical nonterminal SRPR was refused");

  auto invalid_result = result;
  invalid_result.completion_state = 0;
  Require(sblr::EncodeSblrResultPageResultV1(invalid_result).empty(),
          "incomplete SRPR was encoded");
  invalid_result = result;
  invalid_result.terminal_state = 2;
  Require(sblr::EncodeSblrResultPageResultV1(invalid_result).empty(),
          "SRPR with an unknown terminal state was encoded");
  invalid_result = result;
  invalid_result.terminal_state = 0;
  Require(sblr::EncodeSblrResultPageResultV1(invalid_result).empty(),
          "nonterminal SRPR without continuation authority was encoded");
  invalid_result = result;
  invalid_result.next_continuation_uuid = Bytes<16>(17);
  invalid_result.next_continuation_generation = 2;
  Require(sblr::EncodeSblrResultPageResultV1(invalid_result).empty(),
          "terminal SRPR with continuation authority was encoded");

  malformed = result_bytes;
  malformed[42] = 1;
  RequireResultRefusal(malformed,
                       "SRPR with nonzero reserved bytes was accepted");
  malformed = result_bytes;
  malformed[152] = 0;
  std::fill(malformed.begin() + 153, malformed.begin() + 184, 0);
  RequireResultRefusal(malformed,
                       "SRPR with a zero material hash was accepted");
  malformed = result_bytes;
  malformed.pop_back();
  RequireResultRefusal(malformed, "truncated SRPR was accepted");
  malformed = result_bytes;
  malformed.push_back(0);
  RequireResultRefusal(malformed, "SRPR trailing bytes were accepted");
  return EXIT_SUCCESS;
}
