// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../../src/core/datatypes/datatype_type_codec_identity_v3.hpp"
#include "../../src/core/hash/hash_digest.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace dt = scratchbird::core::datatypes;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Check(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <typename T>
concept HasDescriptorPolicyMember = requires(T value) {
  value.descriptor_policy;
};

using Byte = scratchbird::core::platform::byte;

bool SameLegacyIdentity(const dt::DatatypeTypeCodecIdentityRowV1& left,
                        const dt::DatatypeTypeCodecIdentityRowV1& right) {
  return left.catalog_snapshot_uuid == right.catalog_snapshot_uuid &&
         left.catalog_generation == right.catalog_generation &&
         left.registry_generation == right.registry_generation &&
         left.descriptor_uuid == right.descriptor_uuid &&
         left.descriptor_generation == right.descriptor_generation &&
         left.type_uuid == right.type_uuid &&
         left.type_generation == right.type_generation &&
         left.codec_id == right.codec_id &&
         left.codec_version == right.codec_version &&
         left.codec_generation == right.codec_generation &&
         left.canonical_value_bytes == right.canonical_value_bytes &&
         left.null_supported == right.null_supported &&
         left.canonical_name == right.canonical_name &&
         left.datatype_identity_code == right.datatype_identity_code &&
         left.null_encoding_code == right.null_encoding_code &&
         left.byte_order_code == right.byte_order_code &&
         left.signed_code == right.signed_code &&
         left.representation_code == right.representation_code &&
         left.codec_uuid == right.codec_uuid &&
         left.canonical_value_minimum_bytes == right.canonical_value_minimum_bytes &&
         left.canonical_value_maximum_bytes == right.canonical_value_maximum_bytes &&
         left.canonical_value_exact_bytes == right.canonical_value_exact_bytes &&
         left.canonical_binary_type_code == right.canonical_binary_type_code &&
         left.canonical_value_variable_width ==
             right.canonical_value_variable_width &&
         left.canonical_value_exact_zero_is_width_marker ==
             right.canonical_value_exact_zero_is_width_marker &&
         left.canonical_byte_order == right.canonical_byte_order &&
         left.canonical_representation == right.canonical_representation &&
         left.canonical_charset == right.canonical_charset &&
         left.shortest_form_utf8_required ==
             right.shortest_form_utf8_required &&
         left.implicit_normalization_allowed ==
             right.implicit_normalization_allowed &&
         left.descriptor_bound_collation_required ==
             right.descriptor_bound_collation_required &&
         left.empty_value_distinct_from_sql_null ==
             right.empty_value_distinct_from_sql_null &&
         left.sql_null_requires_zero_payload ==
             right.sql_null_requires_zero_payload &&
         left.variable_width_storage_without_truncation ==
             right.variable_width_storage_without_truncation &&
         left.invalid_encoding_diagnostic_id == right.invalid_encoding_diagnostic_id &&
         left.numeric_context_uuid == right.numeric_context_uuid &&
         left.numeric_context_generation == right.numeric_context_generation &&
         left.special_value_policy_uuid == right.special_value_policy_uuid &&
         left.special_value_policy_generation ==
             right.special_value_policy_generation &&
         left.comparison_policy_uuid == right.comparison_policy_uuid &&
         left.comparison_policy_generation ==
             right.comparison_policy_generation &&
         left.comparison_profile == right.comparison_profile &&
         left.allow_special_values == right.allow_special_values;
}

bool SamePolicy(const dt::DatatypePolicyIdentityV1& left,
                const dt::DatatypePolicyIdentityV1& right) {
  return left.uuid == right.uuid && left.generation == right.generation;
}

bool SameV3Identity(const dt::DatatypeTypeCodecIdentityRowV3& left,
                    const dt::DatatypeTypeCodecIdentityRowV3& right) {
  return SameLegacyIdentity(left.legacy_fields, right.legacy_fields) &&
         SamePolicy(left.descriptor_policy, right.descriptor_policy) &&
         SamePolicy(left.canonicalization_policy,
                    right.canonicalization_policy) &&
         SamePolicy(left.ordering_policy, right.ordering_policy) &&
         SamePolicy(left.hash_policy, right.hash_policy) &&
         SamePolicy(left.operation_policy, right.operation_policy);
}

void AppendU64(std::vector<Byte>* out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<Byte>((value >> shift) & 0xffu));
  }
}

