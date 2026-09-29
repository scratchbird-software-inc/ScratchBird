// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbl_numeric.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace numeric = scratchbird::libraries::sbl_numeric;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const std::string& message) {
  ++checks;
  if (!ok && failures++ < 20) std::cerr << "FAIL " << message << '\n';
}
numeric::Decimal128Bytes Hex(const std::string& big_endian) {
  numeric::Decimal128Bytes bytes{};
  for (unsigned i = 0; i < 16; ++i)
    bytes[15 - i] = static_cast<std::uint8_t>(std::stoul(big_endian.substr(2 * i, 2), nullptr, 16));
  return bytes;
}
void Golden(const std::string& text, const std::string& hex, bool special = false) {
  const auto expected = Hex(hex);
  const auto encoded = numeric::EncodeDecimal128LittleEndian(text, special);
  Check(encoded.bytes && *encoded.bytes == expected, "exact encode " + text);
  const auto decoded = numeric::DecodeDecimal128LittleEndian(expected.data(), expected.size(), special);
  Check(decoded.numeric.status == numeric::NumericStatusCode::ok && decoded.bytes == expected && decoded.value,
        "exact decode " + text);
}
void Refuse(const std::string& text, bool special = false) {
  const auto result = numeric::EncodeDecimal128LittleEndian(text, special);
  Check(result.numeric.status != numeric::NumericStatusCode::ok && !result.bytes && !result.value &&
            !result.numeric.diagnostic_code.empty(), "refuse " + text);
}
void RefuseBytes(const numeric::Decimal128Bytes& bytes, bool special = true) {
  const auto result = numeric::DecodeDecimal128LittleEndian(bytes.data(), bytes.size(), special);
  Check(result.numeric.status != numeric::NumericStatusCode::ok && !result.bytes && !result.value,
        "refuse malformed canonical bytes");
}
std::string Twice(std::string digits) {
  unsigned carry = 0;
  for (auto digit = digits.rbegin(); digit != digits.rend(); ++digit) {
    const unsigned doubled = 2 * static_cast<unsigned>(*digit - '0') + carry;
    *digit = static_cast<char>('0' + doubled % 10);
    carry = doubled / 10;
  }
  if (carry) digits.insert(digits.begin(), '1');
  return digits;
}
}
int main() {
  // Literal oracles are independent of the production field packer.
  Golden("0", "30400000000000000000000000000000");
  Golden("-0", "b0400000000000000000000000000000");
  Golden("1", "30400000000000000000000000000001");
  Golden("-1", "b0400000000000000000000000000001");
  Golden("0.1", "303e0000000000000000000000000001");
  Golden("1.00", "303c0000000000000000000000000064");
  Golden("125E-2", "303c000000000000000000000000007d");
  Golden("1E-6176", "00000000000000000000000000000001");
  Golden("1E6111", "5ffe0000000000000000000000000001");
  Golden("9999999999999999999999999999999999E6111", "5fffed09bead87c0378d8e63ffffffff");
  Golden("Infinity", "78000000000000000000000000000000", true);
  Golden("-inf", "f8000000000000000000000000000000", true);
  Golden("NaN", "7c000000000000000000000000000000", true);
  Golden("-NaN123", "fc00000000000000000000000000007b", true);
  Golden("sNaN123", "7e00000000000000000000000000007b", true);

  std::string power = "1";
  for (unsigned bit = 0; bit < 113; ++bit, power = Twice(power)) {
    auto expected = Hex("30400000000000000000000000000000");
    expected[bit / 8] |= static_cast<std::uint8_t>(1u << (bit % 8));
    const auto encoded = numeric::EncodeDecimal128LittleEndian(power);
    Check(encoded.bytes == expected, "independent coefficient bit " + std::to_string(bit));
    const auto decoded = numeric::DecodeDecimal128LittleEndian(expected.data(), 16);
    numeric::Decimal128Bytes coefficient{};
    coefficient[bit / 8] = static_cast<std::uint8_t>(1u << (bit % 8));
    Check(decoded.value && decoded.value->coefficient == coefficient &&
              decoded.value->classification == numeric::Decimal128Class::finite &&
              decoded.value->exponent == 0 && !decoded.value->negative, "decoded coefficient bit");
    if (bit < 110) {
      for (bool negative : {false, true}) {
        for (bool signaling : {false, true}) {
          const auto nan = numeric::EncodeDecimal128LittleEndian(
              std::string(negative ? "-" : "") + (signaling ? "sNaN" : "NaN") + power, true);
          auto nan_expected = Hex(signaling ? "7e000000000000000000000000000000" :
                                              "7c000000000000000000000000000000");
          nan_expected[bit / 8] |= coefficient[bit / 8];
          if (negative) nan_expected[15] |= 128;
          Check(nan.bytes == nan_expected && nan.value && nan.value->negative == negative &&
                    nan.value->coefficient == coefficient && nan.value->classification ==
                    (signaling ? numeric::Decimal128Class::signaling_nan : numeric::Decimal128Class::quiet_nan),
                "NaN payload bit/sign/class");
        }
      }
    }
  }

  for (std::int32_t exponent = -6176; exponent <= 6111; ++exponent) {
    for (bool negative : {false, true}) {
      const auto spelling = std::string(negative ? "-7E" : "7E") + std::to_string(exponent);
      const auto encoded = numeric::EncodeDecimal128LittleEndian(spelling);
      Check(encoded.bytes && encoded.value && encoded.value->exponent == exponent &&
                encoded.value->negative == negative && encoded.value->coefficient[0] == 7 &&
                std::all_of(encoded.value->coefficient.begin() + 1, encoded.value->coefficient.end(),
                            [](auto b) { return b == 0; }), "exponent/sign/coefficient " + spelling);
      if (encoded.bytes) {
        const auto decoded = numeric::DecodeDecimal128LittleEndian(encoded.bytes->data(), 16);
        Check(decoded.bytes == encoded.bytes && decoded.value && decoded.value->exponent == exponent,
              "all finite exponents round trip");
      }
    }
  }
  // Exact rebalance and quantum preservation; never round a nonzero digit.
  Check(numeric::EncodeDecimal128LittleEndian("100E-6178").bytes ==
        numeric::EncodeDecimal128LittleEndian("1E-6176").bytes, "exact subnormal rebalance");
  Check(numeric::EncodeDecimal128LittleEndian("1E6112").bytes ==
        numeric::EncodeDecimal128LittleEndian("10E6111").bytes, "exact high-exponent rebalance");
  Check(numeric::EncodeDecimal128LittleEndian("1E6144").bytes.has_value(), "largest scientific exponent");
  Check(numeric::EncodeDecimal128LittleEndian("1.0").bytes !=
        numeric::EncodeDecimal128LittleEndian("1.00").bytes, "storage quantum retained");
  for (const auto text : {"", "+", "-", ".", "e1", "1e", "1e+", "1e-", "1e1x", "1.2.3",
                          " 1", "1 ", "1x", "1E6145", "1E-6177", "11E-6177",
                          "99999999999999999999999999999999999", "1E999999999999999999999999",
                          "1E-999999999999999999999999", "nan", "-Infinity"}) Refuse(text);
  Refuse(std::string("1\0", 2));
  const auto overflow = numeric::EncodeDecimal128LittleEndian("1E999999999999999999999");
  const auto underflow = numeric::EncodeDecimal128LittleEndian("1E-999999999999999999999");
  Check(overflow.numeric.status == numeric::NumericStatusCode::overflow && overflow.numeric.overflow,
        "huge positive exponent reports overflow");
  Check(underflow.numeric.diagnostic_code == "numeric.decimal128.inexact_conversion" &&
            !underflow.numeric.overflow, "huge negative exponent reports inexact conversion not overflow");
  for (const auto text : {"nanx", "snan-1", "NaN1000000000000000000000000000000000"}) Refuse(text, true);
  Golden("0E99999999999999999999999", "5ffe0000000000000000000000000000");
  Golden("-0E-99999999999999999999999", "80000000000000000000000000000000");

  const auto valid = Hex("30400000000000000000000000000001");
  for (std::size_t size : {0u, 15u, 17u, 24u}) {
    const auto decoded = numeric::DecodeDecimal128LittleEndian(valid.data(), size);
    Check(!decoded.bytes && !decoded.value, "width refusal before read");
  }
  Check(!numeric::DecodeDecimal128LittleEndian(nullptr, 16).bytes, "null pointer refused");
  RefuseBytes(Hex("78000000000000000000000000000001"));
  RefuseBytes(Hex("7c004000000000000000000000000000"));
  RefuseBytes(Hex("7c003fffffffffffffffffffffffffff"));
  RefuseBytes(Hex("60000000000000000000000000000000"));
  RefuseBytes(Hex("3041ffffffffffffffffffffffffffff"));
  RefuseBytes(Hex("3041ed09bead87c0378d8e6400000000"));
  for (unsigned bit = 0; bit < 122; ++bit) {
    auto infinity = Hex("78000000000000000000000000000000");
    infinity[bit / 8] |= static_cast<std::uint8_t>(1u << (bit % 8));
    RefuseBytes(infinity);
  }
  for (unsigned bit = 110; bit < 121; ++bit) {
    auto nan = Hex("7c000000000000000000000000000000");
    nan[bit / 8] |= static_cast<std::uint8_t>(1u << (bit % 8));
    RefuseBytes(nan);
  }
  for (const auto special : {"78000000000000000000000000000000", "7c000000000000000000000000000000",
                             "7e000000000000000000000000000001"}) RefuseBytes(Hex(special), false);
  using Profile = numeric::Decimal128OrderProfile;
  const auto key = [](const std::string& text, Profile profile) {
    const auto encoded = numeric::EncodeDecimal128LittleEndian(text, true);
    if (!encoded.bytes) { Check(false, "key fixture refused: " + text); return numeric::Decimal128OrderKey{}; }
    const auto ordered = numeric::MakeDecimal128OrderKey(encoded.bytes->data(), 16, profile, true);
    Check(ordered.key.has_value(), "comparison key refused: " + text);
    return ordered.key.value_or(numeric::Decimal128OrderKey{});
  };
  const std::vector<std::string> total_order = {
      "-NaN2", "-NaN1", "-sNaN2", "-sNaN1", "-Infinity", "-1E6144", "-2",
      "-1", "-1.0", "-1.00", "-1E-6176", "-0E1", "-0", "-0.0",
      "0.0", "0", "0E1", "1E-6176", "0.01", "0.1", "1.00", "1.0", "1", "2",
      "1E6144", "Infinity", "sNaN1", "sNaN2", "NaN1", "NaN2"};
  const numeric::Decimal128OrderKey one_key = {
      6, 0x18, 0x20, 0, 0, 0x31, 0x4d, 0xc6, 0x44, 0x8d, 0x93,
      0x38, 0xc1, 0x5b, 0x0a, 0, 0, 0, 0, 0x18, 0x20};
  Check(key("1", Profile::ieee_total_order) == one_key, "independent full comparison key bytes");
  for (std::size_t i = 1; i < total_order.size(); ++i)
    Check(key(total_order[i - 1], Profile::ieee_total_order) < key(total_order[i], Profile::ieee_total_order),
          "independent total order " + total_order[i - 1] + " < " + total_order[i]);
  for (const auto& group : std::vector<std::vector<std::string>>{
           {"-0", "0", "0.0", "-0E6111"}, {"1", "1.0", "100E-2"},
           {"-1", "-1.00", "-100E-2"}, {"NaN", "-NaN123", "sNaN321"}}) {
    for (const auto& spelling : group)
      Check(key(group.front(), Profile::numeric_total_nan_last) == key(spelling, Profile::numeric_total_nan_last),
            "numeric comparison cohort equality " + spelling);
  }
  const std::vector<std::string> numeric_order = {"-Infinity", "-1E6144", "-2", "-1", "-1E-6176",
      "0", "1E-6176", "0.01", "0.1", "1", "2", "1E6144", "Infinity", "NaN"};
  for (std::size_t i = 1; i < numeric_order.size(); ++i)
    Check(key(numeric_order[i - 1], Profile::numeric_total_nan_last) <
              key(numeric_order[i], Profile::numeric_total_nan_last), "independent numeric order");
  const auto canonical_one = Hex("30400000000000000000000000000001");
  Check(!numeric::MakeDecimal128OrderKey(canonical_one.data(), 16, static_cast<Profile>(255)).key,
        "invalid ordering policy refused");
  Check(!numeric::MakeDecimal128OrderKey(canonical_one.data(), 15, Profile::ieee_total_order).key,
        "invalid key input width refused");
  const auto infinity = Hex("78000000000000000000000000000000");
  Check(!numeric::MakeDecimal128OrderKey(infinity.data(), 16, Profile::ieee_total_order).key,
        "key generation cannot bypass special-value policy");
  std::cout << "decimal128 codec checks=" << checks << " failures=" << failures << '\n';
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
