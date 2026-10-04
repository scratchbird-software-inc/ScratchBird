// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_date.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
namespace dt = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;

unsigned checks = 0;
[[noreturn]] void Fail(std::string_view text) {
  std::cerr << "FAIL: " << text << '\n';
  std::exit(EXIT_FAILURE);
}
void Check(bool condition, std::string_view text) {
  ++checks;
  if (!condition) Fail(text);
}

platform::Uuid D708() {
  return {{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x08}};
}

dt::DateValidatedProfileHandleV3 Profile() {
  const auto result = dt::BuildCurrentDateValidatedProfileHandleV3(D708());
  Check(result.ok(), "current profile construction failed");
  return result.profile;
}

struct Vector {
  std::int32_t day;
  std::string_view text;
  std::array<platform::byte, 4> component;
};

constexpr std::array<Vector, 20> kVectors{{
    {std::numeric_limits<std::int32_t>::min(), "-5877641-06-23", {0x00,0x00,0x00,0x80}},
    {-2147483647, "-5877641-06-24", {0x01,0x00,0x00,0x80}},
    {-865566, "-0000400-02-29", {0xe2,0xca,0xf2,0xff}},
    {-755993, "-0000100-03-01", {0xe7,0x76,0xf4,0xff}},
    {-720930, "-0000004-02-29", {0xde,0xff,0xf4,0xff}},
    {-719893, "-0000001-01-01", {0xeb,0x03,0xf5,0xff}},
    {-719528, "0000-01-01", {0x58,0x05,0xf5,0xff}},
    {-719469, "0000-02-29", {0x93,0x05,0xf5,0xff}},
    {-719162, "0001-01-01", {0xc6,0x06,0xf5,0xff}},
    {-135081, "1600-02-29", {0x57,0xf0,0xfd,0xff}},
    {-25508, "1900-03-01", {0x5c,0x9c,0xff,0xff}},
    {-1, "1969-12-31", {0xff,0xff,0xff,0xff}},
    {0, "1970-01-01", {0x00,0x00,0x00,0x00}},
    {1, "1970-01-02", {0x01,0x00,0x00,0x00}},
    {11016, "2000-02-29", {0x08,0x2b,0x00,0x00}},
    {18628, "2021-01-01", {0xc4,0x48,0x00,0x00}},
    {2932896, "9999-12-31", {0xa0,0xc0,0x2c,0x00}},
    {2932897, "+0010000-01-01", {0xa1,0xc0,0x2c,0x00}},
    {2147483646, "+5881580-07-10", {0xfe,0xff,0xff,0x7f}},
    {std::numeric_limits<std::int32_t>::max(), "+5881580-07-11", {0xff,0xff,0xff,0x7f}},
}};

void ProfileAuthority() {
  auto profile = Profile();
  Check(profile.profile_material.size() == 584, "profile material extent");
  Check(profile.comparison_material.size() == 344, "comparison material extent");
  Check(dt::ValidateDateProfileHandleV3(profile).ok(), "profile validation");

  auto alias = profile;
  alias.identity.legacy_fields.canonical_name = "DATE";
  alias.identity.legacy_fields.codec_id = "localized-date-codec-label";
  Check(dt::ValidateDateProfileHandleV3(alias).ok(),
        "presentation alias changed identity");

  for (std::size_t index = 0; index < profile.profile_material.size(); ++index) {
    auto changed = profile;
    changed.profile_material[index] ^= 1;
    Check(!dt::ValidateDateProfileHandleV3(changed).ok(),
          "mutated profile byte admitted");
  }
  for (std::size_t index = 0; index < profile.comparison_material.size(); ++index) {
    auto changed = profile;
    changed.comparison_material[index] ^= 1;
    Check(!dt::ValidateDateProfileHandleV3(changed).ok(),
          "mutated comparison byte admitted");
  }
  auto changed_policy = profile;
  changed_policy.calendar_policy.generation = 2;
  Check(!dt::ValidateDateProfileHandleV3(changed_policy).ok(),
        "mutated transitive policy admitted");
}

