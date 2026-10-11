// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/timestamp_successor_fixture.hpp"
#include "../../../src/core/datatypes/datatype_timestamp.hpp"

#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string_view>

namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;

namespace {

unsigned checks = 0;

void Check(bool condition, std::string_view message) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

p::Uuid D710() {
  return p::Uuid{{0x01, 0x9d, 0, 0, 0, 0, 0x70, 0,
                  0x80, 0, 0, 0, 0, 0, 0xd7, 0x10}};
}

std::shared_ptr<const dt::TimestampValidatedProfileHandleV3> Profile() {
  auto built = dt::BuildCurrentTimestampValidatedProfileHandleV3(D710());
  Check(built.ok(), "build d710 timestamp profile");
  return std::make_shared<const dt::TimestampValidatedProfileHandleV3>(
      std::move(built.profile));
}

std::array<p::byte, 32> Hex32(std::string_view text) {
  std::array<p::byte, 32> out{};
  auto nibble = [](char c) -> unsigned {
    return c <= '9' ? static_cast<unsigned>(c - '0')
                    : static_cast<unsigned>((c | 32) - 'a' + 10);
  };
  Check(text.size() == out.size() * 2, "SHA-256 fixture width");
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<p::byte>((nibble(text[i * 2]) << 4) |
                                  nibble(text[i * 2 + 1]));
  }
  return out;
}

struct Vector {
  dt::TimestampValueStateV3 state;
  std::string_view text;
  std::int32_t civil_day;
  p::u64 nanoseconds_since_midnight;
  std::string_view component_hex;
};

constexpr std::array<Vector, 21> kVectors{{
    {dt::TimestampValueStateV3::value, "-5877641-06-23T00:00:00",
     INT32_MIN, 0, "000000004057ffff0000000000000000"},
    {dt::TimestampValueStateV3::value,
     "-5877641-06-23T23:59:59.999999999", INT32_MIN,
     86'399'999'999'999ull, "7f5101004057ffffffc99a3b00000000"},
    {dt::TimestampValueStateV3::value, "0000-01-01T00:00:00", -719528, 0,
     "00848b86f1ffffff0000000000000000"},
    {dt::TimestampValueStateV3::value, "0000-02-29T12:00:00", -719469,
     43'200'000'000'000ull, "40f5d986f1ffffff0000000000000000"},
    {dt::TimestampValueStateV3::value,
     "1969-12-31T23:59:59.999999999", -1, 86'399'999'999'999ull,
     "ffffffffffffffffffc99a3b00000000"},
    {dt::TimestampValueStateV3::value, "1970-01-01T00:00:00", 0, 0,
     "00000000000000000000000000000000"},
    {dt::TimestampValueStateV3::value, "1970-01-01T00:00:00.000000001", 0,
     1, "00000000000000000100000000000000"},
    {dt::TimestampValueStateV3::value, "1970-01-01T00:00:00.1234", 0,
     123'400'000, "000000000000000040ef5a0700000000"},
    {dt::TimestampValueStateV3::value, "1972-02-29T00:00:00", 789, 0,
     "802f1004000000000000000000000000"},
    {dt::TimestampValueStateV3::value, "1900-02-28T23:59:59", -25509,
     86'399'000'000'000ull, "ff49a37cffffffff0000000000000000"},
    {dt::TimestampValueStateV3::value, "2000-02-29T12:34:56.1234", 11016,
     45'296'123'400'000ull, "f0bcbb380000000040ef5a0700000000"},
    {dt::TimestampValueStateV3::value,
     "2000-12-31T23:59:59.999999999", 11322, 86'399'999'999'999ull,
     "7fc84f3a00000000ffc99a3b00000000"},
    {dt::TimestampValueStateV3::value, "2001-01-01T00:00:00", 11323, 0,
     "80c84f3a000000000000000000000000"},
    {dt::TimestampValueStateV3::value, "2024-06-30T12:00:00", 19904,
     43'200'000'000'000ull, "c0488166000000000000000000000000"},
    {dt::TimestampValueStateV3::value, "2024-02-29T06:07:08.9", 19782,
     22'028'900'000'000ull, "0c1fe0650000000000e9a43500000000"},
    {dt::TimestampValueStateV3::value, "2025-01-02T03:04:05.1", 20090,
     11'045'100'000'000ull, "250276670000000000e1f50500000000"},
    {dt::TimestampValueStateV3::value, "2025-01-02T03:04:05.12345678",
     20090, 11'045'123'456'780ull,
     "25027667000000000ccd5b0700000000"},
    {dt::TimestampValueStateV3::value,
     "9999-12-31T23:59:59.999999999", 2932896, 86'399'999'999'999ull,
     "7f41f4ff3a000000ffc99a3b00000000"},
    {dt::TimestampValueStateV3::value, "+0010000-01-01T00:00:00", 2932897,
     0, "8041f4ff3a0000000000000000000000"},
    {dt::TimestampValueStateV3::value,
     "+5881580-07-11T23:59:59.999999999", INT32_MAX,
     86'399'999'999'999ull, "ffffffffbfa80000ffc99a3b00000000"},
    {dt::TimestampValueStateV3::sql_null, "", 0, 0, ""},
}};