void AppendU32(std::vector<Byte>* out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<Byte>((value >> shift) & 0xffu));
  }
}

void AppendU16(std::vector<Byte>* out, std::uint16_t value) {
  out->push_back(static_cast<Byte>(value & 0xffu));
  out->push_back(static_cast<Byte>((value >> 8) & 0xffu));
}

void AppendBool(std::vector<Byte>* out, bool value) {
  out->push_back(value ? 1 : 0);
}

void AppendUuid(std::vector<Byte>* out,
                const scratchbird::core::platform::Uuid& value) {
  out->insert(out->end(), value.bytes.begin(), value.bytes.end());
}

void AppendString(std::vector<Byte>* out, const std::string& value) {
  AppendU64(out, value.size());
  out->insert(out->end(), value.begin(), value.end());
}

void AppendLegacy(std::vector<Byte>* out,
                  const dt::DatatypeTypeCodecIdentityRowV1& row) {
  AppendUuid(out, row.catalog_snapshot_uuid);
  AppendU64(out, row.catalog_generation);
  AppendU64(out, row.registry_generation);
  AppendUuid(out, row.descriptor_uuid);
  AppendU64(out, row.descriptor_generation);
  AppendUuid(out, row.type_uuid);
  AppendU64(out, row.type_generation);
  AppendString(out, row.codec_id);
  AppendU16(out, row.codec_version);
  AppendU64(out, row.codec_generation);
  AppendU32(out, row.canonical_value_bytes);
  AppendBool(out, row.null_supported);
  AppendString(out, row.canonical_name);
  out->push_back(row.datatype_identity_code);
  out->push_back(row.null_encoding_code);
  out->push_back(row.byte_order_code);
  AppendBool(out, row.signed_code);
  out->push_back(row.representation_code);
  AppendU32(out, row.canonical_value_minimum_bytes);
  AppendU32(out, row.canonical_value_maximum_bytes);
  AppendU32(out, row.canonical_value_exact_bytes);
  AppendU32(out, row.canonical_binary_type_code);
  AppendUuid(out, row.codec_uuid);
  AppendBool(out, row.canonical_value_variable_width);
  AppendBool(out, row.canonical_value_exact_zero_is_width_marker);
  AppendString(out, row.canonical_byte_order);
  AppendString(out, row.canonical_representation);
  AppendString(out, row.canonical_charset);
  AppendBool(out, row.shortest_form_utf8_required);
  AppendBool(out, row.implicit_normalization_allowed);
  AppendBool(out, row.descriptor_bound_collation_required);
  AppendBool(out, row.empty_value_distinct_from_sql_null);
  AppendBool(out, row.sql_null_requires_zero_payload);
  AppendBool(out, row.variable_width_storage_without_truncation);
  AppendString(out, row.invalid_encoding_diagnostic_id);
  AppendUuid(out, row.numeric_context_uuid);
  AppendU64(out, row.numeric_context_generation);
  AppendUuid(out, row.special_value_policy_uuid);
  AppendU64(out, row.special_value_policy_generation);
  AppendUuid(out, row.comparison_policy_uuid);
  AppendU64(out, row.comparison_policy_generation);
  AppendString(out, row.comparison_profile);
  AppendBool(out, row.allow_special_values);
}

void AppendPolicy(std::vector<Byte>* out,
                  const dt::DatatypePolicyIdentityV1& policy) {
  AppendUuid(out, policy.uuid);
  AppendU64(out, policy.generation);
}

std::string RegistryDigest(
    std::span<const dt::DatatypeTypeCodecIdentityRowV3> rows) {
  std::vector<Byte> material;
  const std::string domain =
      "ScratchBird.DatatypeTypeCodecIdentityRegistry.V3.TestOracle.V1";
  material.insert(material.end(), domain.begin(), domain.end());
  AppendU64(&material, rows.size());
  for (const auto& row : rows) {
    AppendLegacy(&material, row.legacy_fields);
    AppendPolicy(&material, row.descriptor_policy);
    AppendPolicy(&material, row.canonicalization_policy);
    AppendPolicy(&material, row.ordering_policy);
    AppendPolicy(&material, row.hash_policy);
    AppendPolicy(&material, row.operation_policy);
  }
  const auto digest =
      scratchbird::core::hash::ComputeSha256Digest(material);
  Check(digest.ok(), "V3 authority oracle SHA-256 failed");
  return scratchbird::core::hash::HexLower(digest.digest);
}

