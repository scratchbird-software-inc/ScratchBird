// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_database_attach_runtime.hpp"
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
  std::cerr << "CSC-TEST-003634: " << message << '\n';
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
  sblr::SblrDatabaseAttachRequestV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrDatabaseAttachRequestV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseDescriptor(std::vector<std::uint8_t> bytes,
                      std::string_view message) {
  sblr::SblrDatabaseAttachDescriptorV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrDatabaseAttachDescriptorV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseResult(std::vector<std::uint8_t> bytes,
                  std::string_view message) {
  sblr::SblrDatabaseAttachResultV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrDatabaseAttachResultV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

}  // namespace

int main() {
  std::string detail;
  sblr::SblrDatabaseAttachRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 7;
  request.catalog_generation = 11;
  request.security_epoch = 13;
  request.resource_epoch = 17;
  const auto request_bytes = sblr::EncodeSblrDatabaseAttachRequestV1(request);
  sblr::SblrDatabaseAttachRequestV1 decoded_request;
  Require(request_bytes.size() == sblr::kSblrDatabaseAttachRequestBytes &&
              sblr::DecodeSblrDatabaseAttachRequestV1(
                  request_bytes.data(), request_bytes.size(),
                  &decoded_request, &detail) &&
              sblr::EncodeSblrDatabaseAttachRequestV1(decoded_request) ==
                  request_bytes,
          "canonical SBAQ did not round trip byte-identically");

  auto malformed = request_bytes;
  malformed[0] = 'X';
  RefuseRequest(malformed, "SBAQ with wrong magic was accepted");
  malformed = request_bytes;
  malformed[4] = 2;
  RefuseRequest(malformed, "SBAQ with wrong version was accepted");
  malformed = request_bytes;
  malformed[12] = 1;
  RefuseRequest(malformed, "SBAQ with unknown flags was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 16, malformed.begin() + 32, 0);
  RefuseRequest(malformed, "SBAQ with nil receipt was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 32, malformed.begin() + 40, 0);
  RefuseRequest(malformed, "SBAQ with zero occurrence was accepted");
  malformed = request_bytes;
  malformed.pop_back();
  RefuseRequest(malformed, "truncated SBAQ was accepted");
  malformed = request_bytes;
  malformed.push_back(0);
  RefuseRequest(malformed, "SBAQ trailing bytes were accepted");

  sblr::SblrDatabaseAttachDescriptorV1 descriptor;
  descriptor.attach_uuid = Bytes<16>(21);
  descriptor.statement_receipt_uuid = request.statement_receipt_uuid;
  descriptor.storage_uuid = Bytes<16>(41);
  descriptor.alias_uuid = Bytes<16>(61);
  descriptor.database_uuid = Bytes<16>(81);
  descriptor.catalog_snapshot_uuid = Bytes<16>(101);
  descriptor.catalog_generation = request.catalog_generation;
  descriptor.security_context_uuid = Bytes<16>(121);
  descriptor.policy_snapshot_uuid = Bytes<16>(141);
  descriptor.policy_generation = 19;
  descriptor.transaction_uuid = Bytes<16>(161);
  descriptor.transaction_generation = 23;
  descriptor.mode = 1;
  descriptor.alias_scope = 1;
  descriptor.executor_availability_generation = 29;
  descriptor.storage_alias_binding_sha256 =
      sblr::SblrDatabaseAttachBindingSha256V1(
          descriptor.storage_uuid, descriptor.alias_uuid, descriptor.mode,
          descriptor.alias_scope);
  const auto descriptor_bytes =
      sblr::EncodeSblrDatabaseAttachDescriptorV1(descriptor);
  sblr::SblrDatabaseAttachDescriptorV1 decoded_descriptor;
  Require(descriptor_bytes.size() ==
                  sblr::kSblrDatabaseAttachDescriptorBytes &&
              sblr::DecodeSblrDatabaseAttachDescriptorV1(
                  descriptor_bytes.data(), descriptor_bytes.size(),
                  &decoded_descriptor, &detail) &&
              decoded_descriptor.storage_alias_binding_sha256 ==
                  descriptor.storage_alias_binding_sha256 &&
              sblr::EncodeSblrDatabaseAttachDescriptorV1(
                  decoded_descriptor) == descriptor_bytes,
          "canonical SADD did not round trip byte-identically");

  auto invalid_descriptor = descriptor;
  invalid_descriptor.attach_uuid = {};
  Require(sblr::EncodeSblrDatabaseAttachDescriptorV1(
              invalid_descriptor).empty(),
          "SADD with nil attachment identity was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.mode = 3;
  Require(sblr::EncodeSblrDatabaseAttachDescriptorV1(
              invalid_descriptor).empty(),
          "SADD with unknown access mode was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.storage_alias_binding_sha256[0] ^= 1;
  Require(sblr::EncodeSblrDatabaseAttachDescriptorV1(
              invalid_descriptor).empty(),
          "SADD with mismatched storage/alias binding was encoded");
  malformed = descriptor_bytes;
  malformed[186] = 1;
  RefuseDescriptor(malformed, "SADD with nonzero reserved bytes was accepted");
  malformed = descriptor_bytes;
  malformed[196] ^= 1;
  RefuseDescriptor(malformed, "SADD descriptor evidence tamper was accepted");
  malformed = descriptor_bytes;
  malformed[228] ^= 1;
  RefuseDescriptor(malformed, "SADD binding evidence tamper was accepted");
  malformed = descriptor_bytes;
  malformed[260] = 1;
  RefuseDescriptor(malformed, "SADD with nonzero tail was accepted");

  sblr::SblrDatabaseAttachResultV1 result;
  result.attach_uuid = descriptor.attach_uuid;
  result.database_uuid = descriptor.database_uuid;
  result.alias_uuid = descriptor.alias_uuid;
  result.database_generation = 1;
  result.catalog_epoch_uuid = Bytes<16>(181);
  result.catalog_generation = descriptor.catalog_generation;
  result.status = 1;
  result.lifecycle_state = 1;
  result.publication_barrier = 1;
  result.attachment_evidence_uuid = Bytes<16>(201);
  const auto result_bytes = sblr::EncodeSblrDatabaseAttachResultV1(result);
  sblr::SblrDatabaseAttachResultV1 decoded_result;
  Require(result_bytes.size() == sblr::kSblrDatabaseAttachResultBytes &&
              sblr::DecodeSblrDatabaseAttachResultV1(
                  result_bytes.data(), result_bytes.size(), &decoded_result,
                  &detail) &&
              decoded_result.status == 1 &&
              sblr::EncodeSblrDatabaseAttachResultV1(decoded_result) ==
                  result_bytes,
          "canonical SBAR did not round trip byte-identically");
  auto invalid_result = result;
  invalid_result.status = 3;
  Require(sblr::EncodeSblrDatabaseAttachResultV1(invalid_result).empty(),
          "SBAR with unknown status was encoded");
  malformed = result_bytes;
  malformed[99] = 1;
  RefuseResult(malformed, "SBAR with nonzero reserved byte was accepted");
  malformed = result_bytes;
  malformed[116] ^= 1;
  RefuseResult(malformed, "SBAR material evidence tamper was accepted");
  malformed = result_bytes;
  malformed[148] ^= 1;
  RefuseResult(malformed, "SBAR executor evidence tamper was accepted");
  malformed = result_bytes;
  malformed[180] = 1;
  RefuseResult(malformed, "SBAR with nonzero tail was accepted");

  bind::DatabaseAttachBindRequestV1 bind_request;
  bind_request.authenticated_receipt_uuid = request.statement_receipt_uuid;
  bind_request.occurrence = request.occurrence;
  bind_request.mode = 1;
  bind_request.alias_scope = 1;
  bind_request.storage_reference = {"CURRENT", false};
  bind_request.database_alias = {"workplan_attachment", false};
  std::vector<std::uint8_t> bind_bytes;
  Require(bind::EncodeDatabaseAttachBindRequestV1(
              bind_request, &bind_bytes, &detail),
          "canonical DABQ encoding failed");
  bind::DatabaseAttachBindRequestV1 decoded_bind;
  Require(bind::DecodeDatabaseAttachBindRequestV1(
              bind_bytes.data(), bind_bytes.size(), &decoded_bind, &detail) &&
              decoded_bind.storage_reference.raw_utf8 == "CURRENT" &&
              decoded_bind.database_alias.raw_utf8 == "workplan_attachment" &&
              bind::DatabaseAttachBindRequestEvidenceV1(decoded_bind) ==
                  decoded_bind.request_evidence_sha256,
          "canonical DABQ did not preserve syntax-only inputs");
  auto invalid_bind = bind_request;
  invalid_bind.mode = 0;
  Require(!bind::EncodeDatabaseAttachBindRequestV1(
              invalid_bind, &bind_bytes, &detail),
          "DABQ with unknown mode was encoded");
  invalid_bind = bind_request;
  invalid_bind.alias_scope = 2;
  Require(!bind::EncodeDatabaseAttachBindRequestV1(
              invalid_bind, &bind_bytes, &detail),
          "DABQ with non-session alias scope was encoded");
  invalid_bind = bind_request;
  invalid_bind.database_alias.raw_utf8 = "bad-name";
  Require(!bind::EncodeDatabaseAttachBindRequestV1(
              invalid_bind, &bind_bytes, &detail),
          "DABQ with invalid unquoted alias was encoded");
  Require(bind::EncodeDatabaseAttachBindRequestV1(
              bind_request, &bind_bytes, &detail),
          "canonical DABQ re-encoding failed");
  malformed = bind_bytes;
  malformed[80] = 1;
  Require(!bind::DecodeDatabaseAttachBindRequestV1(
              malformed.data(), malformed.size(), &decoded_bind, &detail),
          "DABQ with nonzero reserved byte was accepted");
  malformed = bind_bytes;
  malformed[48] ^= 1;
  Require(!bind::DecodeDatabaseAttachBindRequestV1(
              malformed.data(), malformed.size(), &decoded_bind, &detail),
          "DABQ request-evidence tamper was accepted");
  malformed = bind_bytes;
  malformed.push_back(0);
  Require(!bind::DecodeDatabaseAttachBindRequestV1(
              malformed.data(), malformed.size(), &decoded_bind, &detail),
          "DABQ trailing bytes were accepted");

  bind::DatabaseAttachBindAckV1 ack;
  ack.authenticated_receipt_uuid = request.statement_receipt_uuid;
  ack.occurrence = request.occurrence;
  ack.binding_uuid = Bytes<16>(31);
  ack.binding_generation = 1;
  ack.attach_uuid = descriptor.attach_uuid;
  ack.storage_uuid = descriptor.storage_uuid;
  ack.alias_uuid = descriptor.alias_uuid;
  ack.database_uuid = descriptor.database_uuid;
  ack.catalog_snapshot_uuid = descriptor.catalog_snapshot_uuid;
  ack.catalog_generation = descriptor.catalog_generation;
  ack.descriptor_sha256 = decoded_descriptor.descriptor_sha256;
  ack.request_evidence_sha256 = decoded_bind.request_evidence_sha256;
  std::vector<std::uint8_t> ack_bytes;
  Require(bind::EncodeDatabaseAttachBindAckV1(ack, &ack_bytes, &detail),
          "canonical DABA encoding failed");
  bind::DatabaseAttachBindAckV1 decoded_ack;
  Require(bind::DecodeDatabaseAttachBindAckV1(
              ack_bytes.data(), ack_bytes.size(), &decoded_ack, &detail) &&
              bind::DatabaseAttachBindAckEvidenceV1(decoded_ack) ==
                  decoded_ack.acknowledgement_evidence_sha256 &&
              decoded_ack.descriptor_sha256 ==
                  decoded_descriptor.descriptor_sha256,
          "canonical DABA did not round trip byte-identically");
  malformed = ack_bytes;
  malformed[216] ^= 1;
  Require(!bind::DecodeDatabaseAttachBindAckV1(
              malformed.data(), malformed.size(), &decoded_ack, &detail),
          "DABA acknowledgement-evidence tamper was accepted");
  malformed = ack_bytes;
  malformed[248] = 1;
  Require(!bind::DecodeDatabaseAttachBindAckV1(
              malformed.data(), malformed.size(), &decoded_ack, &detail),
          "DABA with nonzero tail was accepted");
  return EXIT_SUCCESS;
}
