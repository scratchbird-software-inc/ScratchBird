// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_catalog_introspect_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(const char* message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const char* message) {
  if (!condition) Fail(message);
}

sblr::CatalogUuid Identity(std::uint8_t discriminator) {
  sblr::CatalogUuid value{};
  value[0] = discriminator;
  value[6] = 0x70;
  value[8] = 0x80;
  value[15] = static_cast<std::uint8_t>(discriminator ^ 0xa5U);
  return value;
}

sblr::CatalogSha Digest(std::uint8_t discriminator) {
  sblr::CatalogSha value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(discriminator + index);
  }
  return value;
}

bool DecodeRequest(const std::vector<std::uint8_t>& bytes) {
  sblr::SblrCatalogIntrospectRequestV1 decoded;
  std::string detail;
  return sblr::DecodeSblrCatalogIntrospectRequestV1(
      bytes.data(), bytes.size(), &decoded, &detail);
}

bool DecodeDescriptor(const std::vector<std::uint8_t>& bytes,
                      bool operand) {
  sblr::SblrCatalogIntrospectDescriptorV1 decoded;
  std::string detail;
  return sblr::DecodeSblrCatalogIntrospectDescriptorV1(
      bytes.data(), bytes.size(), &decoded, &detail, operand);
}

bool DecodeResult(const std::vector<std::uint8_t>& bytes) {
  sblr::SblrCatalogIntrospectResultV1 decoded;
  std::string detail;
  return sblr::DecodeSblrCatalogIntrospectResultV1(
      bytes.data(), bytes.size(), &decoded, &detail);
}

}  // namespace

