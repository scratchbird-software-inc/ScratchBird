// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "admitted_datatype_cohort.hpp"
#include "datatype_catalog_manifest.hpp"

#include <span>
#include <string>
#include <string_view>

namespace scratchbird::core::datatypes {

// A policy UUID and generation are one indivisible catalog identity. Neither
// member may be inferred from a datatype name, type code, codec, or payload.
struct DatatypePolicyIdentityV1 {
  platform::Uuid uuid;
  u64 generation = 0;
};

// Current policy identity is nominally distinct from the frozen V1 carrier.
// UUID and generation may be copied explicitly across a legacy boundary, but
// the type distinction prevents an older profile from becoming V3 authority.
struct DatatypePolicyIdentityV3 {
  platform::Uuid uuid;
  u64 generation = 0;
};

// V3 is the current policy-bearing carrier. The nested V1 row is an exact,
// lossy legacy projection surface; its ABI and predecessor rows remain fixed.
struct DatatypeTypeCodecIdentityRowV3 {
  DatatypeTypeCodecIdentityRowV1 legacy_fields;
  DatatypePolicyIdentityV3 descriptor_policy;
  DatatypePolicyIdentityV3 canonicalization_policy;
  DatatypePolicyIdentityV3 ordering_policy;
  DatatypePolicyIdentityV3 hash_policy;
  DatatypePolicyIdentityV3 operation_policy;
};

struct DatatypeTypeCodecIdentityLookupV3 {
  bool ok = false;
  DatatypeTypeCodecIdentityRowV3 row;
  // Lookup is noexcept. Diagnostics are admitted static identifiers rather
  // than owned text so refusal reporting never needs an allocation.
  std::string_view diagnostic_id;
};

// Allocation-safe one-way projection to the frozen V1 carrier. A failed
// projection leaves row default-constructed and reports a static diagnostic.
struct DatatypeTypeCodecIdentityProjectionV1 {
  bool ok = false;
  DatatypeTypeCodecIdentityRowV1 row;
  std::string_view diagnostic_id;
};

std::span<const DatatypeTypeCodecIdentityRowV3>
CurrentDatatypeTypeCodecIdentityRowsV3() noexcept;

DatatypeTypeCodecIdentityLookupV3 LookupDatatypeTypeCodecIdentityV3(
    const platform::Uuid& catalog_snapshot_uuid,
    u64 catalog_generation,
    u64 registry_generation,
    const platform::Uuid& descriptor_uuid,
    u64 descriptor_generation) noexcept;

// Core's canonical JSON digests of the exact 33-row d707-d710 cohorts.
inline constexpr std::string_view kDatatypeCohortV7IdentityDigestSha256 =
    "f10857ec395d4ebca02ec21c785251a668d98f3c0e69eeba11324f3807832dcc";
inline constexpr std::string_view kDatatypeCohortV8IdentityDigestSha256 =
    "7ff7530978f049864ad10ba5a7a1d4ba78246369ad30be7e7aeb5d7f4261bae5";
inline constexpr std::string_view kDatatypeCohortV9IdentityDigestSha256 =
    "7c3eed94150522b474faa22307a3f94a7753898084de09c67bd108e36fa3a978";
inline constexpr std::string_view kDatatypeCohortV10IdentityDigestSha256 =
    "90d4e17c5e98a684422399b16d73b38d3391905ebb775241426755a7323249a3";

// These predicates compare the semantic current d710 row. Canonical-name and
// codec-id strings are presentation labels and never establish identity. Type
// codes, payloads, and predecessor identities cannot substitute for the exact
// UUID/generation/codec-version/policy tuple and physical/semantic fields.
bool IsExactCanonicalBinaryTypeCodecIdentityV3(
    const DatatypeTypeCodecIdentityRowV3& row) noexcept;

bool IsExactCanonicalBitStringTypeCodecIdentityV3(
    const DatatypeTypeCodecIdentityRowV3& row) noexcept;

bool IsExactCanonicalDateTypeCodecIdentityV3(
    const DatatypeTypeCodecIdentityRowV3& row) noexcept;

bool IsExactCanonicalTimeTypeCodecIdentityV3(
    const DatatypeTypeCodecIdentityRowV3& row) noexcept;

bool IsExactCanonicalTimestampTypeCodecIdentityV3(
    const DatatypeTypeCodecIdentityRowV3& row) noexcept;

bool IsExactCanonicalIntervalTypeCodecIdentityV3(
    const DatatypeTypeCodecIdentityRowV3& row) noexcept;

// The only cross-carrier conversion is the explicit lossy V3-to-V1
// projection. No V1-to-V3 API exists because policy identity is not inferable.
DatatypeTypeCodecIdentityProjectionV1 ProjectDatatypeTypeCodecIdentityV3ToV1(
    const DatatypeTypeCodecIdentityRowV3& row) noexcept;

}  // namespace scratchbird::core::datatypes
