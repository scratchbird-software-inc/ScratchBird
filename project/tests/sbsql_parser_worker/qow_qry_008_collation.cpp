// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "datatype_operations.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;

namespace scratchbird::engine::internal_api {
bool QowCompareCanonicalCollatedScalarsV1(
    const EngineTypedValue& left_value,
    const EngineTypedValue& right_value,
    const std::string& collation_uuid,
    EngineApiU64 resource_epoch,
    EngineApiU64 collation_epoch,
    const scratchbird::core::datatypes::DatatypeTextSeedAuthority& text_seed,
    int* comparison,
    std::string* refusal_detail);
}  // namespace scratchbird::engine::internal_api

namespace {

constexpr std::string_view kCollationUuid =
    "019f0000-0000-7400-8000-000000000841";

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

api::EngineDescriptor TextDescriptor(const std::string& descriptor_uuid,
                                     const std::string_view collation_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000000842;"
      "nullability=non_null;collation_uuid=" +
      std::string(collation_uuid);
  return descriptor;
}

dt::DatatypeTextSeedAuthority CollationAuthority() {
  dt::DatatypeTextSeedAuthority authority;
  authority.active = true;
  authority.seed_pack_name = "qow_core_resource_catalog";
  authority.seed_pack_version = "2026.07";
  authority.charset_name = "UTF-8";
  authority.collation_name = "unicode_ci_ai";
  authority.collation_case_insensitive = true;
  authority.collation_accent_insensitive = true;
  return authority;
}

api::EngineTypedValue TextValue(const std::string& descriptor_uuid,
                                const std::string& value) {
  api::EngineTypedValue typed;
  typed.descriptor = TextDescriptor(descriptor_uuid, kCollationUuid);
  typed.encoded_value = value;
  typed.state = api::EngineValueState::value;
  return typed;
}

bool ValidateAcceptedCollationComparison() {
  const auto left = TextValue(
      "019f0000-0000-7200-8000-000000000843", "R\xC3\xA9sum\xC3\xA9");
  const auto right = TextValue(
      "019f0000-0000-7200-8000-000000000844", "resume");
  int comparison = 99;
  std::string refusal;
  const bool accepted = api::QowCompareCanonicalCollatedScalarsV1(
      left, right, std::string(kCollationUuid), 31, 17,
      CollationAuthority(), &comparison, &refusal);
  bool passed = true;
  passed &= Require(accepted && refusal.empty(),
                    "valid bound collation comparison was refused");
  passed &= Require(comparison == 0,
                    "case/accent-insensitive core collation was not applied");
  return passed;
}

bool ValidateCollationRefusal() {
  const auto left = TextValue(
      "019f0000-0000-7200-8000-000000000845", "alpha");
  auto mismatched = TextValue(
      "019f0000-0000-7200-8000-000000000846", "ALPHA");
  mismatched.descriptor = TextDescriptor(
      mismatched.descriptor.descriptor_uuid.canonical,
      "019f0000-0000-7400-8000-000000000847");
  int comparison = 0;
  std::string refusal;
  const bool mismatch_accepted = api::QowCompareCanonicalCollatedScalarsV1(
      left, mismatched, std::string(kCollationUuid), 31, 17,
      CollationAuthority(), &comparison, &refusal);

  auto incomplete_authority = CollationAuthority();
  incomplete_authority.seed_pack_version.clear();
  refusal.clear();
  const bool incomplete_accepted = api::QowCompareCanonicalCollatedScalarsV1(
      left,
      TextValue("019f0000-0000-7200-8000-000000000848", "alpha"),
      std::string(kCollationUuid), 31, 17, incomplete_authority, &comparison,
      &refusal);

  auto duplicate = TextValue(
      "019f0000-0000-7200-8000-000000000849", "alpha");
  duplicate.descriptor.encoded_descriptor +=
      ";collation_uuid=" + std::string(kCollationUuid);
  refusal.clear();
  const bool duplicate_accepted = api::QowCompareCanonicalCollatedScalarsV1(
      left, duplicate, std::string(kCollationUuid), 31, 17,
      CollationAuthority(), &comparison, &refusal);

  auto missing_state = TextValue(
      "019f0000-0000-7200-8000-000000000850", "alpha");
  missing_state.state = api::EngineValueState::missing;
  refusal.clear();
  const bool missing_accepted = api::QowCompareCanonicalCollatedScalarsV1(
      left, missing_state, std::string(kCollationUuid), 31, 17,
      CollationAuthority(), &comparison, &refusal);

  bool passed = true;
  passed &= Require(!mismatch_accepted,
                    "mismatched bound collation UUID was accepted");
  passed &= Require(!incomplete_accepted,
                    "incomplete collation resource authority was accepted");
  passed &= Require(!duplicate_accepted,
                    "duplicate collation descriptor field was accepted");
  passed &= Require(!missing_accepted,
                    "missing value state was compared as character data");
  return passed;
}

}  // namespace

// QOW-TEST-QRY-008-COLLATION-V1
int main() {
  bool passed = true;
  passed &= ValidateAcceptedCollationComparison();
  passed &= ValidateCollationRefusal();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
