// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_parse_text_runtime.hpp"
#include "wire/parser_server_ipc/sbps_statement_management_bind_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace bind = scratchbird::wire::sbps_statement_management;
namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "CSC-TEST-003626: " << message << '\n';
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
  sblr::SblrParseTextRequestV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrParseTextRequestV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseDescriptor(std::vector<std::uint8_t> bytes,
                      std::string_view message) {
  sblr::SblrParseTextDescriptorV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrParseTextDescriptorV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseResult(std::vector<std::uint8_t> bytes,
                  std::string_view message) {
  sblr::SblrParseTextResultV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrParseTextResultV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

}  // namespace

int main() {
  std::string detail;
  sblr::SblrParseTextRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 7;
  request.catalog_generation = 11;
  request.security_epoch = 13;
  request.resource_epoch = 17;
  const auto request_bytes = sblr::EncodeSblrParseTextRequestV1(request);
  sblr::SblrParseTextRequestV1 decoded_request;
  Require(request_bytes.size() == sblr::kSblrParseTextRequestBytes &&
              sblr::DecodeSblrParseTextRequestV1(
                  request_bytes.data(), request_bytes.size(),
                  &decoded_request, &detail) &&
              sblr::EncodeSblrParseTextRequestV1(decoded_request) ==
                  request_bytes,
          "canonical SBTQ did not round trip byte-identically");

  auto malformed = request_bytes;
  malformed[0] = 'X';
  RefuseRequest(malformed, "SBTQ with wrong magic was accepted");
  malformed = request_bytes;
  malformed[4] = 2;
  RefuseRequest(malformed, "SBTQ with wrong version was accepted");
  malformed = request_bytes;
  malformed[12] = 1;
  RefuseRequest(malformed, "SBTQ with nonzero flags was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 16, malformed.begin() + 32, 0);
  RefuseRequest(malformed, "SBTQ with nil receipt was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 32, malformed.begin() + 40, 0);
  RefuseRequest(malformed, "SBTQ with zero occurrence was accepted");
  malformed = request_bytes;
  malformed.pop_back();
  RefuseRequest(malformed, "truncated SBTQ was accepted");
  malformed = request_bytes;
  malformed.push_back(0);
  RefuseRequest(malformed, "SBTQ trailing bytes were accepted");

  const std::vector<std::uint8_t> canonical_nested_sblr{
      0x53, 0x42, 0x4c, 0x52, 1, 0, 1, 0, 0x19, 0x27, 0x31};
  sblr::SblrParseTextDescriptorV1 descriptor;
  descriptor.parse_uuid = Bytes<16>(21);
  descriptor.statement_receipt_uuid = request.statement_receipt_uuid;
  descriptor.language_profile_uuid = Bytes<16>(41);
  descriptor.language_profile_generation = 19;
  descriptor.parser_package_uuid = Bytes<16>(61);
  descriptor.parser_package_version_major = 1;
  descriptor.parser_package_version_minor = 2;
  descriptor.parser_package_version_patch = 3;
  descriptor.catalog_snapshot_uuid = Bytes<16>(81);
  descriptor.catalog_generation = request.catalog_generation;
  descriptor.security_context_uuid = Bytes<16>(101);
  descriptor.security_epoch = request.security_epoch;
  descriptor.resource_epoch = request.resource_epoch;
  descriptor.input_byte_count = 9;
  descriptor.canonical_input_sha256 =
      sblr::SblrParseTextInputSha256V1("SELECT 1;");
  descriptor.requested_maximum_bytes = 4096;
  descriptor.requested_maximum_depth = 64;
  descriptor.extension_capability = 0;
  descriptor.executor_availability_generation = 23;
  descriptor.canonical_sblr_bytes = canonical_nested_sblr;
  const auto descriptor_bytes =
      sblr::EncodeSblrParseTextDescriptorV1(descriptor);
  sblr::SblrParseTextDescriptorV1 decoded_descriptor;
  Require(descriptor_bytes.size() ==
                  sblr::kSblrParseTextDescriptorPrefixBytes +
                      canonical_nested_sblr.size() &&
              sblr::DecodeSblrParseTextDescriptorV1(
                  descriptor_bytes.data(), descriptor_bytes.size(),
                  &decoded_descriptor, &detail) &&
              decoded_descriptor.canonical_sblr_bytes ==
                  canonical_nested_sblr &&
              sblr::EncodeSblrParseTextDescriptorV1(decoded_descriptor) ==
                  descriptor_bytes,
          "canonical SPTD did not round trip byte-identically");

  auto invalid_descriptor = descriptor;
  invalid_descriptor.parse_uuid = {};
  Require(sblr::EncodeSblrParseTextDescriptorV1(invalid_descriptor).empty(),
          "SPTD with nil parse identity was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.input_byte_count = 4097;
  Require(sblr::EncodeSblrParseTextDescriptorV1(invalid_descriptor).empty(),
          "SPTD whose input exceeded its bound was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.extension_capability = 2;
  Require(sblr::EncodeSblrParseTextDescriptorV1(invalid_descriptor).empty(),
          "SPTD with an unknown extension capability was encoded");

  malformed = descriptor_bytes;
  malformed[195] = 1;
  RefuseDescriptor(malformed, "SPTD with nonzero reserved byte was accepted");
  malformed = descriptor_bytes;
  malformed[204] ^= 1;
  RefuseDescriptor(malformed, "SPTD descriptor-evidence tamper was accepted");
  malformed = descriptor_bytes;
  malformed.back() ^= 1;
  RefuseDescriptor(malformed, "SPTD nested-SBLR tamper was accepted");
  malformed = descriptor_bytes;
  malformed.pop_back();
  RefuseDescriptor(malformed, "truncated SPTD was accepted");
  malformed = descriptor_bytes;
  malformed.push_back(0);
  RefuseDescriptor(malformed, "SPTD trailing bytes were accepted");

  sblr::SblrParseTextResultV1 result;
  result.parse_uuid = descriptor.parse_uuid;
  result.statement_receipt_uuid = descriptor.statement_receipt_uuid;
  result.language_profile_uuid = descriptor.language_profile_uuid;
  result.language_profile_generation = descriptor.language_profile_generation;
  result.parser_package_uuid = descriptor.parser_package_uuid;
  result.catalog_generation = descriptor.catalog_generation;
  result.security_epoch = descriptor.security_epoch;
  result.resource_epoch = descriptor.resource_epoch;
  result.canonical_sblr_bytes = descriptor.canonical_sblr_bytes;
  result.status = 1;
  result.publication_barrier = 1;
  result.parse_evidence_uuid = Bytes<16>(121);
  result.executor_availability_generation =
      descriptor.executor_availability_generation;
  const auto result_bytes = sblr::EncodeSblrParseTextResultV1(result);
  sblr::SblrParseTextResultV1 decoded_result;
  Require(result_bytes.size() == sblr::kSblrParseTextResultPrefixBytes +
                                     canonical_nested_sblr.size() &&
              sblr::DecodeSblrParseTextResultV1(
                  result_bytes.data(), result_bytes.size(), &decoded_result,
                  &detail) &&
              decoded_result.canonical_sblr_bytes == canonical_nested_sblr &&
              sblr::EncodeSblrParseTextResultV1(decoded_result) == result_bytes,
          "canonical SPTR did not round trip byte-identically");

  malformed = result_bytes;
  malformed[152] = 2;
  RefuseResult(malformed, "SPTR with unknown status was accepted");
  malformed = result_bytes;
  malformed[120] ^= 1;
  RefuseResult(malformed, "SPTR nested-SBLR hash tamper was accepted");
  malformed = result_bytes;
  malformed[172] ^= 1;
  RefuseResult(malformed, "SPTR result-evidence tamper was accepted");
  malformed = result_bytes;
  malformed[244] = 1;
  RefuseResult(malformed, "SPTR with nonzero reserved byte was accepted");
  malformed = result_bytes;
  malformed.back() ^= 1;
  RefuseResult(malformed, "SPTR nested-SBLR body tamper was accepted");
  malformed = result_bytes;
  malformed.push_back(0);
  RefuseResult(malformed, "SPTR trailing bytes were accepted");

  bind::ParseTextBindRequestV1 bind_request;
  bind_request.authenticated_receipt_uuid = request.statement_receipt_uuid;
  bind_request.occurrence = request.occurrence;
  bind_request.language_profile_id = "sbsql.builtin.recovery.en";
  bind_request.canonical_input_utf8 = "SELECT 1;";
  bind_request.requested_maximum_bytes = 4096;
  bind_request.requested_maximum_depth = 64;
  bind_request.allow_donor_extensions = false;
  bind_request.canonical_container_bytes = {1, 2, 3, 4};
  bind_request.canonical_execution_envelope_bytes = {5, 6, 7, 8};
  std::vector<std::uint8_t> bind_bytes;
  Require(bind::EncodeParseTextBindRequestV1(
              bind_request, &bind_bytes, &detail),
          "canonical PTBQ encoding failed");
  bind::ParseTextBindRequestV1 decoded_bind;
  Require(bind::DecodeParseTextBindRequestV1(
              bind_bytes.data(), bind_bytes.size(), &decoded_bind, &detail) &&
              decoded_bind.language_profile_id ==
                  bind_request.language_profile_id &&
              decoded_bind.canonical_input_utf8 == "SELECT 1;" &&
              decoded_bind.canonical_container_bytes ==
                  bind_request.canonical_container_bytes &&
              decoded_bind.canonical_execution_envelope_bytes ==
                  bind_request.canonical_execution_envelope_bytes &&
              bind::ParseTextBindRequestEvidenceV1(decoded_bind) ==
                  decoded_bind.request_evidence_sha256,
          "canonical PTBQ did not round trip with exact body evidence");

  auto invalid_bind = bind_request;
  invalid_bind.canonical_input_utf8 = std::string("bad\0text", 8);
  std::vector<std::uint8_t> rejected_bind;
  Require(!bind::EncodeParseTextBindRequestV1(
              invalid_bind, &rejected_bind, &detail) &&
              rejected_bind.empty(),
          "PTBQ with embedded NUL was encoded");
  invalid_bind = bind_request;
  invalid_bind.canonical_input_utf8 = std::string("\xef\xbb\xbfSELECT 1;", 12);
  Require(!bind::EncodeParseTextBindRequestV1(
              invalid_bind, &rejected_bind, &detail),
          "PTBQ with a BOM was encoded");
  invalid_bind = bind_request;
  invalid_bind.requested_maximum_bytes = 8;
  Require(!bind::EncodeParseTextBindRequestV1(
              invalid_bind, &rejected_bind, &detail),
          "PTBQ whose input exceeded its requested bound was encoded");

  auto malformed_bind = bind_bytes;
  malformed_bind[47] = 1;
  Require(!bind::DecodeParseTextBindRequestV1(
              malformed_bind.data(), malformed_bind.size(), &decoded_bind,
              &detail),
          "PTBQ with nonzero reserved byte was accepted");
  malformed_bind = bind_bytes;
  malformed_bind[64] ^= 1;
  Require(!bind::DecodeParseTextBindRequestV1(
              malformed_bind.data(), malformed_bind.size(), &decoded_bind,
              &detail),
          "PTBQ input-hash tamper was accepted");
  malformed_bind = bind_bytes;
  malformed_bind[160] ^= 1;
  Require(!bind::DecodeParseTextBindRequestV1(
              malformed_bind.data(), malformed_bind.size(), &decoded_bind,
              &detail),
          "PTBQ request-evidence tamper was accepted");
  malformed_bind = bind_bytes;
  malformed_bind.push_back(0);
  Require(!bind::DecodeParseTextBindRequestV1(
              malformed_bind.data(), malformed_bind.size(), &decoded_bind,
              &detail),
          "PTBQ trailing bytes were accepted");

  bind::ParseTextBindAckV1 bind_ack;
  bind_ack.authenticated_receipt_uuid = request.statement_receipt_uuid;
  bind_ack.occurrence = request.occurrence;
  bind_ack.binding_uuid = Bytes<16>(141);
  bind_ack.binding_generation = 1;
  bind_ack.parse_uuid = descriptor.parse_uuid;
  bind_ack.descriptor_sha256 = decoded_descriptor.descriptor_sha256;
  bind_ack.canonical_input_sha256 = decoded_bind.canonical_input_sha256;
  bind_ack.request_evidence_sha256 = decoded_bind.request_evidence_sha256;
  std::vector<std::uint8_t> ack_bytes;
  Require(bind::EncodeParseTextBindAckV1(bind_ack, &ack_bytes, &detail),
          "canonical PTBA encoding failed");
  bind::ParseTextBindAckV1 decoded_ack;
  Require(bind::DecodeParseTextBindAckV1(
              ack_bytes.data(), ack_bytes.size(), &decoded_ack, &detail) &&
              bind::ParseTextBindAckEvidenceV1(decoded_ack) ==
                  decoded_ack.acknowledgement_evidence_sha256,
          "canonical PTBA did not round trip with exact evidence");
  auto malformed_ack = ack_bytes;
  malformed_ack[176] ^= 1;
  Require(!bind::DecodeParseTextBindAckV1(
              malformed_ack.data(), malformed_ack.size(), &decoded_ack,
              &detail),
          "PTBA acknowledgement-evidence tamper was accepted");
  malformed_ack = ack_bytes;
  malformed_ack.push_back(0);
  Require(!bind::DecodeParseTextBindAckV1(
              malformed_ack.data(), malformed_ack.size(), &decoded_ack,
              &detail),
          "PTBA trailing bytes were accepted");

  return EXIT_SUCCESS;
}