int main() {
  sblr::SblrCatalogIntrospectRequestV1 request;
  request.receipt = Identity(1);
  request.occurrence = 7;
  request.object_occurrence = 3;
  const auto request_bytes =
      sblr::EncodeSblrCatalogIntrospectRequestV1(request);
  Require(request_bytes.size() == 64 && DecodeRequest(request_bytes),
          "003610 canonical CIRQ did not round-trip");
  sblr::SblrCatalogIntrospectRequestV1 decoded_request;
  std::string detail;
  Require(sblr::DecodeSblrCatalogIntrospectRequestV1(
              request_bytes.data(), request_bytes.size(), &decoded_request,
              &detail) &&
              decoded_request.receipt == request.receipt &&
              decoded_request.occurrence == request.occurrence &&
              decoded_request.object_occurrence == request.object_occurrence &&
              sblr::EncodeSblrCatalogIntrospectRequestV1(decoded_request) ==
                  request_bytes,
          "003610 CIRQ fields or canonical re-encoding drifted");
  auto damaged = request_bytes;
  damaged[44] = 1;
  Require(!DecodeRequest(damaged),
          "003610 CIRQ admitted a nonzero reserved byte");
  damaged = request_bytes;
  damaged.pop_back();
  Require(!DecodeRequest(damaged), "003610 CIRQ admitted truncation");
  damaged = request_bytes;
  damaged.push_back(0);
  Require(!DecodeRequest(damaged), "003610 CIRQ admitted trailing bytes");
  auto invalid_request = request;
  invalid_request.receipt = {};
  Require(sblr::EncodeSblrCatalogIntrospectRequestV1(invalid_request).empty(),
          "003610 CIRQ admitted a nil receipt");
  invalid_request = request;
  invalid_request.occurrence = 0;
  Require(sblr::EncodeSblrCatalogIntrospectRequestV1(invalid_request).empty(),
          "003610 CIRQ admitted a zero structural occurrence");

  sblr::SblrCatalogIntrospectDescriptorV1 descriptor;
  descriptor.object_kind =
      sblr::kSblrCatalogIntrospectObjectKindTableV1;
  descriptor.profile =
      sblr::kSblrCatalogIntrospectProfileShowObjectDetailV1;
  descriptor.flags = sblr::kSblrCatalogIntrospectDetailFlagV1;
  descriptor.object_uuid = Identity(2);
  descriptor.catalog_epoch = 17;
  descriptor.security_epoch = 19;
  descriptor.canonical_path_utf8 = "APP.CUSTOMERS";
  descriptor.availability = 23;
  const auto cidd =
      sblr::EncodeSblrCatalogIntrospectDescriptorV1(descriptor, false);
  Require(cidd.size() == 488 && DecodeDescriptor(cidd, false),
          "003610 canonical CIDD did not round-trip");
  sblr::SblrCatalogIntrospectDescriptorV1 decoded_descriptor;
  Require(sblr::DecodeSblrCatalogIntrospectDescriptorV1(
              cidd.data(), cidd.size(), &decoded_descriptor, &detail, false) &&
              decoded_descriptor.object_uuid == descriptor.object_uuid &&
              decoded_descriptor.canonical_path_utf8 ==
                  descriptor.canonical_path_utf8 &&
              decoded_descriptor.catalog_epoch == descriptor.catalog_epoch &&
              decoded_descriptor.security_epoch == descriptor.security_epoch &&
              decoded_descriptor.availability == descriptor.availability &&
              std::any_of(decoded_descriptor.evidence.begin(),
                          decoded_descriptor.evidence.end(),
                          [](std::uint8_t byte) { return byte != 0; }),
          "003610 CIDD authority projection drifted");

  auto cido = cidd;
  std::copy_n("CIDO", 4, cido.begin());
  Require(std::equal(cidd.begin() + 4, cidd.end(), cido.begin() + 4) &&
              DecodeDescriptor(cido, true),
          "003610 literal CIDD-to-CIDO projection was not accepted");
  sblr::SblrCatalogIntrospectDescriptorV1 decoded_operand;
  Require(sblr::DecodeSblrCatalogIntrospectDescriptorV1(
              cido.data(), cido.size(), &decoded_operand, &detail, true) &&
              decoded_operand.evidence == decoded_descriptor.evidence &&
              sblr::EncodeSblrCatalogIntrospectDescriptorV1(
                  decoded_operand, true) == cido,
          "003610 CIDO did not preserve exact descriptor authority");
  damaged = cido;
  damaged[408] ^= 0x01U;
  Require(!DecodeDescriptor(damaged, true),
          "003610 CIDO admitted stale descriptor evidence");
  damaged = cido;
  damaged[12] = 1;
  Require(!DecodeDescriptor(damaged, true),
          "003610 CIDO admitted nonzero header reserved bytes");
  damaged = cido;
  damaged[56] = 0;
  Require(!DecodeDescriptor(damaged, true),
          "003610 CIDO admitted a NUL-bearing canonical path");
  damaged = cido;
  damaged[6] = 2;
  Require(!DecodeDescriptor(damaged, true),
          "003610 CIDO admitted an unregistered object kind");
  damaged = cido;
  damaged[8] = 2;
  Require(!DecodeDescriptor(damaged, true),
          "003610 CIDO admitted an unregistered detail profile");
  damaged = cido;
  damaged[10] = 0;
  Require(!DecodeDescriptor(damaged, true),
          "003610 CIDO admitted a missing detail flag");
  damaged = cido;
  std::fill(damaged.begin() + 440, damaged.begin() + 448, 0);
  Require(!DecodeDescriptor(damaged, true),
          "003610 CIDO admitted zero executor availability");
  damaged = cido;
  damaged[448] = 1;
  Require(!DecodeDescriptor(damaged, true),
          "003610 CIDO admitted a nonzero trailer");
  damaged = cido;
  damaged.pop_back();
  Require(!DecodeDescriptor(damaged, true),
          "003610 CIDO admitted truncation");

  sblr::SblrCatalogIntrospectResultV1 result;
  result.request_uuid = Identity(11);
  result.readable_projection_uuid = Identity(12);
  result.row_descriptor_uuid = Identity(13);
  result.result_set_uuid = Identity(14);
  result.object_uuid = Identity(15);
  result.statement_snapshot_uuid = Identity(16);
  result.catalog_epoch = 17;
  result.security_epoch = 19;
  result.row_count = 8;
  result.object_kind = sblr::kSblrCatalogIntrospectObjectKindTableV1;
  result.profile = sblr::kSblrCatalogIntrospectProfileShowObjectDetailV1;
  result.flags = sblr::kSblrCatalogIntrospectDetailFlagV1;
  result.row_material_sha256 = Digest(31);
  result.descriptor_evidence_sha256 = decoded_descriptor.evidence;
  result.availability = 23;
  result.publication_barrier = Identity(17);
  const auto cirs = sblr::EncodeSblrCatalogIntrospectResultV1(result);
  Require(cirs.size() == 320 && DecodeResult(cirs),
          "003610 canonical CIRS did not round-trip");
  sblr::SblrCatalogIntrospectResultV1 decoded_result;
  Require(sblr::DecodeSblrCatalogIntrospectResultV1(
              cirs.data(), cirs.size(), &decoded_result, &detail) &&
              decoded_result.request_uuid == result.request_uuid &&
              decoded_result.result_set_uuid == result.result_set_uuid &&
              decoded_result.object_uuid == result.object_uuid &&
              decoded_result.row_count == result.row_count &&
              decoded_result.row_material_sha256 ==
                  result.row_material_sha256 &&
              decoded_result.descriptor_evidence_sha256 ==
                  result.descriptor_evidence_sha256 &&
              decoded_result.availability == result.availability &&
              sblr::EncodeSblrCatalogIntrospectResultV1(decoded_result) ==
                  cirs,
          "003610 CIRS fields or canonical re-encoding drifted");
  damaged = cirs;
  damaged[256] ^= 0x01U;
  Require(!DecodeResult(damaged),
          "003610 CIRS admitted stale result evidence");
  damaged = cirs;
  damaged[142] = 1;
  Require(!DecodeResult(damaged),
          "003610 CIRS admitted nonzero result reserved bytes");
  damaged = cirs;
  std::copy_n(damaged.begin() + 16, 16, damaged.begin() + 32);
  Require(!DecodeResult(damaged),
          "003610 CIRS admitted duplicate authority identities");
  damaged = cirs;
  std::fill(damaged.begin() + 128, damaged.begin() + 136, 0);
  Require(!DecodeResult(damaged),
          "003610 CIRS admitted an empty object-detail rowset");
  damaged = cirs;
  damaged[312] = 1;
  Require(!DecodeResult(damaged),
          "003610 CIRS admitted a nonzero terminal trailer");
  damaged = cirs;
  damaged.pop_back();
  Require(!DecodeResult(damaged), "003610 CIRS admitted truncation");

  return EXIT_SUCCESS;
}
