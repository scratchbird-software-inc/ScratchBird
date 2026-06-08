// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-036 focused validation for canonical key ordering and encoding fuzz gates.
#include "ceic_036_canonical_key_ordering_fuzz.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace index = scratchbird::core::index;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

TypedUuid TestUuid(UuidKind kind, unsigned seed) {
  TypedUuid value;
  value.kind = kind;
  for (std::size_t i = 0; i < value.value.bytes.size(); ++i) {
    value.value.bytes[i] =
        static_cast<byte>((seed * 43u + i * 29u + 0x23u) & 0xffu);
  }
  value.value.bytes[6] =
      static_cast<byte>((value.value.bytes[6] & 0x0fu) | 0x70u);
  value.value.bytes[8] =
      static_cast<byte>((value.value.bytes[8] & 0x3fu) | 0x80u);
  return value;
}

bool EvidenceHas(const index::Ceic036CanonicalKeyOrderingFuzzResult& result,
                 std::string_view token) {
  for (const auto& row : result.evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

index::IndexKeySemanticProfile StableProfile() {
  index::IndexKeySemanticProfile profile;
  profile.profile_id = "ceic036_sb_native_ordering";
  profile.bytewise_stable = true;
  return profile;
}

index::Ceic036CanonicalKeyOrderingFuzzRequest ValidRequest() {
  index::Ceic036CanonicalKeyOrderingFuzzRequest request;
  request.semantic_profile = StableProfile();
  request.typed_comparable_payload_contract_declared = true;
  request.donor_comparison_recorded = true;
  request.donor_comparison_evidence_only = true;
  return request;
}

index::IndexKeyEncodingComponent Component(std::vector<byte> payload) {
  index::IndexKeyEncodingComponent component;
  component.kind = index::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = TestUuid(UuidKind::object, 10);
  component.sort_direction = index::IndexKeySortDirection::ascending;
  component.null_placement = index::IndexKeyNullPlacement::nulls_last;
  component.payload = std::move(payload);
  return component;
}

void RequireRefused(
    const index::Ceic036CanonicalKeyOrderingFuzzResult& result,
    std::string_view diagnostic_code,
    std::string_view message) {
  Require(!result.ok(), message);
  Require(result.fail_closed, "CEIC-036 refusal did not fail closed");
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          "CEIC-036 refusal returned wrong diagnostic");
  Require(!result.enterprise_readiness_claimed,
          "CEIC-036 refusal must not claim enterprise readiness");
  Require(!result.all_index_readiness_claimed,
          "CEIC-036 refusal must not claim all-index readiness");
  Require(!result.ceic_037_exact_recheck_claimed,
          "CEIC-036 refusal must not claim CEIC-037 exact recheck");
  Require(!result.ceic_040_runtime_metrics_claimed,
          "CEIC-036 refusal must not claim CEIC-040 runtime metrics");
  Require(!result.ceic_041_crash_matrix_claimed,
          "CEIC-036 refusal must not claim CEIC-041 crash matrix");
  Require(!result.ceic_042_readiness_drift_claimed,
          "CEIC-036 refusal must not claim CEIC-042 readiness drift");
}

void ValidFuzzProofCoversRequiredOrderingSurfaces() {
  const auto result =
      index::ProveCeic036CanonicalKeyOrderingAndEncodingFuzz(ValidRequest());
  Require(result.ok(), "valid CEIC-036 fuzz proof was refused");
  Require(result.canonical_ordering_proven,
          "CEIC-036 did not prove canonical ordering");
  Require(result.encoding_order_matches_semantic_order,
          "CEIC-036 encoded order did not match semantic order");
  Require(result.typed_comparable_payload_contract_proven,
          "CEIC-036 did not record typed comparable payload contract");
  Require(result.raw_textual_numeric_ordering_rejected,
          "CEIC-036 did not reject raw textual numeric ordering proof");
  Require(result.null_ordering_proven, "CEIC-036 null ordering proof missing");
  Require(result.numeric_ordering_proven,
          "CEIC-036 numeric ordering proof missing");
  Require(result.text_collation_ordering_proven,
          "CEIC-036 text/collation proof missing");
  Require(result.binary_embedded_nul_ordering_proven,
          "CEIC-036 binary embedded-NUL proof missing");
  Require(result.composite_ordering_proven,
          "CEIC-036 composite key proof missing");
  Require(result.expression_envelope_validation_proven,
          "CEIC-036 expression envelope validation proof missing");
  Require(result.partial_predicate_implication_evidence_proven,
          "CEIC-036 partial predicate implication proof missing");
  Require(result.prefix_bounds_proven,
          "CEIC-036 prefix lower/upper bound proof missing");
  Require(!result.donor_comparison_authority,
          "CEIC-036 donor comparison must remain non-authority");
  Require(!result.enterprise_readiness_claimed,
          "CEIC-036 must not claim enterprise readiness");
  Require(!result.all_index_readiness_claimed,
          "CEIC-036 must not claim all-index readiness");
  Require(!result.ceic_037_exact_recheck_claimed,
          "CEIC-036 must not claim CEIC-037 exact recheck");
  Require(!result.ceic_040_runtime_metrics_claimed,
          "CEIC-036 must not claim CEIC-040 runtime metrics");
  Require(!result.ceic_041_crash_matrix_claimed,
          "CEIC-036 must not claim CEIC-041 crash matrix");
  Require(!result.ceic_042_readiness_drift_claimed,
          "CEIC-036 must not claim CEIC-042 readiness drift");
  Require(EvidenceHas(result, "typed_comparable_payload_contract=true"),
          "CEIC-036 typed contract evidence missing");
  Require(EvidenceHas(result, "deterministic_fuzz_vectors=320"),
          "CEIC-036 deterministic fuzz count evidence missing");
  Require(EvidenceHas(result, "donor_authority=false"),
          "CEIC-036 donor non-authority evidence missing");
  Require(EvidenceHas(result, "transaction_finality_authority=false"),
          "CEIC-036 transaction non-authority evidence missing");
}

void RequestRefusalsFailClosed() {
  auto missing_contract = ValidRequest();
  missing_contract.typed_comparable_payload_contract_declared = false;
  RequireRefused(index::ProveCeic036CanonicalKeyOrderingAndEncodingFuzz(
                     missing_contract),
                 "SB-CEIC-036-TYPED-COMPARABLE-CONTRACT-MISSING",
                 "missing typed comparable payload contract was not refused");

  auto raw_numeric = ValidRequest();
  raw_numeric.raw_textual_numeric_ordering_claimed = true;
  RequireRefused(index::ProveCeic036CanonicalKeyOrderingAndEncodingFuzz(
                     raw_numeric),
                 "SB-CEIC-036-RAW-NUMERIC-TEXT-REFUSED",
                 "raw textual numeric ordering proof was not refused");

  auto unstable_profile = ValidRequest();
  unstable_profile.semantic_profile.bytewise_stable = false;
  RequireRefused(index::ProveCeic036CanonicalKeyOrderingAndEncodingFuzz(
                     unstable_profile),
                 "SB-CEIC-036-UNSTABLE-PROFILE-REFUSED",
                 "unstable semantic profile was not refused by CEIC-036 proof");

  auto donor_authority = ValidRequest();
  donor_authority.donor_comparison_evidence_only = false;
  RequireRefused(index::ProveCeic036CanonicalKeyOrderingAndEncodingFuzz(
                     donor_authority),
                 "SB-CEIC-036-DONOR-AUTHORITY-REFUSED",
                 "donor comparison authority was not refused");
}

void AuthorityClaimsFailClosed() {
  using Claims = index::Ceic036AuthorityBoundaryClaims;
  const struct {
    bool Claims::*field;
    const char* label;
  } cases[] = {
      {&Claims::visibility_authority, "visibility authority"},
      {&Claims::authorization_security_authority, "security authority"},
      {&Claims::transaction_finality_authority, "transaction finality"},
      {&Claims::recovery_authority, "recovery authority"},
      {&Claims::parser_authority, "parser authority"},
      {&Claims::donor_authority, "donor authority"},
      {&Claims::wal_authority, "WAL authority"},
      {&Claims::benchmark_authority, "benchmark authority"},
      {&Claims::optimizer_plan_authority, "optimizer authority"},
      {&Claims::index_finality_authority, "index finality authority"},
      {&Claims::cluster_action_authority, "cluster action authority"},
      {&Claims::agent_action_authority, "agent action authority"},
  };
  for (const auto& test_case : cases) {
    auto request = ValidRequest();
    request.authority_claims.*(test_case.field) = true;
    RequireRefused(index::ProveCeic036CanonicalKeyOrderingAndEncodingFuzz(
                       request),
                   "SB-CEIC-036-AUTHORITY-CLAIM-REFUSED",
                   test_case.label);
  }
}

void EncoderAndComparatorRefusalsFailClosed() {
  auto donor_raw = Component({'r', 'a', 'w'});
  donor_raw.kind = index::IndexKeyComponentKind::donor_raw;
  const auto donor_raw_result =
      index::EncodeIndexKey({donor_raw}, StableProfile());
  Require(!donor_raw_result.ok() &&
              donor_raw_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-KEY-ENCODING-DONOR-RAW-REFUSED",
          "donor_raw key encoding was not refused");

  auto donor_nulls = Component({});
  donor_nulls.is_null = true;
  donor_nulls.null_placement =
      index::IndexKeyNullPlacement::donor_profile_default;
  const auto donor_nulls_result =
      index::EncodeIndexKey({donor_nulls}, StableProfile());
  Require(!donor_nulls_result.ok() &&
              donor_nulls_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-KEY-ENCODING-DONOR-NULLS-REFUSED",
          "donor profile default null placement was not refused");

  auto missing_type = Component({'a'});
  missing_type.type_descriptor_uuid = {};
  const auto missing_type_result =
      index::EncodeIndexKey({missing_type}, StableProfile());
  Require(!missing_type_result.ok() &&
              missing_type_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-KEY-ENCODING-TYPE-UUID-MISSING",
          "missing type UUID key encoding was not refused");

  auto unstable = StableProfile();
  unstable.bytewise_stable = false;
  const auto unstable_result =
      index::EncodeIndexKey({Component({'a'})}, unstable);
  Require(!unstable_result.ok() &&
              unstable_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-KEY-ENCODING-UNSTABLE-PROFILE",
          "unstable semantic profile encoding was not refused");

  const std::vector<byte> legacy = {'S', 'B', 'K', '1', 1, 0, 0, 0};
  const auto legacy_compare = index::CompareEncodedIndexKeys(legacy, legacy);
  Require(!legacy_compare.ok() &&
              legacy_compare.diagnostic.diagnostic_code ==
                  "SB-INDEX-KEY-COMPARE-UNSAFE-LEGACY-ENVELOPE",
          "unsafe SBK1 legacy envelope compare was not refused");

  const std::vector<byte> bad = {'b', 'a', 'd'};
  const auto bad_compare = index::CompareEncodedIndexKeys(bad, bad);
  Require(!bad_compare.ok() &&
              bad_compare.diagnostic.diagnostic_code ==
                  "SB-INDEX-KEY-COMPARE-BAD-ENVELOPE",
          "bad envelope compare was not refused");

  const auto prefix_missing_type =
      index::BuildEncodedPrefixMatcher({missing_type}, StableProfile());
  Require(!prefix_missing_type.ok() &&
              prefix_missing_type.diagnostic.diagnostic_code ==
                  "SB-INDEX-KEY-PREFIX-TYPE-UUID-MISSING",
          "missing type UUID prefix proof was not refused");
}

}  // namespace

int main() {
  ValidFuzzProofCoversRequiredOrderingSurfaces();
  RequestRefusalsFailClosed();
  AuthorityClaimsFailClosed();
  EncoderAndComparatorRefusalsFailClosed();
  std::cout << "ceic_036_canonical_key_ordering_encoding_fuzz_gate=passed\n";
  return 0;
}