void TestPopulationAndAuthoritativeRows() {
  static_assert(!HasDescriptorPolicyMember<dt::DatatypeTypeCodecIdentityRowV1>);
  static_assert(HasDescriptorPolicyMember<dt::DatatypeTypeCodecIdentityRowV3>);

  const auto v3 = dt::CurrentDatatypeTypeCodecIdentityRowsV3();
  Check(v3.size() == 127, "V3 registry does not contain 127 rows");

  std::array<std::size_t, 6> counts{};
  for (const auto& row : v3) {
    const auto generation = row.legacy_fields.catalog_generation;
    Check(generation >= 1 && generation <= 6 &&
              row.legacy_fields.registry_generation == generation,
          "V3 row has a mixed or invalid cohort generation");
    ++counts[generation - 1];
  }
  Check(counts == std::array<std::size_t, 6>{6, 12, 13, 31, 32, 33},
        "V3 cohort row counts differ from Core V6");

  const auto registry_digest = RegistryDigest(v3);
  if (registry_digest !=
      "3d34b8e5ae035272b60f65ea4839a175060e137216f93d04f38c5b6bc1d2e7e1") {
    std::cerr << "observed_v3_authority_digest=" << registry_digest << '\n';
    Fail("materialized V3 authority rows changed");
  }

  constexpr std::array<std::string_view, 6> expected_cohort_digests{{
      "c3f32a278b09243fe3556ba9520c6ad95fb0995035813f92244c775b1bc4e3e2",
      "72399142c162281c1f81e1ec64175f546b26baa45f0fd241abab5823d913e166",
      "3ca980ce8e214c713d53bffcfb34e73bc64094065c8e2f93ed264e530cfeb299",
      "e2487d9e772f3e0ddb1a9873dffd70f13f55b930df111efc07e7889eb7581968",
      "d2422a08f9c7f4eb5c5c7cc984c6df54128ce5375a7fde25000c0a9842c436ad",
      "f5b95f6de3b668d5fe325f0016d224d2faa591076327e795e63b0858e35aa2b0",
  }};
  for (std::uint64_t generation = 1; generation <= 6; ++generation) {
    std::vector<dt::DatatypeTypeCodecIdentityRowV3> cohort;
    for (const auto& row : v3) {
      if (row.legacy_fields.catalog_generation == generation)
        cohort.push_back(row);
    }
    Check(RegistryDigest(cohort) == expected_cohort_digests[generation - 1],
          "materialized V3 predecessor cohort digest changed");
  }

  using scratchbird::tests::FixtureUuidLiteral;
  const auto binary_descriptor =
      FixtureUuidLiteral("2d010000-6269-7e61-b279-000000000000");
  const auto bit_descriptor =
      FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d829");
  std::size_t binary_policy_rows = 0;
  std::size_t bit_policy_rows = 0;
  for (const auto& row : v3) {
    const auto lookup = dt::LookupDatatypeTypeCodecIdentityV3(
        row.legacy_fields.catalog_snapshot_uuid,
        row.legacy_fields.catalog_generation,
        row.legacy_fields.registry_generation,
        row.legacy_fields.descriptor_uuid,
        row.legacy_fields.descriptor_generation);
    Check(lookup.ok && SameV3Identity(lookup.row, row),
          "exact V3 tuple did not resolve to its authoritative row");

    if (row.legacy_fields.descriptor_uuid == binary_descriptor) {
      Check(!row.descriptor_policy.uuid.is_nil() &&
                row.descriptor_policy.generation == 1 &&
                !row.canonicalization_policy.uuid.is_nil() &&
                row.canonicalization_policy.generation == 1 &&
                !row.ordering_policy.uuid.is_nil() &&
                row.ordering_policy.generation == 1 &&
                !row.hash_policy.uuid.is_nil() &&
                row.hash_policy.generation == 1 &&
                row.operation_policy.uuid.is_nil() &&
                row.operation_policy.generation == 0,
            "base.binary V3 policy authority changed");
      ++binary_policy_rows;
    } else if (row.legacy_fields.descriptor_uuid == bit_descriptor) {
      Check(!row.descriptor_policy.uuid.is_nil() &&
                !row.canonicalization_policy.uuid.is_nil() &&
                !row.ordering_policy.uuid.is_nil() &&
                !row.hash_policy.uuid.is_nil() &&
                !row.operation_policy.uuid.is_nil(),
            "base.bit_string V3 policy authority is incomplete");
      ++bit_policy_rows;
    } else {
      Check(row.descriptor_policy.uuid.is_nil() &&
                row.descriptor_policy.generation == 0 &&
                row.canonicalization_policy.uuid.is_nil() &&
                row.canonicalization_policy.generation == 0 &&
                row.ordering_policy.uuid.is_nil() &&
                row.ordering_policy.generation == 0 &&
                row.hash_policy.uuid.is_nil() &&
                row.hash_policy.generation == 0 &&
                row.operation_policy.uuid.is_nil() &&
                row.operation_policy.generation == 0,
            "V3 row contains an unregistered policy identity");
    }
  }
  Check(binary_policy_rows == 4 && bit_policy_rows == 1,
        "V3 policy-bearing row population changed");

  const std::array<std::size_t, 6> expected_inherited{0, 6, 12, 13, 31, 32};
  std::array<std::size_t, 6> inherited_counts{};
  for (const auto& current : v3) {
    const auto generation = current.legacy_fields.catalog_generation;
    if (generation == 1) continue;
    for (const auto& predecessor : v3) {
      if (predecessor.legacy_fields.catalog_generation != generation - 1 ||
          predecessor.legacy_fields.descriptor_uuid !=
              current.legacy_fields.descriptor_uuid ||
          predecessor.legacy_fields.descriptor_generation !=
              current.legacy_fields.descriptor_generation) {
        continue;
      }
      auto expected_legacy = predecessor.legacy_fields;
      expected_legacy.catalog_snapshot_uuid =
          current.legacy_fields.catalog_snapshot_uuid;
      expected_legacy.catalog_generation = generation;
      expected_legacy.registry_generation = generation;
      Check(SameLegacyIdentity(expected_legacy, current.legacy_fields) &&
                SamePolicy(predecessor.descriptor_policy,
                           current.descriptor_policy) &&
                SamePolicy(predecessor.canonicalization_policy,
                           current.canonicalization_policy) &&
                SamePolicy(predecessor.ordering_policy,
                           current.ordering_policy) &&
                SamePolicy(predecessor.hash_policy, current.hash_policy) &&
                SamePolicy(predecessor.operation_policy,
                           current.operation_policy),
            "successor cohort altered inherited V3 authority");
      ++inherited_counts[generation - 1];
    }
  }
  Check(inherited_counts == expected_inherited,
        "V3 successor inheritance counts changed");
}

