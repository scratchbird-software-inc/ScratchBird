// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace engine = scratchbird::engine;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

engine::ExecutionTypeDescriptor Descriptor(
    std::uint8_t seed,
    std::string_view name,
    engine::ExecutionTypeFamily family,
    std::uint32_t bit_width,
    bool nullable = true,
    std::uint32_t length = 0) {
  engine::ExecutionTypeDescriptor descriptor;
  for (std::size_t index = 0; index < 16; ++index) {
    descriptor.descriptor_uuid.bytes[index] =
        static_cast<std::uint8_t>(seed + index);
  }
  descriptor.descriptor_epoch = 7;
  descriptor.canonical_type_id = seed;
  descriptor.family = family;
  descriptor.width_class =
      length == 0 ? engine::ExecutionTypeWidthClass::fixed
                  : engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  descriptor.bit_width = bit_width;
  descriptor.length = length;
  descriptor.nullable_allowed = nullable;
  return descriptor;
}

engine::ExecutionTypeDescriptor DomainDescriptor(
    std::uint8_t seed,
    std::string_view name,
    engine::ExecutionTypeFamily family,
    std::uint32_t bit_width) {
  auto descriptor = Descriptor(seed, name, family, bit_width);
  descriptor.modifier_flags |=
      engine::ExecutionTypeModifierFlagBit(
          engine::ExecutionTypeModifierFlag::domain_uuid);
  for (std::size_t index = 0; index < 16; ++index) {
    descriptor.domain_uuid.bytes[index] =
        static_cast<std::uint8_t>(seed + 0x30 + index);
  }
  return descriptor;
}

engine::ExecutionCoercionRequest Request(
    const engine::ExecutionTypeDescriptor& source,
    const engine::ExecutionTypeDescriptor& target,
    engine::ExecutionCoercionCategory category) {
  engine::ExecutionCoercionRequest request;
  request.source_descriptor = source;
  request.target_descriptor = target;
  request.category = category;
  return request;
}

void RequireOk(const engine::ExecutionCoercionRequest& request,
               engine::ExecutionCoercionCategory expected,
               std::string_view message) {
  const auto result = engine::convertTo(request);
  Require(result.ok(), message);
  Require(result.accepted_category == expected,
          "EDR-007 accepted coercion category mismatch");
  Require(result.stableFailureIdentity() == "OK",
          "EDR-007 successful coercion did not return stable OK identity");
}

void RequireFailure(const engine::ExecutionCoercionRequest& request,
                    engine::ExecutionCoercionFailureIdentity expected,
                    std::string_view expected_code,
                    std::string_view message) {
  const auto result = engine::ValidateExecutionCoercionRequest(request);
  Require(!result.ok(), message);
  Require(result.failure_identity == expected,
          "EDR-007 coercion failure identity mismatch");
  Require(result.stableFailureIdentity() == expected_code,
          "EDR-007 coercion stable failure code mismatch");
}

void TestIdentityAndDescriptorFailures() {
  const auto int32 =
      Descriptor(0x10, "int32", engine::ExecutionTypeFamily::signed_integer, 32);
  auto request = Request(int32, int32,
                         engine::ExecutionCoercionCategory::identity);
  RequireOk(request, engine::ExecutionCoercionCategory::identity,
            "EDR-007 rejected descriptor-identity coercion");

  auto int32_epoch_8 = int32;
  int32_epoch_8.descriptor_epoch = 8;
  request = Request(int32, int32_epoch_8,
                    engine::ExecutionCoercionCategory::identity);
  RequireFailure(
      request,
      engine::ExecutionCoercionFailureIdentity::descriptor_identity_mismatch,
      "DESCRIPTOR_IDENTITY_MISMATCH",
      "EDR-007 accepted identity coercion across descriptor identity mismatch");

  auto invalid_source = int32;
  invalid_source.descriptor_uuid = {};
  request = Request(invalid_source, int32,
                    engine::ExecutionCoercionCategory::identity);
  const auto source_result = engine::ValidateExecutionCoercionRequest(request);
  Require(!source_result.ok(), "EDR-007 accepted invalid source descriptor");
  Require(source_result.failure_identity ==
              engine::ExecutionCoercionFailureIdentity::source_descriptor_invalid,
          "EDR-007 source descriptor failure identity mismatch");
  Require(source_result.descriptor_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
          "EDR-007 source descriptor diagnostic was not preserved");

  auto invalid_target = int32;
  invalid_target.descriptor_epoch = 0;
  request = Request(int32, invalid_target,
                    engine::ExecutionCoercionCategory::identity);
  const auto target_result = engine::ValidateExecutionCoercionRequest(request);
  Require(!target_result.ok(), "EDR-007 accepted invalid target descriptor");
  Require(target_result.failure_identity ==
              engine::ExecutionCoercionFailureIdentity::target_descriptor_invalid,
          "EDR-007 target descriptor failure identity mismatch");
  Require(target_result.descriptor_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
          "EDR-007 target descriptor diagnostic was not preserved");
}

