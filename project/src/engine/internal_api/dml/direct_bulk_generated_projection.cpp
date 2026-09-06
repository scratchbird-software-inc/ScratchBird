// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/direct_bulk_generated_projection.hpp"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api::dml::detail {

// SEARCH_KEY: SB_ENGINE_DIRECT_BULK_GENERATED_PROJECTION_IMPLEMENTATION_AUTHORITY
// Deterministic projection only: no row/index mutation or transaction finality.

std::string DirectOptionValue(const DirectPhysicalBulkAppendRequest& request,
                              const std::string& key) {
  const std::string equals_prefix = key + "=";
  const std::string colon_prefix = key + ":";
  for (const auto& candidate : request.option_envelopes) {
    if (candidate.rfind(equals_prefix, 0) == 0) {
      return candidate.substr(equals_prefix.size());
    }
    if (candidate.rfind(colon_prefix, 0) == 0) {
      return candidate.substr(colon_prefix.size());
    }
  }
  return {};
}

std::vector<std::string> DirectSplitText(std::string_view text,
                                         char delimiter) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t pos = text.find(delimiter, start);
    if (pos == std::string_view::npos) {
      parts.emplace_back(text.substr(start));
      break;
    }
    parts.emplace_back(text.substr(start, pos - start));
    start = pos + 1;
  }
  return parts;
}

std::optional<std::uint64_t> DirectOptionU64Optional(
    const DirectPhysicalBulkAppendRequest& request,
    const std::string& key) {
  const std::string value = DirectOptionValue(request, key);
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed;
}

bool DirectGeneratedCounterEnvelopeRequested(
    const DirectPhysicalBulkAppendRequest& request) {
  return request.lane_operation == "insert_select" &&
         DirectOptionValue(request, "insert_select_source_kind") ==
             "recursive_counter_cte";
}

std::optional<std::uint64_t> DirectGeneratedCounterRowCount(
    std::uint64_t start,
    std::uint64_t step,
    std::uint64_t limit) {
  if (step == 0 || limit < start) {
    return std::nullopt;
  }
  return ((limit - start) / step) + 1;
}

std::optional<std::uint64_t> DirectPow10U64(int scale) {
  if (scale < 0 || scale > 18) {
    return std::nullopt;
  }
  std::uint64_t value = 1;
  for (int index = 0; index < scale; ++index) {
    value *= 10;
  }
  return value;
}

std::optional<std::uint64_t> DirectCheckedMultiplyU64(std::uint64_t lhs,
                                                       std::uint64_t rhs) {
  if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
    return std::nullopt;
  }
  return lhs * rhs;
}

std::optional<std::uint64_t> DirectRoundDivideU64(std::uint64_t numerator,
                                                  std::uint64_t denominator) {
  if (denominator == 0) {
    return std::nullopt;
  }
  const std::uint64_t half = denominator / 2;
  if (numerator > std::numeric_limits<std::uint64_t>::max() - half) {
    return std::nullopt;
  }
  return (numerator + half) / denominator;
}

void DirectAppendU64(std::string* out, std::uint64_t value) {
  if (out == nullptr) {
    return;
  }
  char buffer[32];
  auto [ptr, ec] = std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (ec == std::errc()) {
    out->append(buffer, static_cast<std::size_t>(ptr - buffer));
  }
}

std::optional<DirectScaledDecimalOperand> DirectParseScaledDecimal(
    std::string_view value) {
  if (value.empty() || value.front() == '-') {
    return std::nullopt;
  }
  DirectScaledDecimalOperand operand;
  bool seen_digit = false;
  bool seen_dot = false;
  for (const char ch : value) {
    if (ch == '.') {
      if (seen_dot) {
        return std::nullopt;
      }
      seen_dot = true;
      continue;
    }
    if (!std::isdigit(static_cast<unsigned char>(ch))) {
      return std::nullopt;
    }
    seen_digit = true;
    if (operand.value > (std::numeric_limits<std::uint64_t>::max() / 10)) {
      return std::nullopt;
    }
    operand.value *= 10;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (operand.value > std::numeric_limits<std::uint64_t>::max() - digit) {
      return std::nullopt;
    }
    operand.value += digit;
    if (seen_dot) {
      ++operand.scale;
      if (operand.scale > 18) {
        return std::nullopt;
      }
    }
  }
  if (!seen_digit || operand.value == 0) {
    return std::nullopt;
  }
  return operand;
}

