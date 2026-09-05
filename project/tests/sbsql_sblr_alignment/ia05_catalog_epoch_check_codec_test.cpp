// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_catalog_epoch_check_runtime.hpp"
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
  std::cerr << "CSC-TEST-003630: " << message << '\n';
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
  sblr::SblrCatalogEpochCheckRequestV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrCatalogEpochCheckRequestV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseDescriptor(std::vector<std::uint8_t> bytes,
                      std::string_view message) {
  sblr::SblrCatalogEpochCheckDescriptorV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrCatalogEpochCheckDescriptorV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

void RefuseResult(std::vector<std::uint8_t> bytes,
                  std::string_view message) {
  sblr::SblrCatalogEpochCheckResultV1 decoded;
  std::string detail;
  Require(!sblr::DecodeSblrCatalogEpochCheckResultV1(
              bytes.data(), bytes.size(), &decoded, &detail),
          message);
}

}  // namespace

int main() {
  std::string detail;
  sblr::SblrCatalogEpochCheckRequestV1 request;
  request.statement_receipt_uuid = Bytes<16>(1);
  request.occurrence = 7;
  request.catalog_generation = 11;
  request.security_epoch = 13;
  request.resource_epoch = 17;
  const auto request_bytes =
      sblr::EncodeSblrCatalogEpochCheckRequestV1(request);
  sblr::SblrCatalogEpochCheckRequestV1 decoded_request;
  Require(request_bytes.size() == sblr::kSblrCatalogEpochCheckRequestBytes &&
              sblr::DecodeSblrCatalogEpochCheckRequestV1(
                  request_bytes.data(), request_bytes.size(),
                  &decoded_request, &detail) &&
              sblr::EncodeSblrCatalogEpochCheckRequestV1(decoded_request) ==
                  request_bytes,
          "canonical SBCQ did not round trip byte-identically");

  auto malformed = request_bytes;
  malformed[0] = 'X';
  RefuseRequest(malformed, "SBCQ with wrong magic was accepted");
  malformed = request_bytes;
  malformed[4] = 2;
  RefuseRequest(malformed, "SBCQ with wrong version was accepted");
  malformed = request_bytes;
  malformed[12] = 2;
  RefuseRequest(malformed, "SBCQ with unknown flags was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 16, malformed.begin() + 32, 0);
  RefuseRequest(malformed, "SBCQ with nil receipt was accepted");
  malformed = request_bytes;
  std::fill(malformed.begin() + 32, malformed.begin() + 40, 0);
  RefuseRequest(malformed, "SBCQ with zero occurrence was accepted");
  malformed = request_bytes;
  malformed.pop_back();
  RefuseRequest(malformed, "truncated SBCQ was accepted");
  malformed = request_bytes;
  malformed.push_back(0);
  RefuseRequest(malformed, "SBCQ trailing bytes were accepted");

  sblr::SblrCatalogEpochCheckDescriptorV1 descriptor;
  descriptor.check_uuid = Bytes<16>(21);
  descriptor.statement_receipt_uuid = request.statement_receipt_uuid;
  descriptor.requested_catalog_epoch_uuid = Bytes<16>(41);
  descriptor.requested_catalog_generation = request.catalog_generation;
  descriptor.database_uuid = Bytes<16>(61);
  descriptor.schema_tree_uuid = Bytes<16>(81);
  descriptor.schema_tree_generation = 19;
  descriptor.security_context_uuid = Bytes<16>(101);
  descriptor.policy_snapshot_uuid = Bytes<16>(121);
  descriptor.policy_generation = 23;
  descriptor.catalog_snapshot_uuid = Bytes<16>(141);
  descriptor.security_epoch = request.security_epoch;
  descriptor.resource_epoch = request.resource_epoch;
  descriptor.executor_availability_generation = 29;
  descriptor.visibility_scope_sha256 =
      sblr::SblrCatalogEpochCheckVisibilityScopeSha256V1(
          false, descriptor.database_uuid, descriptor.schema_tree_uuid,
          descriptor.schema_tree_generation);
  const auto descriptor_bytes =
      sblr::EncodeSblrCatalogEpochCheckDescriptorV1(descriptor);
  sblr::SblrCatalogEpochCheckDescriptorV1 decoded_descriptor;
  Require(descriptor_bytes.size() ==
                  sblr::kSblrCatalogEpochCheckDescriptorBytes &&
              sblr::DecodeSblrCatalogEpochCheckDescriptorV1(
                  descriptor_bytes.data(), descriptor_bytes.size(),
                  &decoded_descriptor, &detail) &&
              decoded_descriptor.visibility_scope_sha256 ==
                  descriptor.visibility_scope_sha256 &&
              sblr::EncodeSblrCatalogEpochCheckDescriptorV1(
                  decoded_descriptor) == descriptor_bytes,
          "canonical SECD did not round trip byte-identically");

  auto invalid_descriptor = descriptor;
  invalid_descriptor.check_uuid = {};
  Require(sblr::EncodeSblrCatalogEpochCheckDescriptorV1(
              invalid_descriptor).empty(),
          "SECD with nil check identity was encoded");
  invalid_descriptor = descriptor;
  invalid_descriptor.visibility_scope_sha256 = {};
  Require(sblr::EncodeSblrCatalogEpochCheckDescriptorV1(
              invalid_descriptor).empty(),
          "SECD with no visibility scope was encoded");
  malformed = descriptor_bytes;
  malformed[12] = 2;
  RefuseDescriptor(malformed, "SECD with unknown flags was accepted");
  malformed = descriptor_bytes;
  malformed[192] ^= 1;
  RefuseDescriptor(malformed, "SECD evidence tamper was accepted");
  malformed = descriptor_bytes;
  malformed[224] ^= 1;
  RefuseDescriptor(malformed, "SECD visibility-scope tamper was accepted");
  malformed = descriptor_bytes;
  malformed.pop_back();
  RefuseDescriptor(malformed, "truncated SECD was accepted");

  sblr::SblrCatalogEpochCheckResultV1 result;
  result.check_uuid = descriptor.check_uuid;
  result.observed_catalog_epoch_uuid =
      descriptor.requested_catalog_epoch_uuid;
  result.observed_catalog_generation =
      descriptor.requested_catalog_generation;
  result.database_uuid = descriptor.database_uuid;
  result.schema_tree_uuid = descriptor.schema_tree_uuid;
  result.schema_tree_generation = descriptor.schema_tree_generation;
  result.status = 1;
  result.visibility = 1;
  result.observed_security_epoch = descriptor.security_epoch;
  result.observed_resource_epoch = descriptor.resource_epoch;
  result.redaction_profile_uuid = Bytes<16>(161);
  result.publication_evidence_uuid = Bytes<16>(181);
  result.executor_availability_generation =
      descriptor.executor_availability_generation;
  const auto result_bytes =
      sblr::EncodeSblrCatalogEpochCheckResultV1(result);
  sblr::SblrCatalogEpochCheckResultV1 decoded_result;
  Require(result_bytes.size() == sblr::kSblrCatalogEpochCheckResultBytes &&
              sblr::DecodeSblrCatalogEpochCheckResultV1(
                  result_bytes.data(), result_bytes.size(), &decoded_result,
                  &detail) &&
              decoded_result.status == 1 &&
              sblr::EncodeSblrCatalogEpochCheckResultV1(decoded_result) ==
                  result_bytes,
          "canonical current SECR did not round trip byte-identically");

  auto hidden = result;
  hidden.observed_catalog_epoch_uuid = {};
  hidden.observed_catalog_generation = 0;
  hidden.database_uuid = {};
  hidden.schema_tree_uuid = {};
  hidden.schema_tree_generation = 0;
  hidden.status = 3;
  hidden.visibility = 2;
  const auto hidden_bytes =
      sblr::EncodeSblrCatalogEpochCheckResultV1(hidden);
  Require(hidden_bytes.size() == sblr::kSblrCatalogEpochCheckResultBytes &&
              sblr::DecodeSblrCatalogEpochCheckResultV1(
                  hidden_bytes.data(), hidden_bytes.size(), &decoded_result,
                  &detail) &&
              decoded_result.status == 3 && decoded_result.visibility == 2,
          "redacted SECR was not canonical");
  auto invalid_result = hidden;
  invalid_result.database_uuid = descriptor.database_uuid;
  Require(sblr::EncodeSblrCatalogEpochCheckResultV1(invalid_result).empty(),
          "redacted SECR disclosed a database identity");
  invalid_result = result;
  invalid_result.status = 4;
  Require(sblr::EncodeSblrCatalogEpochCheckResultV1(invalid_result).empty(),
          "SECR with unknown status was encoded");
  malformed = result_bytes;
  malformed[98] = 1;
  RefuseResult(malformed, "SECR with nonzero reserved bytes was accepted");
  malformed = result_bytes;
  malformed[148] ^= 1;
  RefuseResult(malformed, "SECR evidence tamper was accepted");
  malformed = result_bytes;
  malformed[188] = 1;
  RefuseResult(malformed, "SECR with nonzero tail was accepted");

  bind::CatalogEpochCheckBindRequestV1 bind_request;
  bind_request.authenticated_receipt_uuid = request.statement_receipt_uuid;
  bind_request.occurrence = request.occurrence;
  std::vector<std::uint8_t> bind_bytes;
  Require(bind::EncodeCatalogEpochCheckBindRequestV1(
              bind_request, &bind_bytes, &detail),
          "database-scope CEBQ encoding failed");
  bind::CatalogEpochCheckBindRequestV1 decoded_bind;
  Require(bind::DecodeCatalogEpochCheckBindRequestV1(
              bind_bytes.data(), bind_bytes.size(), &decoded_bind, &detail) &&
              !decoded_bind.object_scoped &&
              decoded_bind.target_name_atoms.empty() &&
              bind::CatalogEpochCheckBindRequestEvidenceV1(decoded_bind) ==
                  decoded_bind.request_evidence_sha256,
          "database-scope CEBQ did not round trip");

  auto object_bind = bind_request;
  object_bind.object_scoped = true;
  object_bind.target_name_atoms = {
      {"tenant_db", false}, {"Sales", true}, {"orders", false}};
  Require(bind::EncodeCatalogEpochCheckBindRequestV1(
              object_bind, &bind_bytes, &detail) &&
              bind::DecodeCatalogEpochCheckBindRequestV1(
                  bind_bytes.data(), bind_bytes.size(), &decoded_bind,
                  &detail) &&
              decoded_bind.target_name_atoms.size() == 3 &&
              decoded_bind.target_name_atoms[0].raw_utf8 == "tenant_db" &&
              !decoded_bind.target_name_atoms[0].quoted &&
              decoded_bind.target_name_atoms[1].raw_utf8 == "Sales" &&
              decoded_bind.target_name_atoms[1].quoted &&
              decoded_bind.target_name_atoms[2].raw_utf8 == "orders" &&
              !decoded_bind.target_name_atoms[2].quoted,
          "object-scope CEBQ did not preserve exact name atoms");
  auto invalid_bind = object_bind;
  invalid_bind.object_scoped = false;
  Require(!bind::EncodeCatalogEpochCheckBindRequestV1(
              invalid_bind, &bind_bytes, &detail),
          "CEBQ scope/atom mismatch was encoded");
  invalid_bind = object_bind;
  invalid_bind.target_name_atoms.front().raw_utf8 = "bad-name";
  Require(!bind::EncodeCatalogEpochCheckBindRequestV1(
              invalid_bind, &bind_bytes, &detail),
          "CEBQ with invalid unquoted identifier was encoded");
  Require(bind::EncodeCatalogEpochCheckBindRequestV1(
              object_bind, &bind_bytes, &detail),
          "canonical object CEBQ re-encoding failed");
  malformed = bind_bytes;
  malformed[52] = 1;
  Require(!bind::DecodeCatalogEpochCheckBindRequestV1(
              malformed.data(), malformed.size(), &decoded_bind, &detail),
          "CEBQ with nonzero reserved byte was accepted");
  malformed = bind_bytes;
  malformed[96] ^= 1;
  Require(!bind::DecodeCatalogEpochCheckBindRequestV1(
              malformed.data(), malformed.size(), &decoded_bind, &detail),
          "CEBQ request-evidence tamper was accepted");

  bind::CatalogEpochCheckBindAckV1 ack;
  ack.authenticated_receipt_uuid = request.statement_receipt_uuid;
  ack.occurrence = request.occurrence;
  ack.binding_uuid = Bytes<16>(31);
  ack.binding_generation = 1;
  ack.check_uuid = descriptor.check_uuid;
  ack.schema_tree_uuid = descriptor.schema_tree_uuid;
  ack.schema_tree_generation = descriptor.schema_tree_generation;
  ack.visibility_scope_sha256 = descriptor.visibility_scope_sha256;
  ack.descriptor_sha256 = decoded_descriptor.descriptor_sha256;
  ack.request_evidence_sha256 = decoded_bind.request_evidence_sha256;
  std::vector<std::uint8_t> ack_bytes;
  Require(bind::EncodeCatalogEpochCheckBindAckV1(ack, &ack_bytes, &detail),
          "canonical CEBA encoding failed");
  bind::CatalogEpochCheckBindAckV1 decoded_ack;
  Require(bind::DecodeCatalogEpochCheckBindAckV1(
              ack_bytes.data(), ack_bytes.size(), &decoded_ack, &detail) &&
              bind::CatalogEpochCheckBindAckEvidenceV1(decoded_ack) ==
                  decoded_ack.acknowledgement_evidence_sha256,
          "canonical CEBA did not round trip byte-identically");
  auto invalid_ack = ack;
  invalid_ack.object_uuid = Bytes<16>(201);
  Require(!bind::EncodeCatalogEpochCheckBindAckV1(
              invalid_ack, &ack_bytes, &detail),
          "CEBA with half-present object identity was encoded");
  Require(bind::EncodeCatalogEpochCheckBindAckV1(ack, &ack_bytes, &detail),
          "canonical CEBA re-encoding failed");
  malformed = ack_bytes;
  malformed[224] ^= 1;
  Require(!bind::DecodeCatalogEpochCheckBindAckV1(
              malformed.data(), malformed.size(), &decoded_ack, &detail),
          "CEBA acknowledgement-evidence tamper was accepted");
  return EXIT_SUCCESS;
}