void CanonicalValues() {
  auto profile = std::make_shared<dt::DateValidatedProfileHandleV3>(Profile());
  for (const auto& vector : kVectors) {
    const auto decoded = dt::DecodeCanonicalDateComponentNoAllocV3(
        *profile, dt::DateValueStateV3::value, true, vector.component);
    Check(decoded.ok() && decoded.value.day == vector.day,
          "canonical component decode");
    const dt::DateOwnedValueV3 owned{profile, decoded.value.state,
                                     decoded.value.day};
    const auto encoded = dt::EncodeCanonicalDateComponentV3(owned);
    Check(encoded.ok() && std::equal(encoded.bytes.begin(), encoded.bytes.end(),
                                     vector.component.begin()),
          "canonical component reencode");
    std::array<platform::byte, 4> bounded_component{{0xaa,0xaa,0xaa,0xaa}};
    const auto bounded = dt::EncodeCanonicalDateComponentIntoNoAllocV3(
        owned, bounded_component.data(), bounded_component.size());
    Check(bounded.ok() && bounded.bytes_required == 4 &&
              bounded.bytes_written == 4 && bounded_component == vector.component,
          "bounded canonical component reencode");
    const auto rendered = dt::RenderCanonicalDateV3(owned);
    Check(rendered.ok() && rendered.text == vector.text, "canonical render");
    std::array<char, 14> bounded_text{};
    const auto bounded_render = dt::RenderCanonicalDateIntoNoAllocV3(
        owned, false, bounded_text.data(), bounded_text.size());
    Check(bounded_render.ok() &&
              std::string_view(bounded_text.data(), bounded_render.bytes_written) ==
                  vector.text,
          "bounded canonical render");
    const auto parsed = dt::ParseCanonicalDateV3(profile, vector.text);
    Check(parsed.ok() && parsed.value.day == vector.day &&
              parsed.value.profile == profile,
          "canonical parse");
  }

  Check(!dt::DecodeCanonicalDateComponentNoAllocV3(
      *profile, dt::DateValueStateV3::value, true, {}).ok(), "empty VALUE component");
  const std::array<platform::byte, 3> short_component{};
  Check(!dt::DecodeCanonicalDateComponentNoAllocV3(
      *profile, dt::DateValueStateV3::value, true, short_component).ok(),
      "short VALUE component");
  const std::array<platform::byte, 5> long_component{};
  Check(!dt::DecodeCanonicalDateComponentNoAllocV3(
      *profile, dt::DateValueStateV3::value, true, long_component).ok(),
      "long VALUE component");
  Check(dt::DecodeCanonicalDateComponentNoAllocV3(
      *profile, dt::DateValueStateV3::sql_null, true, {}).ok(), "clean NULL");
  Check(!dt::DecodeCanonicalDateComponentNoAllocV3(
      *profile, dt::DateValueStateV3::sql_null, true,
      std::span<const platform::byte>(kVectors[12].component)).ok(), "dirty NULL");
  Check(!dt::DecodeCanonicalDateComponentNoAllocV3(
      *profile, dt::DateValueStateV3::sql_null, false, {}).ok(),
      "NULL admitted to nonnullable slot");
}