void TestValueStateAndNullabilityFailures() {
  const auto nullable_int =
      Descriptor(0x20, "nullable-int", engine::ExecutionTypeFamily::signed_integer,
                 32);
  const auto non_nullable_int =
      Descriptor(0x21, "non-null-int",
                 engine::ExecutionTypeFamily::signed_integer, 32, false);

  auto request = Request(nullable_int, nullable_int,
                         engine::ExecutionCoercionCategory::lossless_implicit);
  request.source_state = engine::ExecutionValueState::sql_null;
  RequireOk(request, engine::ExecutionCoercionCategory::lossless_implicit,
            "EDR-007 rejected nullable SQL null coercion");

  request = Request(nullable_int, non_nullable_int,
                    engine::ExecutionCoercionCategory::lossless_implicit);
  request.source_state = engine::ExecutionValueState::sql_null;
  RequireFailure(
      request,
      engine::ExecutionCoercionFailureIdentity::target_nullability_violation,
      "TARGET_NULLABILITY_VIOLATION",
      "EDR-007 accepted SQL null coercion into non-nullable target");

  request = Request(nullable_int, nullable_int,
                    engine::ExecutionCoercionCategory::lossless_implicit);
  request.source_state = static_cast<engine::ExecutionValueState>(0xff);
  RequireFailure(
      request,
      engine::ExecutionCoercionFailureIdentity::source_value_state_invalid,
      "SOURCE_VALUE_STATE_INVALID",
      "EDR-007 accepted invalid source value-state code");

  request = Request(nullable_int, nullable_int,
                    engine::ExecutionCoercionCategory::lossless_implicit);
  request.source_state = engine::ExecutionValueState::missing;
  RequireFailure(
      request,
      engine::ExecutionCoercionFailureIdentity::source_value_state_not_coercible,
      "SOURCE_VALUE_STATE_NOT_COERCIBLE",
      "EDR-007 accepted non-coercible source value state");
}

void TestLosslessAndLossyCoercionRules() {
  const auto int32 =
      Descriptor(0x30, "int32", engine::ExecutionTypeFamily::signed_integer, 32);
  const auto int64 =
      Descriptor(0x31, "int64", engine::ExecutionTypeFamily::signed_integer, 64);
  const auto real64 =
      Descriptor(0x32, "real64", engine::ExecutionTypeFamily::real, 64);

  auto request = Request(int32, int64,
                         engine::ExecutionCoercionCategory::lossless_implicit);
  RequireOk(request, engine::ExecutionCoercionCategory::lossless_implicit,
            "EDR-007 rejected descriptor-based integer widening");

  request = Request(int64, int32,
                    engine::ExecutionCoercionCategory::lossless_implicit);
  RequireFailure(
      request,
      engine::ExecutionCoercionFailureIdentity::coercion_forbidden,
      "COERCION_FORBIDDEN",
      "EDR-007 accepted narrowing as lossless implicit coercion");

  request = Request(int32, int64,
                    engine::ExecutionCoercionCategory::lossless_explicit);
  RequireFailure(request,
                 engine::ExecutionCoercionFailureIdentity::explicit_cast_required,
                 "EXPLICIT_CAST_REQUIRED",
                 "EDR-007 accepted explicit lossless coercion without context");

  request.context.explicit_cast = true;
  RequireOk(request, engine::ExecutionCoercionCategory::lossless_explicit,
            "EDR-007 rejected explicit descriptor-based widening");

  request = Request(int64, int32,
                    engine::ExecutionCoercionCategory::lossy_explicit);
  request.context.explicit_cast = true;
  RequireFailure(request,
                 engine::ExecutionCoercionFailureIdentity::lossy_cast_not_allowed,
                 "LOSSY_CAST_NOT_ALLOWED",
                 "EDR-007 accepted lossy cast without lossy context");

  request.context.allow_lossy = true;
  RequireOk(request, engine::ExecutionCoercionCategory::lossy_explicit,
            "EDR-007 rejected explicit lossy narrowing with lossy context");

  request = Request(int32, real64,
                    engine::ExecutionCoercionCategory::lossy_explicit);
  request.context.explicit_cast = true;
  request.context.allow_lossy = true;
  RequireOk(request, engine::ExecutionCoercionCategory::lossy_explicit,
            "EDR-007 rejected explicit numeric cross-family cast context");
}

