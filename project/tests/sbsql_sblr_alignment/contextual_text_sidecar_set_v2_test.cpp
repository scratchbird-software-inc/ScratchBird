// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hash_digest.hpp"
#include "mga_relation_store/mga_contextual_text_sidecar_set_v2.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace hash = scratchbird::core::hash;
namespace sblr = scratchbird::engine::sblr;

using FieldPair = api::MgaContextualTextDescriptorFieldPairV2;
using Owner = api::MgaContextualTextSidecarSetOwnerV2;
using ProjectedColumn = api::MgaContextualTextProjectedColumnV2;
using RawBytes = api::MgaContextualTextRawBytesV2;
using Sha256 = api::MgaContextualTextSha256V2;
using SidecarSet = api::MgaContextualTextSidecarSetV2;
using Uuid = api::MgaContextualTextUuidV2;

[[noreturn]] void Fail(const char* message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool value, const char* message) {
  if (!value) Fail(message);
}

RawBytes Raw(std::string_view text) {
  return RawBytes(text.begin(), text.end());
}

std::string Text(const RawBytes& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

Uuid CanonicalDatatypeUuid(std::uint8_t tail) {
  return {0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
          0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, tail};
}

Uuid FixtureUuid(std::uint8_t tag) {
  return {0x01, 0xa0, 0x11, 0x22, 0x33, 0x44, 0x75, 0x66,
          0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, tag};
}

void AppendU32(RawBytes* bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift != 32; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void AppendU64(RawBytes* bytes, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    bytes->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

template <std::size_t N>
void AppendArray(RawBytes* bytes,
                 const std::array<std::uint8_t, N>& value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

void AppendHex(RawBytes* bytes, const RawBytes& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  for (const std::uint8_t byte : value) {
    bytes->push_back(static_cast<std::uint8_t>(kHex[byte >> 4]));
    bytes->push_back(static_cast<std::uint8_t>(kHex[byte & 0x0f]));
  }
}

RawBytes OracleSerialize(std::span<const FieldPair> fields) {
  RawBytes serialized;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) serialized.push_back('|');
    AppendHex(&serialized, fields[index].key_raw_bytes);
    serialized.push_back('=');
    AppendHex(&serialized, fields[index].value_raw_bytes);
  }
  return serialized;
}

Sha256 OracleSha256(const RawBytes& material) {
  const auto digest = hash::ComputeSha256Digest(material);
  Require(digest.ok(), "independent SHA-256 oracle failed");
  Require(digest.digest_bytes == hash::kSha256DigestBytes,
          "independent SHA-256 oracle returned the wrong extent");
  Sha256 result{};
  std::copy(digest.digest.begin(), digest.digest.end(), result.begin());
  return result;
}

struct OracleSidecarEntry {
  std::uint32_t column_ordinal = 0;
  Uuid column_uuid{};
  Sha256 descriptor_evidence_sha256{};
};

Sha256 OracleSeal(const Owner& owner,
                  std::span<const FieldPair> pre_seal_fields,
                  std::span<const OracleSidecarEntry> sidecars) {
  const RawBytes serialized = OracleSerialize(pre_seal_fields);
  RawBytes material(api::kMgaContextualTextSidecarSetSealDomainV2.begin(),
                    api::kMgaContextualTextSidecarSetSealDomainV2.end());
  material.push_back(0);
  AppendU64(&material, owner.creator_transaction_id);
  AppendU64(&material, owner.event_sequence);
  AppendArray(&material, owner.relation_uuid);
  AppendArray(&material, owner.relation_descriptor_uuid);
  AppendU64(&material, owner.relation_descriptor_generation);
  AppendU64(&material, static_cast<std::uint64_t>(pre_seal_fields.size()));
  AppendU64(&material, static_cast<std::uint64_t>(serialized.size()));
  material.insert(material.end(), serialized.begin(), serialized.end());
  AppendU32(&material, static_cast<std::uint32_t>(sidecars.size()));
  for (const auto& sidecar : sidecars) {
    AppendU32(&material, sidecar.column_ordinal);
    AppendArray(&material, sidecar.column_uuid);
    AppendArray(&material, sidecar.descriptor_evidence_sha256);
  }
  return OracleSha256(material);
}

sblr::ContextualTextDescriptorV2 Descriptor(std::uint64_t resource_epoch,
                                             std::uint8_t resource_seed) {
  sblr::ContextualTextDescriptorV2 descriptor;
  descriptor.flags = 1;
  descriptor.malformed_sequence_policy = 1;
  descriptor.null_encoding = 1;
  descriptor.descriptor_uuid = CanonicalDatatypeUuid(0x18);
  descriptor.descriptor_generation = 1;
  descriptor.type_uuid = CanonicalDatatypeUuid(0x19);
  descriptor.type_generation = 1;
  descriptor.codec_uuid = CanonicalDatatypeUuid(0x1a);
  descriptor.codec_version = 1;
  descriptor.codec_generation = 1;
  descriptor.character_limit = 257 + resource_seed;
  descriptor.byte_limit = 1028 + resource_seed;
  descriptor.charset_uuid = FixtureUuid(resource_seed);
  descriptor.charset_generation = 11;
  descriptor.collation_uuid = FixtureUuid(resource_seed + 1);
  descriptor.collation_generation = 12;
  descriptor.normalization_policy_uuid = FixtureUuid(resource_seed + 2);
  descriptor.normalization_policy_generation = 13;
  descriptor.render_policy_uuid = FixtureUuid(resource_seed + 3);
  descriptor.render_policy_generation = 14;
  descriptor.canonicalization_profile_uuid = FixtureUuid(resource_seed + 4);
  descriptor.canonicalization_profile_generation = 15;
  descriptor.comparison_contract_uuid = FixtureUuid(resource_seed + 5);
  descriptor.comparison_contract_generation = 16;
  descriptor.equality_operation_uuid = FixtureUuid(resource_seed + 6);
  descriptor.equality_operation_generation = 17;
  descriptor.datatype_catalog_snapshot_uuid = CanonicalDatatypeUuid(0x01);
  descriptor.datatype_catalog_generation = 1;
  descriptor.datatype_registry_generation = 1;
  descriptor.resource_epoch = resource_epoch;
  return descriptor;
}

ProjectedColumn ComparableColumn(std::uint32_t ordinal,
                                 std::uint8_t column_tag,
                                 std::uint64_t resource_epoch,
                                 std::uint8_t resource_seed) {
  ProjectedColumn column;
  column.column_ordinal = ordinal;
  column.column_uuid = FixtureUuid(column_tag);
  column.comparable_persisted_text = true;
  column.projected_datatype_descriptor_uuid = CanonicalDatatypeUuid(0x18);
  column.projected_datatype_descriptor_generation = 1;
  column.projected_datatype_catalog_snapshot_uuid =
      CanonicalDatatypeUuid(0x01);
  column.projected_datatype_catalog_generation = 1;
  column.projected_datatype_registry_generation = 1;
  column.projected_resource_epoch = resource_epoch;
  column.expected_text_descriptor = Descriptor(resource_epoch, resource_seed);
  return column;
}

ProjectedColumn NoncomparableColumn(std::uint32_t ordinal,
                                    std::uint8_t column_tag) {
  ProjectedColumn column;
  column.column_ordinal = ordinal;
  column.column_uuid = FixtureUuid(column_tag);
  return column;
}

Owner FixtureOwner() {
  Owner owner;
  owner.creator_transaction_id = 701;
  owner.event_sequence = 19;
  owner.relation_uuid = FixtureUuid(0x81);
  owner.relation_descriptor_uuid = FixtureUuid(0x82);
  owner.relation_descriptor_generation = 23;
  return owner;
}

void RequireInvalid(const Owner& owner,
                    std::span<const FieldPair> base,
                    std::span<const ProjectedColumn> columns,
                    const SidecarSet& candidate,
                    const char* message) {
  api::MgaContextualTextSidecarSetDiagnosticV2 diagnostic;
  Require(!api::ValidateMgaContextualTextSidecarSetV2(
              owner, base, columns, candidate, &diagnostic),
          message);
  Require(diagnostic.code == "CTB.TEXT.DESCRIPTOR_INVALID",
          "invalid sidecar set returned the wrong diagnostic");
}

void TestCanonicalBuildValidateAndLookup() {
  const Owner owner = FixtureOwner();
  const std::vector<FieldPair> base = {
      {Raw("relation.kind"), Raw("ordinary")},
      {Raw("opaque.binary"), RawBytes{0x00, 0xff, 0x10}},
  };
  const ProjectedColumn column_two = ComparableColumn(2, 0x92, 77, 0x30);
  const ProjectedColumn column_one = NoncomparableColumn(1, 0x91);
  const ProjectedColumn column_zero = ComparableColumn(0, 0x90, 77, 0x40);
  const std::vector<ProjectedColumn> unsorted_columns = {
      column_two, column_one, column_zero};

  SidecarSet built;
  api::MgaContextualTextSidecarSetDiagnosticV2 diagnostic;
  Require(api::BuildMgaContextualTextSidecarSetV2(
              owner, base, unsorted_columns, &built, &diagnostic),
          "canonical contextual sidecar-set build failed");
  Require(built.owner == owner, "sidecar-set owner changed during build");
  Require(built.contextual_sidecar_count == 2,
          "comparable sidecar count is not exact");
  Require(built.descriptor_field_count == 7 &&
              built.descriptor_fields.size() == 7,
          "complete field count does not include the final seal");
  Require(Text(built.descriptor_fields[2].key_raw_bytes) ==
              "column.0.contextual_text_descriptor_sidecar_v2" &&
              Text(built.descriptor_fields[3].key_raw_bytes) ==
              "column.0.contextual_text_descriptor_sidecar_v2.sha256" &&
              Text(built.descriptor_fields[4].key_raw_bytes) ==
              "column.2.contextual_text_descriptor_sidecar_v2" &&
              Text(built.descriptor_fields[5].key_raw_bytes) ==
              "column.2.contextual_text_descriptor_sidecar_v2.sha256" &&
              Text(built.descriptor_fields[6].key_raw_bytes) ==
              api::kMgaContextualTextSidecarSetSealKeyV2,
          "contextual suffix key ordering or grammar is wrong");
  Require(built.descriptor_fields[2].value_raw_bytes.size() == 533 &&
              built.descriptor_fields[4].value_raw_bytes.size() == 533 &&
              built.descriptor_fields[3].value_raw_bytes.size() == 32 &&
              built.descriptor_fields[5].value_raw_bytes.size() == 32 &&
              built.descriptor_fields[6].value_raw_bytes.size() == 32,
          "sidecar blob, hash, or seal extent is wrong");

  const RawBytes independent_complete = OracleSerialize(built.descriptor_fields);
  RawBytes module_complete;
  std::uint64_t module_complete_bytes = 0;
  Require(api::SerializeMgaContextualTextDescriptorFieldVectorV2(
              built.descriptor_fields, &module_complete,
              &module_complete_bytes, &diagnostic),
          "module serialization failed");
  Require(module_complete == independent_complete &&
              module_complete_bytes == independent_complete.size() &&
              built.descriptor_field_bytes == independent_complete.size() &&
              module_complete.back() != '|',
          "canonical pair serialization or complete byte equation differs");

  const std::span<const FieldPair> complete(built.descriptor_fields);
  const auto pre_seal = complete.first(complete.size() - 1);
  const RawBytes independent_pre_seal = OracleSerialize(pre_seal);
  Require(built.descriptor_field_bytes == independent_pre_seal.size() + 172,
          "descriptor_field_bytes is not pre-seal bytes plus 172");
  Sha256 column_zero_hash{};
  Sha256 column_two_hash{};
  std::copy(built.descriptor_fields[3].value_raw_bytes.begin(),
            built.descriptor_fields[3].value_raw_bytes.end(),
            column_zero_hash.begin());
  std::copy(built.descriptor_fields[5].value_raw_bytes.begin(),
            built.descriptor_fields[5].value_raw_bytes.end(),
            column_two_hash.begin());
  const std::array<OracleSidecarEntry, 2> oracle_entries = {{
      {0, column_zero.column_uuid, column_zero_hash},
      {2, column_two.column_uuid, column_two_hash},
  }};
  const Sha256 independent_seal = OracleSeal(owner, pre_seal, oracle_entries);
  Require(built.seal_sha256 == independent_seal &&
              built.descriptor_fields.back().value_raw_bytes ==
                  RawBytes(independent_seal.begin(), independent_seal.end()),
          "final raw32 set seal differs from independent material oracle");

  Require(api::ValidateMgaContextualTextSidecarSetV2(
              owner, base, unsorted_columns, built, &diagnostic),
          "canonical contextual sidecar-set validation failed");
  api::MgaContextualTextSidecarLookupResultV2 found;
  Require(api::LookupMgaContextualTextSidecarV2(
              owner, base, unsorted_columns, built, 2,
              column_two.column_uuid, &found, &diagnostic),
          "full-vector unique contextual sidecar lookup failed");
  Require(found.exact_blob == built.descriptor_fields[4].value_raw_bytes &&
              found.descriptor_evidence_sha256 == column_two_hash &&
              found.descriptor.resource_epoch == 77 &&
              found.descriptor.datatype_catalog_snapshot_uuid ==
                  CanonicalDatatypeUuid(0x01) &&
              found.descriptor.datatype_catalog_generation == 1 &&
              found.descriptor.datatype_registry_generation == 1,
          "lookup did not return the exact blob/hash/d701/1/1/epoch binding");

  Require(!api::LookupMgaContextualTextSidecarV2(
              owner, base, unsorted_columns, built, 1,
              column_one.column_uuid, &found, &diagnostic),
          "lookup admitted a noncomparable column");
  Require(!api::LookupMgaContextualTextSidecarV2(
              owner, base, unsorted_columns, built, 2, FixtureUuid(0xfe),
              &found, &diagnostic),
          "lookup admitted the wrong projected column UUID");

  SidecarSet mutated = built;
  mutated.descriptor_fields[2].key_raw_bytes =
      Raw("column.00.contextual_text_descriptor_sidecar_v2");
  RequireInvalid(owner, base, unsorted_columns, mutated,
                 "leading-zero ordinal key was admitted");
  mutated = built;
  std::swap(mutated.descriptor_fields[2], mutated.descriptor_fields[3]);
  RequireInvalid(owner, base, unsorted_columns, mutated,
                 "nonadjacent or reordered blob/hash pair was admitted");
  mutated = built;
  mutated.descriptor_fields[3].key_raw_bytes =
      mutated.descriptor_fields[2].key_raw_bytes;
  RequireInvalid(owner, base, unsorted_columns, mutated,
                 "duplicate contextual key was admitted");
  mutated = built;
  mutated.descriptor_fields[2].value_raw_bytes[100] ^= 1;
  RequireInvalid(owner, base, unsorted_columns, mutated,
                 "mutated SBTLTD02 blob was admitted");
  mutated = built;
  mutated.descriptor_fields[3].value_raw_bytes[0] ^= 1;
  RequireInvalid(owner, base, unsorted_columns, mutated,
                 "blob/hash mismatch was admitted");
  mutated = built;
  mutated.descriptor_fields.back().value_raw_bytes[0] ^= 1;
  RequireInvalid(owner, base, unsorted_columns, mutated,
                 "mutated final seal pair was admitted");
  mutated = built;
  --mutated.descriptor_field_count;
  RequireInvalid(owner, base, unsorted_columns, mutated,
                 "header field count omitting the seal was admitted");
  mutated = built;
  --mutated.descriptor_field_bytes;
  RequireInvalid(owner, base, unsorted_columns, mutated,
                 "incorrect descriptor byte equation was admitted");
  mutated = built;
  ++mutated.owner.event_sequence;
  RequireInvalid(owner, base, unsorted_columns, mutated,
                 "different owner event sequence was admitted");

  std::vector<ProjectedColumn> stale_columns = unsorted_columns;
  stale_columns[0].projected_resource_epoch = 78;
  RequireInvalid(owner, base, stale_columns, built,
                 "projected/SBTLTD02 resource epoch mismatch was admitted");
  stale_columns = unsorted_columns;
  stale_columns[0].projected_datatype_catalog_generation = 2;
  RequireInvalid(owner, base, stale_columns, built,
                 "non-d701/1/1 projected snapshot was admitted");

  std::vector<FieldPair> reserved_base = base;
  reserved_base.push_back(
      {Raw("COLUMN.0.CONTEXTUAL_TEXT_DESCRIPTOR_SIDECAR_V2"), Raw("x")});
  SidecarSet rejected;
  Require(!api::BuildMgaContextualTextSidecarSetV2(
              owner, reserved_base, unsorted_columns, &rejected, &diagnostic),
          "case-variant contextual key was admitted as a base field");
  std::vector<ProjectedColumn> duplicate_columns = unsorted_columns;
  duplicate_columns.push_back(ComparableColumn(2, 0x93, 77, 0x50));
  Require(!api::BuildMgaContextualTextSidecarSetV2(
              owner, base, duplicate_columns, &rejected, &diagnostic),
          "duplicate projected column ordinal was admitted");
}

void TestZeroSidecarSuccessor() {
  const Owner owner = FixtureOwner();
  const std::vector<FieldPair> base = {
      {Raw("relation.kind"), Raw("ordinary")},
  };
  const std::vector<ProjectedColumn> columns = {
      NoncomparableColumn(0, 0xa0),
  };
  SidecarSet built;
  api::MgaContextualTextSidecarSetDiagnosticV2 diagnostic;
  Require(api::BuildMgaContextualTextSidecarSetV2(
              owner, base, columns, &built, &diagnostic),
          "zero-sidecar successor build failed");
  Require(built.contextual_sidecar_count == 0 &&
              built.descriptor_fields.size() == 2 &&
              built.descriptor_field_count == 2 &&
              Text(built.descriptor_fields.back().key_raw_bytes) ==
                  api::kMgaContextualTextSidecarSetSealKeyV2,
          "zero-sidecar successor did not retain only base plus final seal");
  Require(api::ValidateMgaContextualTextSidecarSetV2(
              owner, base, columns, built, &diagnostic),
          "zero-sidecar successor validation failed");

  const std::span<const FieldPair> complete(built.descriptor_fields);
  const auto pre_seal = complete.first(complete.size() - 1);
  const std::array<OracleSidecarEntry, 0> no_sidecars{};
  Require(OracleSeal(owner, pre_seal, no_sidecars) == built.seal_sha256,
          "zero-sidecar seal omitted the exact zero count");
}

}  // namespace

int main() {
  TestCanonicalBuildValidateAndLookup();
  TestZeroSidecarSuccessor();
  std::cout << "contextual TEXT MGA sealed sidecar-set V2 tests passed\n";
  return EXIT_SUCCESS;
}
