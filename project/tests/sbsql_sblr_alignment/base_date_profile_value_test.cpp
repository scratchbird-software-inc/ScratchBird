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

platform::Uuid D707() {
  return {{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x07}};
}

dt::DateValidatedProfileHandleV1 Profile() {
  const auto result = dt::BuildCurrentDateValidatedProfileHandleV1(D707());
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
  Check(dt::ValidateDateProfileHandleV1(profile).ok(), "profile validation");

  auto alias = profile;
  alias.identity.legacy_fields.canonical_name = "DATE";
  alias.identity.legacy_fields.codec_id = "localized-date-codec-label";
  Check(dt::ValidateDateProfileHandleV1(alias).ok(),
        "presentation alias changed identity");

  for (std::size_t index = 0; index < profile.profile_material.size(); ++index) {
    auto changed = profile;
    changed.profile_material[index] ^= 1;
    Check(!dt::ValidateDateProfileHandleV1(changed).ok(),
          "mutated profile byte admitted");
  }
  for (std::size_t index = 0; index < profile.comparison_material.size(); ++index) {
    auto changed = profile;
    changed.comparison_material[index] ^= 1;
    Check(!dt::ValidateDateProfileHandleV1(changed).ok(),
          "mutated comparison byte admitted");
  }
  auto changed_policy = profile;
  changed_policy.calendar_policy.generation = 2;
  Check(!dt::ValidateDateProfileHandleV1(changed_policy).ok(),
        "mutated transitive policy admitted");
}

void CanonicalValues() {
  auto profile = std::make_shared<dt::DateValidatedProfileHandleV1>(Profile());
  for (const auto& vector : kVectors) {
    const auto decoded = dt::DecodeCanonicalDateComponentNoAllocV1(
        *profile, dt::DateValueStateV1::value, true, vector.component);
    Check(decoded.ok() && decoded.value.day == vector.day,
          "canonical component decode");
    const dt::DateOwnedValueV1 owned{profile, decoded.value.state,
                                     decoded.value.day};
    const auto encoded = dt::EncodeCanonicalDateComponentV1(owned);
    Check(encoded.ok() && std::equal(encoded.bytes.begin(), encoded.bytes.end(),
                                     vector.component.begin()),
          "canonical component reencode");
    std::array<platform::byte, 4> bounded_component{{0xaa,0xaa,0xaa,0xaa}};
    const auto bounded = dt::EncodeCanonicalDateComponentIntoNoAllocV1(
        owned, bounded_component.data(), bounded_component.size());
    Check(bounded.ok() && bounded.bytes_required == 4 &&
              bounded.bytes_written == 4 && bounded_component == vector.component,
          "bounded canonical component reencode");
    const auto rendered = dt::RenderCanonicalDateV1(owned);
    Check(rendered.ok() && rendered.text == vector.text, "canonical render");
    std::array<char, 14> bounded_text{};
    const auto bounded_render = dt::RenderCanonicalDateIntoNoAllocV1(
        owned, false, bounded_text.data(), bounded_text.size());
    Check(bounded_render.ok() &&
              std::string_view(bounded_text.data(), bounded_render.bytes_written) ==
                  vector.text,
          "bounded canonical render");
    const auto parsed = dt::ParseCanonicalDateV1(profile, vector.text);
    Check(parsed.ok() && parsed.value.day == vector.day &&
              parsed.value.profile == profile,
          "canonical parse");
  }

  Check(!dt::DecodeCanonicalDateComponentNoAllocV1(
      *profile, dt::DateValueStateV1::value, true, {}).ok(), "empty VALUE component");
  const std::array<platform::byte, 3> short_component{};
  Check(!dt::DecodeCanonicalDateComponentNoAllocV1(
      *profile, dt::DateValueStateV1::value, true, short_component).ok(),
      "short VALUE component");
  const std::array<platform::byte, 5> long_component{};
  Check(!dt::DecodeCanonicalDateComponentNoAllocV1(
      *profile, dt::DateValueStateV1::value, true, long_component).ok(),
      "long VALUE component");
  Check(dt::DecodeCanonicalDateComponentNoAllocV1(
      *profile, dt::DateValueStateV1::sql_null, true, {}).ok(), "clean NULL");
  Check(!dt::DecodeCanonicalDateComponentNoAllocV1(
      *profile, dt::DateValueStateV1::sql_null, true,
      std::span<const platform::byte>(kVectors[12].component)).ok(), "dirty NULL");
  Check(!dt::DecodeCanonicalDateComponentNoAllocV1(
      *profile, dt::DateValueStateV1::sql_null, false, {}).ok(),
      "NULL admitted to nonnullable slot");
}

void BatchMemoryRules() {
  auto profile = Profile();
  const std::array<std::int32_t, 9> days{{0,0,1,2,3,4,5,6,0}};
  const std::array<platform::byte, 2> bitmap{{0x01,0x01}};
  Check(dt::ValidateDateBatchViewV1({&profile, days, bitmap}).ok(),
        "N9 LSB-first batch refused");
  auto dirty_days = days;
  dirty_days[0] = 7;
  const auto dirty = dt::ValidateDateBatchViewV1({&profile, dirty_days, bitmap});
  Check(!dirty.ok() && dirty.diagnostic.diagnostic_code ==
        "DATATYPE.NULL_STATE.INVALID", "dirty NULL slot diagnosis");
  const std::array<platform::byte, 2> dirty_tail{{0x01,0x81}};
  Check(!dt::ValidateDateBatchViewV1({&profile, days, dirty_tail}).ok(),
        "dirty bitmap tail admitted");
  Check(dt::ValidateDateBatchViewV1({&profile, {}, {}}).ok(),
        "N0 batch refused");
}

}  // namespace

int main() {
  ProfileAuthority();
  CanonicalValues();
  BatchMemoryRules();
  std::cout << "PASS base.date profile/value checks=" << checks << '\n';
  return EXIT_SUCCESS;
}