void TestCharacterLengthAndTextFailureIdentity() {
  const auto varchar_8 =
      Descriptor(0x40, "varchar8", engine::ExecutionTypeFamily::character, 0,
                 true, 8);
  const auto varchar_16 =
      Descriptor(0x41, "varchar16", engine::ExecutionTypeFamily::character, 0,
                 true, 16);

  auto request = Request(varchar_8, varchar_16,
                         engine::ExecutionCoercionCategory::lossless_implicit);
  RequireOk(request, engine::ExecutionCoercionCategory::lossless_implicit,
            "EDR-007 rejected character length widening");

  request = Request(varchar_16, varchar_8,
                    engine::ExecutionCoercionCategory::lossless_implicit);
  RequireFailure(
      request,
      engine::ExecutionCoercionFailureIdentity::coercion_forbidden,
      "COERCION_FORBIDDEN",
      "EDR-007 accepted character truncation as lossless coercion");

  request = Request(varchar_8, varchar_16,
                    engine::ExecutionCoercionCategory::lossless_implicit);
  request.context.source_payload_text_valid = false;
  RequireFailure(
      request,
      engine::ExecutionCoercionFailureIdentity::invalid_text_representation,
      "INVALID_TEXT_REPRESENTATION",
      "EDR-007 failed to preserve invalid text representation identity");
}

void TestDonorAndDomainContexts() {
  const auto int32 =
      Descriptor(0x50, "int32", engine::ExecutionTypeFamily::signed_integer, 32);
  const auto donor_text =
      Descriptor(0x51, "donor-text", engine::ExecutionTypeFamily::character, 0,
                 true, 32);

  auto request =
      Request(int32, donor_text,
              engine::ExecutionCoercionCategory::donor_compatibility_explicit);
  RequireFailure(request,
                 engine::ExecutionCoercionFailureIdentity::explicit_cast_required,
                 "EXPLICIT_CAST_REQUIRED",
                 "EDR-007 accepted donor compatibility without explicit cast");

  request.context.explicit_cast = true;
  RequireFailure(request,
                 engine::ExecutionCoercionFailureIdentity::donor_profile_required,
                 "DONOR_PROFILE_REQUIRED",
                 "EDR-007 accepted donor compatibility without donor profile");

  request.context.donor_compatibility_profile = true;
  RequireOk(
      request,
      engine::ExecutionCoercionCategory::donor_compatibility_explicit,
      "EDR-007 rejected donor compatibility with explicit donor profile");

  const auto domain_int =
      DomainDescriptor(0x60, "positive-int",
                       engine::ExecutionTypeFamily::signed_integer, 32);
  auto base_int =
      Descriptor(0x61, "base-int", engine::ExecutionTypeFamily::signed_integer,
                 32);

  request = Request(domain_int, base_int,
                    engine::ExecutionCoercionCategory::domain_to_base);
  RequireFailure(
      request,
      engine::ExecutionCoercionFailureIdentity::domain_boundary_not_allowed,
      "DOMAIN_BOUNDARY_NOT_ALLOWED",
      "EDR-007 accepted domain-to-base without domain boundary context");

  request.context.allow_domain_boundary = true;
  RequireOk(request, engine::ExecutionCoercionCategory::domain_to_base,
            "EDR-007 rejected descriptor-backed domain-to-base context");

  request = Request(base_int, domain_int,
                    engine::ExecutionCoercionCategory::base_to_domain);
  request.context.allow_domain_boundary = true;
  RequireOk(request, engine::ExecutionCoercionCategory::base_to_domain,
            "EDR-007 rejected descriptor-backed base-to-domain context");

  request = Request(base_int, base_int,
                    engine::ExecutionCoercionCategory::base_to_domain);
  request.context.allow_domain_boundary = true;
  RequireFailure(request,
                 engine::ExecutionCoercionFailureIdentity::domain_metadata_required,
                 "DOMAIN_METADATA_REQUIRED",
                 "EDR-007 accepted base-to-domain without domain metadata");
}

void TestForbiddenCategory() {
  const auto int32 =
      Descriptor(0x70, "int32", engine::ExecutionTypeFamily::signed_integer, 32);
  const auto request =
      Request(int32, int32, engine::ExecutionCoercionCategory::forbidden);
  RequireFailure(request,
                 engine::ExecutionCoercionFailureIdentity::coercion_forbidden,
                 "COERCION_FORBIDDEN",
                 "EDR-007 accepted explicitly forbidden coercion");
}

}  // namespace

int main() {
  TestIdentityAndDescriptorFailures();
  TestValueStateAndNullabilityFailures();
  TestLosslessAndLossyCoercionRules();
  TestCharacterLengthAndTextFailureIdentity();
  TestDonorAndDomainContexts();
  TestForbiddenCategory();
  return EXIT_SUCCESS;
}