std::string DirectFormatScaledDecimal(std::uint64_t scaled_value, int scale) {
  if (scale <= 0) {
    std::string out;
    out.reserve(20);
    DirectAppendU64(&out, scaled_value);
    return out;
  }
  const auto divisor = DirectPow10U64(scale);
  if (!divisor || *divisor == 0) {
    return {};
  }
  const std::uint64_t whole = scaled_value / *divisor;
  const std::uint64_t fraction = scaled_value % *divisor;
  std::string out;
  out.reserve(32);
  DirectAppendU64(&out, whole);
  out.push_back('.');
  std::string fraction_text;
  fraction_text.reserve(20);
  DirectAppendU64(&fraction_text, fraction);
  if (fraction_text.size() < static_cast<std::size_t>(scale)) {
    out.append(static_cast<std::size_t>(scale) - fraction_text.size(), '0');
  }
  out.append(fraction_text);
  return out;
}

std::string DirectFormatScaledDecimalWithFactor(std::uint64_t scaled_value,
                                                int scale,
                                                std::uint64_t divisor) {
  if (scale <= 0) {
    std::string out;
    out.reserve(20);
    DirectAppendU64(&out, scaled_value);
    return out;
  }
  if (divisor == 0) {
    return {};
  }
  const std::uint64_t whole = scaled_value / divisor;
  const std::uint64_t fraction = scaled_value % divisor;
  std::string out;
  out.reserve(32);
  DirectAppendU64(&out, whole);
  out.push_back('.');
  std::string fraction_text;
  fraction_text.reserve(20);
  DirectAppendU64(&fraction_text, fraction);
  if (fraction_text.size() < static_cast<std::size_t>(scale)) {
    out.append(static_cast<std::size_t>(scale) - fraction_text.size(), '0');
  }
  out.append(fraction_text);
  return out;
}

bool DirectParseLongLong(const std::string& value, long long* out) {
  if (out == nullptr || value.empty()) {
    return false;
  }
  try {
    *out = std::stoll(value);
    return true;
  } catch (...) {
    return false;
  }
}

std::optional<std::uint64_t> DirectParseU64(const std::string& value) {
  if (value.empty()) {
    return std::nullopt;
  }
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') {
      return std::nullopt;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed;
}

bool DirectParseLongDouble(const std::string& value, long double* out) {
  if (out == nullptr || value.empty()) {
    return false;
  }
  try {
    *out = std::stold(value);
    return true;
  } catch (...) {
    return false;
  }
}

