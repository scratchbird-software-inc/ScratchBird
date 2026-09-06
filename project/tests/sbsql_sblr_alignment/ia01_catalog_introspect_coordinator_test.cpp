// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/internal_api/sblr_catalog_introspect_coordinator.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

[[noreturn]] void Fail(const char* message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const char* message) {
  if (!condition) Fail(message);
}

api::SblrCatalogIntrospectAuthorityInputV1 Input() {
  api::SblrCatalogIntrospectAuthorityInputV1 input;
  input.object_kind = sblr::kSblrCatalogIntrospectObjectKindTableV1;
  input.profile = sblr::kSblrCatalogIntrospectProfileShowObjectDetailV1;
  input.flags = sblr::kSblrCatalogIntrospectDetailFlagV1;
  input.object_uuid[0] = 0x21;
  input.object_uuid[6] = 0x70;
  input.object_uuid[8] = 0x80;
  input.object_uuid[15] = 0x42;
  input.catalog_epoch = 17;
  input.security_epoch = 19;
  input.canonical_path_utf8 = "APP.CUSTOMERS";
  input.executor_availability_generation = 23;
  return input;
}

void RequireInvalid(api::SblrCatalogIntrospectAuthorityInputV1 input,
                    const char* message) {
  const auto result = api::BuildSblrCatalogIntrospectDescriptorV1(input);
  Require(!result.ok && result.canonical_descriptor_bytes.empty() &&
              result.diagnostic.code == "SBLR.OPERAND.INVALID" &&
              result.diagnostic.message_key ==
                  "sblr.catalog_introspect.descriptor_authority_invalid",
          message);
}

}  // namespace

int main() {
  const auto input = Input();
  const auto first = api::BuildSblrCatalogIntrospectDescriptorV1(input);
  Require(first.ok && first.canonical_descriptor_bytes.size() == 488 &&
              first.diagnostic.code == "OK" &&
              first.descriptor.object_uuid == input.object_uuid &&
              first.descriptor.catalog_epoch == input.catalog_epoch &&
              first.descriptor.security_epoch == input.security_epoch &&
              first.descriptor.canonical_path_utf8 ==
                  input.canonical_path_utf8 &&
              first.descriptor.availability ==
                  input.executor_availability_generation &&
              std::any_of(first.descriptor.evidence.begin(),
                          first.descriptor.evidence.end(),
                          [](std::uint8_t byte) { return byte != 0; }),
          "catalog-introspect coordinator did not publish exact CIDD");
  sblr::SblrCatalogIntrospectDescriptorV1 decoded;
  std::string detail;
  Require(sblr::DecodeSblrCatalogIntrospectDescriptorV1(
              first.canonical_descriptor_bytes.data(),
              first.canonical_descriptor_bytes.size(), &decoded, &detail,
              false),
          "catalog-introspect coordinator CIDD did not strictly decode");

  const auto replay = api::BuildSblrCatalogIntrospectDescriptorV1(input);
  Require(replay.ok && replay.canonical_descriptor_bytes ==
                           first.canonical_descriptor_bytes &&
              replay.descriptor.evidence == first.descriptor.evidence,
          "catalog-introspect coordinator was not deterministic");
  auto operand = first.canonical_descriptor_bytes;
  std::copy_n("CIDO", 4, operand.begin());
  Require(std::equal(first.canonical_descriptor_bytes.begin() + 4,
                     first.canonical_descriptor_bytes.end(),
                     operand.begin() + 4) &&
              sblr::DecodeSblrCatalogIntrospectDescriptorV1(
                  operand.data(), operand.size(), &decoded, &detail, true) &&
              decoded.evidence == first.descriptor.evidence,
          "catalog-introspect coordinator did not permit literal CIDD/CIDO projection");

  auto invalid = input;
  invalid.object_uuid = {};
  RequireInvalid(invalid,
                 "catalog-introspect coordinator admitted a nil object UUID");
  invalid = input;
  invalid.object_kind = 2;
  RequireInvalid(invalid,
                 "catalog-introspect coordinator admitted an unsupported object kind");
  invalid = input;
  invalid.profile = 2;
  RequireInvalid(invalid,
                 "catalog-introspect coordinator admitted an unsupported profile");
  invalid = input;
  invalid.flags = 0;
  RequireInvalid(invalid,
                 "catalog-introspect coordinator admitted missing detail flags");
  invalid = input;
  invalid.catalog_epoch = 0;
  RequireInvalid(invalid,
                 "catalog-introspect coordinator admitted a zero catalog epoch");
  invalid = input;
  invalid.security_epoch = 0;
  RequireInvalid(invalid,
                 "catalog-introspect coordinator admitted a zero security epoch");
  invalid = input;
  invalid.executor_availability_generation = 0;
  RequireInvalid(invalid,
                 "catalog-introspect coordinator admitted missing executor evidence");
  invalid = input;
  invalid.canonical_path_utf8.assign(353, 'a');
  RequireInvalid(invalid,
                 "catalog-introspect coordinator admitted an oversized path");
  invalid = input;
  invalid.canonical_path_utf8.assign("bad\0path", 8);
  RequireInvalid(invalid,
                 "catalog-introspect coordinator admitted a NUL-bearing path");
  invalid = input;
  invalid.canonical_path_utf8 = std::string("\xc0\x80", 2);
  RequireInvalid(invalid,
                 "catalog-introspect coordinator admitted invalid UTF-8");

  return EXIT_SUCCESS;
}