void TestExactBitStringIdentity() {
  using scratchbird::tests::FixtureUuidLiteral;
  const auto descriptor = FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d829");
  const auto type = FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d82a");
  const auto codec = FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d82b");
  const auto lookup = dt::LookupDatatypeTypeCodecIdentityV3(
      dt::kDatatypeCohortV6, 6, 6, descriptor, 1);
  Check(lookup.ok && dt::IsExactCanonicalBitStringTypeCodecIdentityV3(lookup.row),
        "exact V6 bit-string row did not resolve");
  const auto& row = lookup.row;
  const auto& legacy = row.legacy_fields;
  Check(legacy.type_uuid == type && legacy.codec_uuid == codec &&
            legacy.codec_id == "datatype.bit_string.msb0.packed.v1" &&
            legacy.canonical_binary_type_code == 302 &&
            legacy.canonical_value_minimum_bytes == 4 &&
            legacy.canonical_value_maximum_bytes == 2097156 &&
            legacy.canonical_value_exact_bytes == 0 &&
            legacy.canonical_value_variable_width &&
            legacy.canonical_value_exact_zero_is_width_marker &&
            legacy.canonical_byte_order ==
                "u32_logical_bit_count_little_endian_then_byte_sequence" &&
            legacy.canonical_representation ==
                "logical_count_then_MSB_first_packed_bits_unused_low_tail_zero" &&
            legacy.invalid_encoding_diagnostic_id ==
                "CTB.BIT.CANONICAL_ENCODING_INVALID",
        "V6 bit-string identity/bounds/representation drifted");
  Check(row.descriptor_policy.uuid == FixtureUuidLiteral("01a0ff27-2715-75d2-98fb-c853527d3811") &&
            row.canonicalization_policy.uuid == FixtureUuidLiteral("01a0ff27-2716-7c83-9ae4-23bbc73e6a9d") &&
            row.ordering_policy.uuid == FixtureUuidLiteral("01a0ff27-2717-7a54-bdbc-a3c1fee7c5e3") &&
            row.hash_policy.uuid == FixtureUuidLiteral("01a0ff27-2718-7e29-b4b3-7de1719709b8") &&
            row.operation_policy.uuid == FixtureUuidLiteral("01a0ff27-271b-74de-8bc6-d6a93484ac36") &&
            row.descriptor_policy.generation == 1 &&
            row.canonicalization_policy.generation == 1 &&
            row.ordering_policy.generation == 1 &&
            row.hash_policy.generation == 1 &&
            row.operation_policy.generation == 1,
        "V6 bit-string policy identity drifted");

  for (unsigned generation = 1; generation <= 5; ++generation) {
    auto snapshot = dt::kDatatypeCohortV6;
    snapshot.bytes.back() = static_cast<std::uint8_t>(generation);
    Check(!dt::LookupDatatypeTypeCodecIdentityV3(
               snapshot, generation, generation, descriptor, 1).ok,
          "bit-string identity leaked into a predecessor cohort");
  }
  Check(!dt::LookupDatatypeTypeCodecIdentityV3(dt::kDatatypeCohortV6, 5, 6,
                                               descriptor, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV3(dt::kDatatypeCohortV6, 6, 5,
                                                   descriptor, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV3(dt::kDatatypeCohortV6, 6, 6,
                                                   type, 1).ok &&
            !dt::LookupDatatypeTypeCodecIdentityV3(dt::kDatatypeCohortV6, 6, 6,
                                                   codec, 1).ok,
        "V3 lookup inferred an identity from a mismatched tuple");

  const auto reject_mutation = [&](auto mutate, std::string_view message) {
    auto changed = row;
    mutate(changed);
    Check(!dt::IsExactCanonicalBitStringTypeCodecIdentityV3(changed), message);
  };
  reject_mutation([](auto& value) { value.legacy_fields.canonical_value_maximum_bytes--; },
                  "altered bit-string maximum admitted");
  reject_mutation([](auto& value) { value.legacy_fields.canonical_representation += "_other"; },
                  "altered bit-string representation admitted");
  reject_mutation([](auto& value) { value.descriptor_policy.generation = 2; },
                  "altered descriptor policy admitted");
  reject_mutation([](auto& value) { value.canonicalization_policy.uuid.bytes[0] ^= 1; },
                  "altered canonicalization policy admitted");
  reject_mutation([](auto& value) { value.ordering_policy.uuid.bytes[15] ^= 1; },
                  "altered ordering policy admitted");
  reject_mutation([](auto& value) { value.hash_policy.generation = 0; },
                  "altered hash policy admitted");
  reject_mutation([](auto& value) { value.operation_policy.uuid.bytes[8] ^= 1; },
                  "altered operation policy admitted");

  // Every field in the policy-bearing V3 row is authority.  Exercise each
  // non-UUID legacy field independently; UUIDs receive the exhaustive
  // single-bit coverage below.
  reject_mutation([](auto& value) { ++value.legacy_fields.catalog_generation; },
                  "altered catalog generation admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.registry_generation; },
                  "altered registry generation admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.descriptor_generation; },
                  "altered descriptor generation admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.type_generation; },
                  "altered type generation admitted");
  reject_mutation([](auto& value) { value.legacy_fields.codec_id += ".other"; },
                  "altered codec id admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.codec_version; },
                  "altered codec version admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.codec_generation; },
                  "altered codec generation admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.canonical_value_bytes; },
                  "altered canonical value width admitted");
  reject_mutation([](auto& value) { value.legacy_fields.null_supported = false; },
                  "altered NULL support admitted");
  reject_mutation([](auto& value) { value.legacy_fields.canonical_name += ".other"; },
                  "altered canonical name admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.datatype_identity_code; },
                  "altered datatype identity code admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.null_encoding_code; },
                  "altered NULL encoding code admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.byte_order_code; },
                  "altered byte-order code admitted");
  reject_mutation([](auto& value) { value.legacy_fields.signed_code = true; },
                  "altered signed code admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.representation_code; },
                  "altered representation code admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.canonical_value_minimum_bytes; },
                  "altered canonical minimum admitted");
  reject_mutation([](auto& value) { --value.legacy_fields.canonical_value_exact_bytes; },
                  "altered canonical exact width admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.canonical_binary_type_code; },
                  "altered canonical binary type code admitted");
  reject_mutation([](auto& value) { value.legacy_fields.canonical_value_variable_width = false; },
                  "altered variable-width flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.canonical_value_exact_zero_is_width_marker = false; },
                  "altered zero-width-marker flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.canonical_byte_order += ".other"; },
                  "altered canonical byte order admitted");
  reject_mutation([](auto& value) { value.legacy_fields.canonical_charset = "binary"; },
                  "altered canonical charset admitted");
  reject_mutation([](auto& value) { value.legacy_fields.shortest_form_utf8_required = true; },
                  "altered shortest-form flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.implicit_normalization_allowed = true; },
                  "altered normalization flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.descriptor_bound_collation_required = true; },
                  "altered collation flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.empty_value_distinct_from_sql_null = false; },
                  "altered empty-versus-NULL flag admitted");
  reject_mutation([](auto& value) { value.legacy_fields.sql_null_requires_zero_payload = false; },
                  "altered NULL payload rule admitted");
  reject_mutation([](auto& value) { value.legacy_fields.variable_width_storage_without_truncation = false; },
                  "altered storage truncation rule admitted");
  reject_mutation([](auto& value) { value.legacy_fields.invalid_encoding_diagnostic_id += ".other"; },
                  "altered invalid-encoding diagnostic admitted");
  reject_mutation([](auto& value) { value.legacy_fields.numeric_context_uuid.bytes[0] ^= 1; },
                  "altered numeric context UUID admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.numeric_context_generation; },
                  "altered numeric context generation admitted");
  reject_mutation([](auto& value) { value.legacy_fields.special_value_policy_uuid.bytes[0] ^= 1; },
                  "altered special-value policy UUID admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.special_value_policy_generation; },
                  "altered special-value policy generation admitted");
  reject_mutation([](auto& value) { value.legacy_fields.comparison_policy_uuid.bytes[0] ^= 1; },
                  "altered comparison policy UUID admitted");
  reject_mutation([](auto& value) { ++value.legacy_fields.comparison_policy_generation; },
                  "altered comparison policy generation admitted");
  reject_mutation([](auto& value) { value.legacy_fields.comparison_profile = "other"; },
                  "altered comparison profile admitted");
  reject_mutation([](auto& value) { value.legacy_fields.allow_special_values = true; },
                  "altered special-value admission admitted");

  using Legacy = dt::DatatypeTypeCodecIdentityRowV1;
  for (const auto member : {&Legacy::catalog_snapshot_uuid,
                            &Legacy::descriptor_uuid,
                            &Legacy::type_uuid,
                            &Legacy::codec_uuid}) {
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto changed = row;
      (changed.legacy_fields.*member).bytes[bit / 8] ^=
          static_cast<std::uint8_t>(1u << (bit % 8));
      Check(!dt::IsExactCanonicalBitStringTypeCodecIdentityV3(changed),
            "single-bit legacy identity mutation was admitted");
    }
  }
  for (const auto member : {&dt::DatatypeTypeCodecIdentityRowV3::descriptor_policy,
                            &dt::DatatypeTypeCodecIdentityRowV3::canonicalization_policy,
                            &dt::DatatypeTypeCodecIdentityRowV3::ordering_policy,
                            &dt::DatatypeTypeCodecIdentityRowV3::hash_policy,
                            &dt::DatatypeTypeCodecIdentityRowV3::operation_policy}) {
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto changed = row;
      (changed.*member).uuid.bytes[bit / 8] ^=
          static_cast<std::uint8_t>(1u << (bit % 8));
      Check(!dt::IsExactCanonicalBitStringTypeCodecIdentityV3(changed),
            "single-bit policy identity mutation was admitted");
    }
    for (const auto generation : {0ULL, 2ULL, ~0ULL}) {
      auto changed = row;
      (changed.*member).generation = generation;
      Check(!dt::IsExactCanonicalBitStringTypeCodecIdentityV3(changed),
            "policy generation mutation was admitted");
    }
  }

  const auto projected = dt::ProjectDatatypeTypeCodecIdentityV3ToV1(row);
  Check(SameLegacyIdentity(projected, legacy),
        "one-way V3-to-V1 projection changed legacy fields");
}

}  // namespace

int main() {
  TestPopulationAndAuthoritativeRows();
  TestExactBitStringIdentity();
  std::cout << "datatype_identity_v3_test=passed\n";
  return EXIT_SUCCESS;
}