std::vector<p::byte> Hex(std::string_view text) {
  std::vector<p::byte> out(text.size() / 2);
  auto nibble = [](char c) -> unsigned {
    return c <= '9' ? static_cast<unsigned>(c - '0')
                    : static_cast<unsigned>((c | 32) - 'a' + 10);
  };
  Check((text.size() & 1u) == 0, "component hex width");
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<p::byte>((nibble(text[i * 2]) << 4) |
                                  nibble(text[i * 2 + 1]));
  }
  return out;
}

void ProfileAuthority(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile) {
  Check(profile->profile_material.size() == 656 &&
            std::memcmp(profile->profile_material.data(), "SBTSPP01", 8) == 0,
        "exact 656-byte SBTSPP01 material");
  Check(profile->comparison_material.size() == 416 &&
            std::memcmp(profile->comparison_material.data(), "SBTSPC01", 8) == 0,
        "exact 416-byte SBTSPC01 material");
  const bool successor = profile->receipt.catalog_generation == 11;
  Check(profile->profile_fingerprint == Hex32(successor ?
            "b3f2a0e9d7c564c93754f4471b3710b7e9215216987166e5c3f16fbbe8b226d8" :
            "4aa13879ac9d345ceb2b17e1068f0a0049ff45508d76f4f5af5c6ddc1d07a3ca"),
        "exact timestamp profile fingerprint");
  Check(profile->comparison_fingerprint == Hex32(successor ?
            "879004814d3fc6583cc0de3263cd72da1d97e15bb06d942609056f58ca9c2da4" :
            "35ce26afa08d48af46c79e08cd056a453b7deba80d3569e78ffdba5b1c93fee7"),
        "exact timestamp comparison fingerprint");
  Check(dt::ValidateTimestampProfileHandleV3(*profile).ok(),
        "exact profile validates");

  for (std::size_t i = 0; i < profile->profile_material.size(); ++i) {
    auto changed = *profile;
    changed.profile_material[i] ^= 1;
    Check(!dt::ValidateTimestampProfileHandleV3(changed).ok(),
          "all 656 profile byte mutations refuse");
  }
  for (std::size_t i = 0; i < profile->comparison_material.size(); ++i) {
    auto changed = *profile;
    changed.comparison_material[i] ^= 1;
    Check(!dt::ValidateTimestampProfileHandleV3(changed).ok(),
          "all 416 comparison byte mutations refuse");
  }
}

