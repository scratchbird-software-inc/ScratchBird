// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_operator_runtime.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace sblr = scratchbird::engine::sblr;

namespace {

std::string JsonEscape(std::string_view text) {
  std::string out;
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += ch; break;
    }
  }
  return out;
}

void Expect(bool condition, std::string message, std::vector<std::string>* errors) {
  if (!condition) errors->push_back(std::move(message));
}

sblr::SblrValue DecimalValue(std::string encoded) {
  sblr::SblrValue value;
  value.descriptor_id = "decimal";
  value.payload_kind = sblr::SblrValuePayloadKind::high_precision_numeric_text;
  value.encoded_value = std::move(encoded);
  value.text_value = value.encoded_value;
  value.is_null = false;
  return value;
}

sblr::SblrValue BinaryValue(std::vector<std::uint8_t> bytes) {
  sblr::SblrValue value;
  value.descriptor_id = "binary";
  value.payload_kind = sblr::SblrValuePayloadKind::binary;
  value.binary_value = std::move(bytes);
  value.is_null = false;
  return value;
}

sblr::SblrValue TextValue(std::string text, std::string charset, std::string collation) {
  sblr::SblrValue value;
  value.descriptor_id = "text";
  value.payload_kind = sblr::SblrValuePayloadKind::text;
  value.text_value = std::move(text);
  value.encoded_value = value.text_value;
  value.charset_name = std::move(charset);
  value.collation_name = std::move(collation);
  value.is_null = false;
  return value;
}

bool TruthValueIs(const sblr::SblrResult& result, std::int64_t expected) {
  return result.ok() && result.scalar_values.size() == 1 &&
         result.scalar_values[0].descriptor_id == "boolean" &&
         result.scalar_values[0].has_int64_value &&
         result.scalar_values[0].int64_value == expected;
}

}  // namespace

int main() {
  std::vector<std::string> errors;
  sblr::SblrExecutionContext context;
  context.database_uuid = "019b6cf8-b000-7000-8000-000000000001";
  context.statement_uuid = "019b6cf8-b000-7000-8000-000000000002";
  context.user_uuid = "019b6cf8-b000-7000-8000-000000000003";
  context.security_snapshot_uuid = "019b6cf8-b000-7000-8000-000000000004";

  const auto decimal_sum = sblr::EvaluateSblrArithmetic("op_add", DecimalValue("1.25"), DecimalValue("2.50"), context);
  Expect(decimal_sum.ok(), "decimal add should succeed", &errors);
  Expect(decimal_sum.scalar_values.size() == 1, "decimal add should return one scalar", &errors);
  if (decimal_sum.scalar_values.size() == 1) {
    const auto& value = decimal_sum.scalar_values[0];
    Expect(value.descriptor_id == "decimal", "decimal add should preserve decimal descriptor", &errors);
    Expect(value.payload_kind == sblr::SblrValuePayloadKind::high_precision_numeric_text,
           "decimal add should preserve high-precision payload kind", &errors);
    Expect(!value.encoded_value.empty(), "decimal add should preserve encoded numeric value", &errors);
  }

  const auto decimal_gt = sblr::EvaluateSblrComparison("op_gt", DecimalValue("10.5"), DecimalValue("2.25"), context);
  Expect(TruthValueIs(decimal_gt, 1), "decimal comparison should use numeric ordering", &errors);

  const auto binary_lt = sblr::EvaluateSblrComparison("op_lt", BinaryValue({0x01, 0x02}), BinaryValue({0x01, 0x03}), context);
  Expect(TruthValueIs(binary_lt, 1), "binary comparison should use binary payload bytes", &errors);

  const auto text_lt = sblr::EvaluateSblrComparison("op_lt",
                                                    TextValue("alpha", "UTF8", "UTF8_BINARY"),
                                                    TextValue("beta", "UTF8", "UTF8_BINARY"),
                                                    context);
  Expect(TruthValueIs(text_lt, 1), "text comparison with seed metadata should use datatype comparison authority", &errors);

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n";
  std::cout << "  \"checks\": 4,\n";
  std::cout << "  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[i]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