bool DirectParseIntScale(const std::string& value, int* out) {
  if (out == nullptr || value.empty()) {
    return false;
  }
  try {
    const int parsed = std::stoi(value);
    if (parsed < 0 || parsed > 18) {
      return false;
    }
    *out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

std::string DirectGeneratedProjectionType(const std::string& descriptor,
                                          const std::string& target_descriptor) {
  if (descriptor == "counter") return "integer";
  if (descriptor.rfind("literal_text:", 0) == 0) return "text";
  if (descriptor.rfind("literal_boolean:", 0) == 0) return "boolean";
  if (descriptor.rfind("literal_integer:", 0) == 0) return "integer";
  if (descriptor.rfind("mod:", 0) == 0) return "integer";
  if (descriptor.rfind("prefix_counter:", 0) == 0) return "text";
  if (descriptor.rfind("prefix_counter_offset:", 0) == 0) return "text";
  if (descriptor.rfind("case_zero_literal_else_", 0) == 0) return "text";
  if (descriptor.rfind("cast_divide:", 0) == 0) return "decimal";
  if (descriptor.rfind("counter_multiply:", 0) == 0) return "decimal";
  if (descriptor.rfind("mod_equals:", 0) == 0) return "boolean";
  if (target_descriptor.rfind("type=", 0) == 0) {
    return target_descriptor.substr(5);
  }
  return target_descriptor.empty() ? "text" : target_descriptor;
}

DirectGeneratedProjectionPlan DirectBuildGeneratedProjectionPlan(
    const std::string& column_name,
    const std::string& descriptor,
    const std::string& target_descriptor) {
  DirectGeneratedProjectionPlan plan;
  plan.column_name = column_name;
  plan.descriptor = descriptor;
  plan.parts = DirectSplitText(descriptor, ':');
  plan.type_name = DirectGeneratedProjectionType(descriptor, target_descriptor);
  if (plan.type_name.empty()) {
    plan.type_name = "text";
  }
  if (descriptor == "counter") {
    plan.kind = DirectGeneratedProjectionKind::counter;
    return plan;
  }
  if (plan.parts.empty()) {
    return plan;
  }
  if (plan.parts[0] == "literal_text" && plan.parts.size() >= 2) {
    plan.kind = DirectGeneratedProjectionKind::literal;
    plan.literal_value = plan.parts[1];
    return plan;
  }
  if ((plan.parts[0] == "literal_boolean" ||
       plan.parts[0] == "literal_integer") &&
      plan.parts.size() == 2) {
    plan.kind = DirectGeneratedProjectionKind::literal;
    plan.literal_value = plan.parts[1];
    return plan;
  }
  if (plan.parts[0] == "mod" && plan.parts.size() == 2) {
    const auto modulus = DirectParseU64(plan.parts[1]);
    if (modulus && *modulus != 0) {
      plan.kind = DirectGeneratedProjectionKind::mod;
      plan.modulus = *modulus;
      return plan;
    }
  }
  if (plan.parts[0] == "prefix_counter" && plan.parts.size() >= 2) {
    plan.kind = DirectGeneratedProjectionKind::prefix_counter;
    plan.prefix = plan.parts[1];
    return plan;
  }
  if (plan.parts[0] == "prefix_counter_offset" &&
      plan.parts.size() == 3 &&
      DirectParseLongLong(plan.parts[2], &plan.offset)) {
    plan.kind = DirectGeneratedProjectionKind::prefix_counter_offset;
    plan.prefix = plan.parts[1];
    return plan;
  }
  if (plan.parts[0] == "case_zero_literal_else_literal" &&
      plan.parts.size() == 3) {
    plan.kind = DirectGeneratedProjectionKind::case_zero_literal_else_literal;
    plan.literal_value = plan.parts[1];
    plan.alternate_literal_value = plan.parts[2];
    return plan;
  }
  if (plan.parts[0] == "case_zero_literal_else_prefix_counter_offset" &&
      plan.parts.size() == 4 &&
      DirectParseLongLong(plan.parts[3], &plan.offset)) {
    plan.kind =
        DirectGeneratedProjectionKind::case_zero_literal_else_prefix_counter_offset;
    plan.literal_value = plan.parts[1];
    plan.prefix = plan.parts[2];
    return plan;
  }
  if (plan.parts[0] == "cast_divide" && plan.parts.size() >= 4 &&
      DirectParseLongDouble(plan.parts[2], &plan.factor) &&
      plan.factor != 0.0L &&
      DirectParseIntScale(plan.parts[3], &plan.scale)) {
    plan.kind = DirectGeneratedProjectionKind::cast_divide;
    if (const auto scaled = DirectParseScaledDecimal(plan.parts[2])) {
      plan.has_scaled_operand = true;
      plan.scaled_operand = *scaled;
    }
    return plan;
  }
  if (plan.parts[0] == "counter_multiply" && plan.parts.size() >= 3 &&
      DirectParseLongDouble(plan.parts[1], &plan.factor) &&
      DirectParseIntScale(plan.parts[2], &plan.scale)) {
    plan.kind = DirectGeneratedProjectionKind::counter_multiply;
    if (const auto output_scale = DirectPow10U64(plan.scale)) {
      plan.output_scale_factor = *output_scale;
    }
    if (const auto scaled = DirectParseScaledDecimal(plan.parts[1])) {
      plan.has_scaled_operand = true;
      plan.scaled_operand = *scaled;
      const auto operand_scale = DirectPow10U64(plan.scaled_operand.scale);
      const auto numerator =
          plan.output_scale_factor == 0
              ? std::nullopt
              : DirectCheckedMultiplyU64(plan.scaled_operand.value,
                                         plan.output_scale_factor);
      if (operand_scale && *operand_scale != 0 && numerator &&
          *numerator % *operand_scale == 0) {
        plan.has_exact_scaled_result_multiplier = true;
        plan.exact_scaled_result_multiplier = *numerator / *operand_scale;
      }
    }
    return plan;
  }
  if (plan.parts[0] == "mod_equals" && plan.parts.size() == 3) {
    const auto modulus = DirectParseU64(plan.parts[1]);
    const auto expected = DirectParseU64(plan.parts[2]);
    if (modulus && *modulus != 0 && expected) {
      plan.kind = DirectGeneratedProjectionKind::mod_equals;
      plan.modulus = *modulus;
      plan.expected = *expected;
      return plan;
    }
  }
  return plan;
}

std::string DirectGeneratedProjectionValue(
    const DirectGeneratedProjectionPlan& plan,
    std::uint64_t counter) {
  switch (plan.kind) {
    case DirectGeneratedProjectionKind::counter:
    {
      std::string out;
      out.reserve(20);
      DirectAppendU64(&out, counter);
      return out;
    }
    case DirectGeneratedProjectionKind::literal:
      return plan.literal_value;
    case DirectGeneratedProjectionKind::mod:
    {
      std::string out;
      out.reserve(20);
      DirectAppendU64(&out, counter % plan.modulus);
      return out;
    }
    case DirectGeneratedProjectionKind::prefix_counter: {
      std::string out;
      out.reserve(plan.prefix.size() + 20);
      out.append(plan.prefix);
      DirectAppendU64(&out, counter);
      return out;
    }
    case DirectGeneratedProjectionKind::prefix_counter_offset: {
      const auto adjusted = static_cast<long long>(counter) + plan.offset;
      if (adjusted < 0) {
        return {};
      }
      std::string out;
      out.reserve(plan.prefix.size() + 20);
      out.append(plan.prefix);
      DirectAppendU64(&out, static_cast<std::uint64_t>(adjusted));
      return out;
    }
    case DirectGeneratedProjectionKind::case_zero_literal_else_literal:
      return counter == 0 ? plan.literal_value : plan.alternate_literal_value;
    case DirectGeneratedProjectionKind::case_zero_literal_else_prefix_counter_offset: {
      if (counter == 0) {
        return plan.literal_value;
      }
      const auto adjusted = static_cast<long long>(counter) + plan.offset;
      if (adjusted < 0) {
        return {};
      }
      std::string out;
      out.reserve(plan.prefix.size() + 20);
      out.append(plan.prefix);
      DirectAppendU64(&out, static_cast<std::uint64_t>(adjusted));
      return out;
    }
    case DirectGeneratedProjectionKind::cast_divide: {
      if (plan.has_scaled_operand) {
        const auto scale_factor =
            DirectPow10U64(plan.scaled_operand.scale + plan.scale);
        if (scale_factor) {
          const auto numerator =
              DirectCheckedMultiplyU64(counter, *scale_factor);
          if (numerator) {
            const auto scaled_result =
                DirectRoundDivideU64(*numerator, plan.scaled_operand.value);
            if (scaled_result) {
              const std::string fast =
                  DirectFormatScaledDecimal(*scaled_result, plan.scale);
              if (!fast.empty()) {
                return fast;
              }
            }
          }
        }
      }
      std::ostringstream out;
      out << std::fixed << std::setprecision(plan.scale)
          << (static_cast<long double>(counter) / plan.factor);
      return out.str();
    }
    case DirectGeneratedProjectionKind::counter_multiply: {
      if (plan.has_exact_scaled_result_multiplier) {
        const auto scaled_result =
            DirectCheckedMultiplyU64(counter,
                                     plan.exact_scaled_result_multiplier);
        if (scaled_result) {
          const std::string fast = DirectFormatScaledDecimalWithFactor(
              *scaled_result,
              plan.scale,
              plan.output_scale_factor);
          if (!fast.empty()) {
            return fast;
          }
        }
      }
      if (plan.has_scaled_operand) {
        const auto output_scale = DirectPow10U64(plan.scale);
        const auto operand_scale = DirectPow10U64(plan.scaled_operand.scale);
        if (output_scale && operand_scale) {
          const auto first =
              DirectCheckedMultiplyU64(counter, plan.scaled_operand.value);
          const auto numerator =
              first ? DirectCheckedMultiplyU64(*first, *output_scale)
                    : std::nullopt;
          if (numerator) {
            const auto scaled_result =
                DirectRoundDivideU64(*numerator, *operand_scale);
            if (scaled_result) {
              const std::string fast =
                  DirectFormatScaledDecimal(*scaled_result, plan.scale);
              if (!fast.empty()) {
                return fast;
              }
            }
          }
        }
      }
      std::ostringstream out;
      out << std::fixed << std::setprecision(plan.scale)
          << (static_cast<long double>(counter) * plan.factor);
      return out.str();
    }
    case DirectGeneratedProjectionKind::mod_equals:
      return (counter % plan.modulus) == plan.expected ? "true" : "false";
    case DirectGeneratedProjectionKind::unsupported:
      break;
  }
  return {};
}

DirectGeneratedCounterPlan DirectBuildGeneratedCounterPlan(
    const DirectPhysicalBulkAppendRequest& request,
    const CrudTableRecord& table) {
  DirectGeneratedCounterPlan plan;
  plan.requested = DirectGeneratedCounterEnvelopeRequested(request);
  if (!plan.requested) {
    return plan;
  }
  const auto start = DirectOptionU64Optional(request, "insert_select_counter_start");
  const auto step = DirectOptionU64Optional(request, "insert_select_counter_step");
  const auto limit = DirectOptionU64Optional(request, "insert_select_counter_limit");
  const auto projection_count =
      DirectOptionU64Optional(request, "insert_select_projection_count");
  if (!start || !step || !limit || !projection_count || *step == 0 ||
      *projection_count == 0 || *projection_count > table.columns.size()) {
    plan.failure_reason = "insert_select_generator_descriptor_invalid";
    return plan;
  }
  const auto row_count = DirectGeneratedCounterRowCount(*start, *step, *limit);
  if (!row_count || *row_count == 0 || *row_count > 1000000ULL) {
    plan.failure_reason = "insert_select_generator_bound_refused";
    return plan;
  }
  plan.start = *start;
  plan.step = *step;
  plan.limit = *limit;
  plan.row_count = *row_count;
  plan.projections.reserve(static_cast<std::size_t>(*projection_count));
  for (std::uint64_t index = 0; index < *projection_count; ++index) {
    const std::string descriptor =
        DirectOptionValue(request,
                          "insert_select_projection_" + std::to_string(index));
    if (descriptor.empty()) {
      plan.failure_reason = "insert_select_projection_descriptor_missing";
      return plan;
    }
    const auto& column = table.columns[static_cast<std::size_t>(index)];
    auto projection =
        DirectBuildGeneratedProjectionPlan(column.first, descriptor, column.second);
    if (projection.kind == DirectGeneratedProjectionKind::unsupported) {
      plan.failure_reason = "insert_select_projection_descriptor_invalid";
      return plan;
    }
    plan.projections.push_back(std::move(projection));
  }
  plan.ok = true;
  return plan;
}

std::size_t DirectRequestRowCount(const DirectPhysicalBulkAppendRequest& request,
                                  const DirectGeneratedCounterPlan& generated) {
  if (!request.borrowed_input_rows.empty()) {
    return request.borrowed_input_rows.size();
  }
  if (request.native_row_packet != nullptr &&
      request.native_row_packet->present &&
      request.native_row_packet->row_count != 0 &&
      request.native_row_packet->row_count <=
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return static_cast<std::size_t>(request.native_row_packet->row_count);
  }
  if (generated.ok) {
    return static_cast<std::size_t>(generated.row_count);
  }
  return static_cast<std::size_t>(request.estimated_row_count);
}

}  // namespace scratchbird::engine::internal_api::dml::detail