void CanonicalValues(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile) {
  for (const auto& fixture : kVectors) {
    auto bytes = Hex(fixture.component_hex);
    const auto decoded = dt::DecodeCanonicalTimestampComponentNoAllocV3(
        *profile, fixture.state, true, bytes);
    Check(decoded.ok() && decoded.value.civil_day == fixture.civil_day &&
              decoded.value.nanoseconds_since_midnight ==
                  fixture.nanoseconds_since_midnight,
          "canonical LE16 decode vector");

    dt::TimestampOwnedValueV3 value{profile, fixture.state, fixture.civil_day,
                                    fixture.nanoseconds_since_midnight};
    const auto encoded = dt::EncodeCanonicalTimestampComponentV3(value);
    Check(encoded.ok() && encoded.bytes == bytes,
          "canonical LE16 encode vector");
    const auto rendered = dt::RenderCanonicalTimestampV3(value);
    Check(rendered.ok() && rendered.text == fixture.text,
          "canonical local-civil render vector");
    if (fixture.state == dt::TimestampValueStateV3::value) {
      const auto parsed = dt::ParseCanonicalTimestampV3(profile, fixture.text);
      Check(parsed.ok() && parsed.value.civil_day == fixture.civil_day &&
                parsed.value.nanoseconds_since_midnight ==
                    fixture.nanoseconds_since_midnight,
            "canonical local-civil parse vector");
    }
  }

  // This is the mandatory Euclidean-floor seam. Truncating division would
  // decode the civil second -1 as day zero with a negative remainder.
  const auto before_epoch = Hex("ffffffffffffffffffc99a3b00000000");
  const auto decoded = dt::DecodeCanonicalTimestampComponentNoAllocV3(
      *profile, dt::TimestampValueStateV3::value, true, before_epoch);
  Check(decoded.ok() && decoded.value.civil_day == -1 &&
            decoded.value.nanoseconds_since_midnight == 86'399'999'999'999ull,
        "negative civil-second decode uses Euclidean floor division");

  std::array<p::byte, 16> bad{};
  p::StoreLittle32(bad.data() + 8, 1'000'000'000u);
  Check(!dt::DecodeCanonicalTimestampComponentNoAllocV3(
             *profile, dt::TimestampValueStateV3::value, true, bad)
             .ok(),
        "nanosecond-of-second upper bound");
  bad.fill(0);
  bad[12] = 1;
  Check(!dt::DecodeCanonicalTimestampComponentNoAllocV3(
             *profile, dt::TimestampValueStateV3::value, true, bad)
             .ok(),
        "reserved durable bytes are zero");
  Check(!dt::DecodeCanonicalTimestampComponentNoAllocV3(
             *profile, dt::TimestampValueStateV3::sql_null, true, bad)
             .ok(),
        "dirty NULL component refuses");
  Check(!dt::ValidateTimestampValueViewV3(
             {profile.get(), dt::TimestampValueStateV3::sql_null, 0, 1})
             .ok(),
        "dirty NULL native tuple refuses");
}

void BatchRules(const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& profile) {
  std::array<std::int32_t, 3> days{{INT32_MIN, 0, INT32_MAX}};
  std::array<p::u64, 3> nanos{{0, 0, 86'399'999'999'999ull}};
  std::array<p::byte, 1> bitmap{{0x02}};
  Check(dt::ValidateTimestampBatchViewV3(
            {profile.get(), days, nanos, bitmap}).ok(),
        "timestamp struct-of-arrays batch");
  days[1] = 1;
  Check(!dt::ValidateTimestampBatchViewV3(
             {profile.get(), days, nanos, bitmap}).ok(),
        "NULL batch slot requires zero day");
  days[1] = 0;
  bitmap[0] = 0x82;
  Check(!dt::ValidateTimestampBatchViewV3(
             {profile.get(), days, nanos, bitmap}).ok(),
        "LSB-first bitmap tail bits are zero");
  Check(dt::ComputeTimestampBatchExtentsV3(3, 37).ok() &&
            !dt::ComputeTimestampBatchExtentsV3(3, 36).ok(),
        "checked dual-array batch extent");
}

void SuccessorAuthority(
    const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& old,
    const std::shared_ptr<const dt::TimestampValidatedProfileHandleV3>& successor) {
  auto material = old->profile_material;
  auto comparison = old->comparison_material;
  const auto uuid = Hex("01a1095ff20572d3abea15c6d41a4ad4");
  for (auto data : {material.data(), comparison.data()}) {
    std::copy(uuid.begin(), uuid.end(), data + 16);
    p::StoreLittle64(data + 32, 11);
    p::StoreLittle64(data + 40, 11);
  }
  auto copy_seal = [](auto& destination, std::size_t offset, auto seal) {
    std::copy(seal.begin(), seal.end(), destination.begin() + offset);
  };
  copy_seal(material, 592, Hex32("1f2886db581146fc27408d3fc9c2914b348a6fd957891925a0ef245b858a8a77"));
  copy_seal(material, 624, Hex32("f17f7861ee31364530c880dc01b1836a8629d4c5c6ea5d287b4f9bef1882528d"));
  copy_seal(comparison, 352, Hex32("0d747769d2324170e000cb1973f8b1f0282990343cd308da83804a641b9c5796"));
  copy_seal(comparison, 384, Hex32("1673475a46395937c1eef4e90bdc92e72eec21a47404394bfe272224cf983f44"));
  Check(material == successor->profile_material &&
        comparison == successor->comparison_material, "exact successor dependencies and cohort");
  Check(!dt::BuildTimestampValidatedProfileHandleV3(old->receipt, successor->identity).ok() &&
        !dt::BuildTimestampValidatedProfileHandleV3(successor->receipt, old->identity).ok(),
        "no receipt relabeling");
  for (const auto& profile : {old, successor}) {
    for (unsigned field = 0; field < 8; ++field) {
      auto altered = *profile;
      auto& native = altered.identity.native_fields;
      switch (field) {
        case 0: native.present = !native.present; break;
        case 1: ++native.canonical_value_minimum_bytes; break;
        case 2: ++native.canonical_value_maximum_bytes; break;
        case 3: ++native.canonical_value_transport_width; break;
        case 4: native.canonical_value_variable_width = !native.canonical_value_variable_width; break;
        case 5: native.policy_profile_uuid.bytes[0] ^= 1; break;
        case 6: ++native.policy_profile_generation; break;
        case 7: native.profile_fingerprint_sha256[0] ^= 1; break;
      }
      Check(!dt::ValidateTimestampProfileHandleV3(altered).ok() &&
            !dt::BuildTimestampValidatedProfileHandleV3(altered.receipt, altered.identity).ok(),
            "every native identity field is authoritative");
    }
  }
  for (const auto& fixture : kVectors) {
    dt::TimestampOwnedValueV3 value{successor, fixture.state, fixture.civil_day, fixture.nanoseconds_since_midnight};
    dt::TimestampOwnedValueV3 previous{old, fixture.state, fixture.civil_day, fixture.nanoseconds_since_midnight};
    Check(dt::CompareTimestampValuesV3(value.view(), value.view()).ok() &&
          !dt::CompareTimestampValuesV3(value.view(), previous.view()).ok() &&
          !dt::DifferenceTimestampV3(value.view(), previous.view()).ok(),
          "same-cohort comparison and cross-cohort refusal include NULL");
    for (auto direction : {dt::TimestampSortDirectionV3::ascending, dt::TimestampSortDirectionV3::descending})
      for (auto mode : {dt::TimestampNullModeV3::nulls_first, dt::TimestampNullModeV3::nulls_last}) {
        auto key = dt::MakeTimestampSortKeyV3(value, direction, mode);
        auto old_key = dt::MakeTimestampSortKeyV3(previous, direction, mode);
        Check(key.ok() && old_key.ok(), "both cohorts produce keys");
        const bool present = fixture.state == dt::TimestampValueStateV3::value;
        std::vector<p::byte> expected(present ? 112 : 100, 0);
        std::memcpy(expected.data(), "SBTSPK01", 8);
        std::copy(uuid.begin(), uuid.end(), expected.begin() + 8);
        p::StoreLittle64(expected.data() + 24, 11);
        p::StoreLittle64(expected.data() + 32, 11);
        copy_seal(expected, 40, Hex32("879004814d3fc6583cc0de3263cd72da1d97e15bb06d942609056f58ca9c2da4"));
        const auto ordering = Hex("01a104ec8e3c7dff8e89868ecc2a8a0c");
        std::copy(ordering.begin(), ordering.end(), expected.begin() + 72);
        p::StoreLittle64(expected.data() + 88, 1);
        expected[96] = static_cast<p::byte>(direction);
        expected[97] = static_cast<p::byte>(mode);
        expected[98] = present ? 1 : (mode == dt::TimestampNullModeV3::nulls_first ? 0 : 2);
        expected[99] = present ? 12 : 0;
        if (present) {
          // Independent canonical fixture bytes, not a production key encoder.
          const auto component = Hex(fixture.component_hex);
          for (unsigned n = 0; n < 8; ++n) expected[100+n] = component[7-n];
          expected[100] ^= 0x80;
          for (unsigned n = 0; n < 4; ++n) expected[108+n] = component[11-n];
          if (direction == dt::TimestampSortDirectionV3::descending)
            for (unsigned n = 100; n < 112; ++n) expected[n] ^= 0xff;
        }
        Check(key.bytes == expected, "independent successor key oracle");
        Check(!dt::DecodeTimestampSortKeyNoAllocV3(*old, key.bytes).ok() &&
              !dt::DecodeTimestampSortKeyNoAllocV3(*successor, old_key.bytes).ok(),
              "key cross-cohort refusal both directions");
      }
  }
  for (bool null : {false, true}) {
    const auto result = dt::HashTimestampValueV3({successor, null ? dt::TimestampValueStateV3::sql_null : dt::TimestampValueStateV3::value, 0, 0});
    Check(result.ok() && result.bytes == Hex(null ?
          "9c216850057be2015961474a90f3045fe868d726322f9c9f1d88890fd97382cb" :
          "833264d581e4c1e83d23bfccb31a6fa7bbbf5aabf359fcfe0fbacdb5b0b83262"),
          "independently computed D711 NULL and epoch hashes");
  }
}

}  // namespace

int main() {
  auto profile = Profile();
  ProfileAuthority(profile);
  CanonicalValues(profile);
  BatchRules(profile);
  auto successor = scratchbird::tests::D711TimestampProfile();
  ProfileAuthority(successor);
  CanonicalValues(successor);
  BatchRules(successor);
  SuccessorAuthority(profile, successor);
  std::cout << "PASS base.timestamp V3 profile/value checks=" << checks
            << " canonical_vectors=21 profile_mutations=656"
               " comparison_mutations=416\n";
  return EXIT_SUCCESS;
}