void DynamicOperandAdmission() {
  auto profile = std::make_shared<dt::DateValidatedProfileHandleV3>(Profile());
  for (const std::int64_t day : {
           static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
           std::int64_t{0},
           static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())}) {
    const dt::DateOperandV3 operand{
        profile, dt::DateValueStateV3::value,
        dt::DateDayCarrierKindV3::signed_i32, day};
    const auto admitted = dt::AdmitDateOperandV3(operand);
    Check(admitted.ok() && admitted.value.profile == profile.get() &&
              admitted.value.state == dt::DateValueStateV3::value &&
              admitted.value.day == static_cast<std::int32_t>(day),
          "dynamic signed-i32 operand refused");
  }

  constexpr std::array<dt::DateDayCarrierKindV3, 5> invalid_carriers{{
      dt::DateDayCarrierKindV3::wrong_host_type,
      dt::DateDayCarrierKindV3::wrong_host_boolean,
      dt::DateDayCarrierKindV3::wrong_host_string,
      dt::DateDayCarrierKindV3::below_i32,
      dt::DateDayCarrierKindV3::above_i32,
  }};
  for (const auto carrier : invalid_carriers) {
    std::int64_t day = 0;
    if (carrier == dt::DateDayCarrierKindV3::below_i32)
      day = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) - 1;
    if (carrier == dt::DateDayCarrierKindV3::above_i32)
      day = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1;
    const auto refused = dt::AdmitDateOperandV3(
        {profile, dt::DateValueStateV3::value, carrier, day});
    Check(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID" &&
              refused.value.profile == nullptr && refused.value.day == 0,
          "invalid dynamic date carrier was published");
  }

  auto invalid_profile = std::make_shared<dt::DateValidatedProfileHandleV3>(*profile);
  invalid_profile->profile_material[0] ^= 1;
  const auto bad_profile = dt::AdmitDateOperandV3(
      {invalid_profile, dt::DateValueStateV3::value,
       dt::DateDayCarrierKindV3::wrong_host_type, 0});
  Check(!bad_profile.ok() &&
            bad_profile.diagnostic.diagnostic_code ==
                "CTI.TEMPORAL.DESCRIPTOR_INVALID",
        "dynamic operand did not give profile authority precedence");

  const auto bad_state = dt::AdmitDateOperandV3(
      {profile, static_cast<dt::DateValueStateV3>(0xff),
       dt::DateDayCarrierKindV3::wrong_host_type, 0});
  Check(!bad_state.ok() &&
            bad_state.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID",
        "dynamic operand did not give state precedence over payload");

  const auto clean_null = dt::AdmitDateOperandV3(
      {profile, dt::DateValueStateV3::sql_null,
       dt::DateDayCarrierKindV3::wrong_host_type, 0});
  Check(clean_null.ok() &&
            clean_null.value.state == dt::DateValueStateV3::sql_null &&
            clean_null.value.day == 0,
        "clean dynamic NULL inspected an absent payload");
  const auto dirty_null = dt::AdmitDateOperandV3(
      {profile, dt::DateValueStateV3::sql_null,
       dt::DateDayCarrierKindV3::signed_i32, 1});
  Check(!dirty_null.ok() &&
            dirty_null.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID" &&
            dirty_null.value.profile == nullptr,
        "dirty dynamic NULL was published");
}

void BatchMemoryRules() {
  auto profile = Profile();
  const std::array<std::int32_t, 9> days{{0,0,1,2,3,4,5,6,0}};
  const std::array<platform::byte, 2> bitmap{{0x01,0x01}};
  Check(dt::ValidateDateBatchViewV3({&profile, days, bitmap}).ok(),
        "N9 LSB-first batch refused");
  auto dirty_days = days;
  dirty_days[0] = 7;
  const auto dirty = dt::ValidateDateBatchViewV3({&profile, dirty_days, bitmap});
  Check(!dirty.ok() && dirty.diagnostic.diagnostic_code ==
        "DATATYPE.NULL_STATE.INVALID", "dirty NULL slot diagnosis");
  const std::array<platform::byte, 2> dirty_tail{{0x01,0x81}};
  Check(!dt::ValidateDateBatchViewV3({&profile, days, dirty_tail}).ok(),
        "dirty bitmap tail admitted");
  Check(dt::ValidateDateBatchViewV3({&profile, {}, {}}).ok(),
        "N0 batch refused");
}

}  // namespace

int main() {
  ProfileAuthority();
  CanonicalValues();
  DynamicOperandAdmission();
  BatchMemoryRules();
  std::cout << "PASS base.date profile/value checks=" << checks << '\n';
  return EXIT_SUCCESS;
}
