// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/vector_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::functions {
namespace {

struct ParsedVector {
  std::vector<double> values;
  std::string error;

  [[nodiscard]] bool ok() const { return error.empty(); }
};

std::string Trim(std::string_view input) {
  std::size_t first = 0;
  while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first]))) ++first;
  std::size_t last = input.size();
  while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1]))) --last;
  return std::string(input.substr(first, last - first));
}

bool IdIs(const std::string& id, std::initializer_list<std::string_view> names) {
  for (const auto name : names) {
    if (id == name || id == "sb.vector." + std::string(name) || id == "vector." + std::string(name) ||
        id == "sb.fn.vector." + std::string(name)) {
      return true;
    }
  }
  return false;
}

bool IdEndsWith(const std::string& id, std::string_view suffix) {
  return id.size() >= suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool AnyNull(const FunctionCallRequest& request) {
  for (const auto& argument : request.arguments) {
    if (IsSqlNull(argument.value)) return true;
  }
  return false;
}

ParsedVector ParseVectorText(std::string text) {
  text = Trim(text);
  if (text.empty()) return {{}, "vector text is empty"};

  const auto first_container = text.find_first_of("[{(");
  if (first_container != std::string::npos) {
    const char open = text[first_container];
    const char close = open == '[' ? ']' : (open == '{' ? '}' : ')');
    const auto last_container = text.find_last_of(close);
    if (last_container == std::string::npos || last_container < first_container) {
      return {{}, "vector text has unmatched container delimiters"};
    }
    text = text.substr(first_container + 1, last_container - first_container - 1);
  }

  for (char& ch : text) {
    if (ch == ',' || ch == ';' || ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '(' || ch == ')') {
      ch = ' ';
    }
  }

  std::istringstream in(text);
  std::string token;
  std::vector<double> values;
  while (in >> token) {
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(token.c_str(), &end);
    if (end == token.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
      return {{}, "vector contains a non-finite or non-numeric element: " + token};
    }
    values.push_back(value);
  }
  if (values.empty()) return {{}, "vector must contain at least one element"};
  return {std::move(values), {}};
}

ParsedVector ParseVectorValue(const scratchbird::engine::sblr::SblrValue& value) {
  if (value.has_real64_value) {
    if (!std::isfinite(value.real64_value)) return {{}, "vector contains a non-finite element"};
    return {{value.real64_value}, {}};
  }
  if (value.has_int64_value) return {{static_cast<double>(value.int64_value)}, {}};
  if (value.has_uint64_value) return {{static_cast<double>(value.uint64_value)}, {}};
  return ParseVectorText(ValueAsText(value));
}

ParsedVector ParseVectorArgument(const FunctionCallRequest& request, std::size_t index) {
  if (index >= request.arguments.size()) return {{}, "missing vector argument"};
  return ParseVectorValue(request.arguments[index].value);
}

std::string FormatDouble(double value) {
  if (value == 0.0) value = 0.0;
  char buffer[64];
  const auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
  if (ec == std::errc{}) return std::string(buffer, ptr);
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
}

std::string SerializeVector(const std::vector<double>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += ",";
    out += FormatDouble(values[i]);
  }
  out += "]";
  return out;
}

std::string SerializeIntVector(const std::vector<std::int64_t>& values) {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += ",";
    out += std::to_string(values[i]);
  }
  out += "]";
  return out;
}

std::string VectorTextFromArguments(const std::vector<FunctionArgument>& arguments) {
  std::vector<double> values;
  values.reserve(arguments.size());
  for (const auto& argument : arguments) {
    const auto parsed = ParseVectorValue(argument.value);
    if (!parsed.ok() || parsed.values.size() != 1) return {};
    values.push_back(parsed.values.front());
  }
  return SerializeVector(values);
}

bool SameDimension(const std::vector<double>& left, const std::vector<double>& right) {
  return !left.empty() && left.size() == right.size();
}

double Dot(const std::vector<double>& left, const std::vector<double>& right) {
  double dot = 0.0;
  for (std::size_t i = 0; i < left.size(); ++i) dot += left[i] * right[i];
  return dot;
}

double NormSquared(const std::vector<double>& values) {
  double sum = 0.0;
  for (const auto value : values) sum += value * value;
  return sum;
}

FunctionCallResult RefuseVectorParse(const FunctionCallRequest& request, const ParsedVector& parsed) {
  return RefuseFunctionInvalidInput(request, parsed.error.empty() ? "invalid vector input" : parsed.error);
}

FunctionCallResult RequireTwoVectors(const FunctionCallRequest& request,
                                     ParsedVector* left,
                                     ParsedVector* right,
                                     std::string_view arity_detail) {
  if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, std::string(arity_detail));
  *left = ParseVectorArgument(request, 0);
  if (!left->ok()) return RefuseVectorParse(request, *left);
  *right = ParseVectorArgument(request, 1);
  if (!right->ok()) return RefuseVectorParse(request, *right);
  if (!SameDimension(left->values, right->values)) {
    return RefuseFunctionInvalidInput(request, "vector dimensions must match and be non-empty");
  }
  return {};
}

std::optional<std::vector<char>> ParseBitVectorText(std::string text, std::string* error) {
  text = Trim(text);
  if (text.size() >= 3 && (text[0] == 'B' || text[0] == 'b') && text[1] == '\'' && text.back() == '\'') {
    text = text.substr(2, text.size() - 3);
  } else if (text.size() >= 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
    text = text.substr(2);
  }

  std::vector<char> bits;
  for (const char ch : text) {
    if (ch == '0' || ch == '1') {
      bits.push_back(ch);
    } else if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',' || ch == '_' || ch == '[' || ch == ']' ||
               ch == '{' || ch == '}' || ch == '(' || ch == ')') {
      continue;
    } else {
      *error = "bit vector contains a non-bit character";
      return std::nullopt;
    }
  }
  if (bits.empty()) {
    *error = "bit vector must contain at least one bit";
    return std::nullopt;
  }
  return bits;
}

std::uint16_t FloatToHalfBits(float input) {
  const auto bits = std::bit_cast<std::uint32_t>(input);
  const std::uint32_t sign = (bits >> 16) & 0x8000u;
  std::int32_t exponent = static_cast<std::int32_t>((bits >> 23) & 0xffu) - 127 + 15;
  std::uint32_t mantissa = bits & 0x7fffffu;

  if (exponent <= 0) {
    if (exponent < -10) return static_cast<std::uint16_t>(sign);
    mantissa |= 0x800000u;
    const std::uint32_t shift = static_cast<std::uint32_t>(14 - exponent);
    const std::uint32_t rounded = mantissa + (1u << (shift - 1));
    return static_cast<std::uint16_t>(sign | (rounded >> shift));
  }

  mantissa += 0x1000u;
  if ((mantissa & 0x800000u) != 0) {
    mantissa = 0;
    ++exponent;
  }
  if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00u);
  return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10) | (mantissa >> 13));
}

float HalfBitsToFloat(std::uint16_t half) {
  const std::uint32_t sign = (static_cast<std::uint32_t>(half & 0x8000u)) << 16;
  std::uint32_t exponent = (half >> 10) & 0x1fu;
  std::uint32_t mantissa = half & 0x03ffu;
  std::uint32_t bits = 0;

  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 1;
      while ((mantissa & 0x0400u) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x03ffu;
      bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  return std::bit_cast<float>(bits);
}

FunctionCallResult ReturnNull(const FunctionCallRequest& request, std::string descriptor_id) {
  return MakeFunctionSuccess(request, {MakeNullValue(std::move(descriptor_id))});
}

FunctionCallResult ReturnVector(const FunctionCallRequest& request,
                                const std::vector<double>& values,
                                std::string descriptor_id = "dense_vector") {
  return MakeFunctionSuccess(request, {MakeTextValue(std::move(descriptor_id), SerializeVector(values))});
}

FunctionCallResult ReturnIntVector(const FunctionCallRequest& request,
                                   const std::vector<std::int64_t>& values,
                                   std::string descriptor_id) {
  return MakeFunctionSuccess(request, {MakeTextValue(std::move(descriptor_id), SerializeIntVector(values))});
}

FunctionCallResult ConstructVector(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "vector constructor expects at least one numeric value");
  if (AnyNull(request)) return ReturnNull(request, "dense_vector");

  if (request.arguments.size() == 1) {
    const auto parsed = ParseVectorArgument(request, 0);
    if (!parsed.ok()) return RefuseVectorParse(request, parsed);
    return ReturnVector(request, parsed.values);
  }

  const auto text = VectorTextFromArguments(request.arguments);
  if (text.empty()) return RefuseFunctionInvalidInput(request, "vector constructor expects numeric scalar arguments");
  return MakeFunctionSuccess(request, {MakeTextValue("dense_vector", text)});
}

FunctionCallResult L2Distance(const FunctionCallRequest& request) {
  if (AnyNull(request)) return ReturnNull(request, "real64");
  ParsedVector left;
  ParsedVector right;
  auto guard = RequireTwoVectors(request, &left, &right, "l2_distance expects two vectors");
  if (!guard.result.ok()) return guard;

  double sum = 0.0;
  for (std::size_t i = 0; i < left.values.size(); ++i) {
    const double delta = left.values[i] - right.values[i];
    sum += delta * delta;
  }
  return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::sqrt(sum))});
}

FunctionCallResult L1Distance(const FunctionCallRequest& request) {
  if (AnyNull(request)) return ReturnNull(request, "real64");
  ParsedVector left;
  ParsedVector right;
  auto guard = RequireTwoVectors(request, &left, &right, "l1_distance expects two vectors");
  if (!guard.result.ok()) return guard;

  double sum = 0.0;
  for (std::size_t i = 0; i < left.values.size(); ++i) sum += std::abs(left.values[i] - right.values[i]);
  return MakeFunctionSuccess(request, {MakeReal64Value("real64", sum)});
}

FunctionCallResult InnerProduct(const FunctionCallRequest& request, bool negative) {
  if (AnyNull(request)) return ReturnNull(request, "real64");
  ParsedVector left;
  ParsedVector right;
  auto guard = RequireTwoVectors(request, &left, &right, "inner_product expects two vectors");
  if (!guard.result.ok()) return guard;

  const double dot = Dot(left.values, right.values);
  return MakeFunctionSuccess(request, {MakeReal64Value("real64", negative ? -dot : dot)});
}

FunctionCallResult JaccardDistance(const FunctionCallRequest& request) {
  if (AnyNull(request)) return ReturnNull(request, "real64");
  ParsedVector left;
  ParsedVector right;
  auto guard = RequireTwoVectors(request, &left, &right, "jaccard_distance expects two vectors");
  if (!guard.result.ok()) return guard;

  std::size_t intersection = 0;
  std::size_t union_count = 0;
  for (std::size_t i = 0; i < left.values.size(); ++i) {
    const bool l = left.values[i] != 0.0;
    const bool r = right.values[i] != 0.0;
    if (l || r) ++union_count;
    if (l && r) ++intersection;
  }
  const double distance = union_count == 0 ? 0.0 : 1.0 - (static_cast<double>(intersection) / static_cast<double>(union_count));
  return MakeFunctionSuccess(request, {MakeReal64Value("real64", distance)});
}

FunctionCallResult CosineSimilarity(const FunctionCallRequest& request, bool distance) {
  if (AnyNull(request)) return ReturnNull(request, "real64");
  ParsedVector left;
  ParsedVector right;
  auto guard = RequireTwoVectors(request, &left, &right, "cosine distance expects two vectors");
  if (!guard.result.ok()) return guard;

  const double left_norm = NormSquared(left.values);
  const double right_norm = NormSquared(right.values);
  if (left_norm == 0.0 || right_norm == 0.0) {
    return RefuseFunctionInvalidInput(request, "cosine distance requires non-zero vectors");
  }
  const double similarity = Dot(left.values, right.values) / (std::sqrt(left_norm) * std::sqrt(right_norm));
  return MakeFunctionSuccess(request, {MakeReal64Value("real64", distance ? 1.0 - similarity : similarity)});
}

FunctionCallResult VectorDims(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "vector_dims expects one vector");
  if (AnyNull(request)) return ReturnNull(request, "int64");
  const auto parsed = ParseVectorArgument(request, 0);
  if (!parsed.ok()) return RefuseVectorParse(request, parsed);
  return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(parsed.values.size()))});
}

FunctionCallResult VectorNorm(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "vector_norm expects one vector");
  if (AnyNull(request)) return ReturnNull(request, "real64");
  const auto parsed = ParseVectorArgument(request, 0);
  if (!parsed.ok()) return RefuseVectorParse(request, parsed);
  return MakeFunctionSuccess(request, {MakeReal64Value("real64", std::sqrt(NormSquared(parsed.values)))});
}

FunctionCallResult VectorAggregate(const FunctionCallRequest& request, bool average) {
  if (request.arguments.empty()) {
    return RefuseFunctionInvalidInput(
        request, average ? "vector_avg expects at least one dense vector" : "vector_sum expects at least one dense vector");
  }
  if (AnyNull(request)) return ReturnNull(request, "dense_vector");

  std::vector<double> accumulator;
  std::size_t vector_count = 0;
  for (const auto& argument : request.arguments) {
    if (argument.value.has_real64_value || argument.value.has_int64_value || argument.value.has_uint64_value) {
      return RefuseFunctionInvalidInput(
          request, average ? "vector_avg expects dense-vector arguments" : "vector_sum expects dense-vector arguments");
    }
    const auto parsed = ParseVectorText(ValueAsText(argument.value));
    if (!parsed.ok()) return RefuseVectorParse(request, parsed);
    if (vector_count == 0) {
      accumulator = parsed.values;
    } else {
      if (!SameDimension(accumulator, parsed.values)) {
        return RefuseFunctionInvalidInput(request, "vector aggregate dimensions must match and be non-empty");
      }
      for (std::size_t i = 0; i < accumulator.size(); ++i) accumulator[i] += parsed.values[i];
    }
    ++vector_count;
  }

  if (average) {
    for (auto& value : accumulator) value /= static_cast<double>(vector_count);
  }
  return ReturnVector(request, accumulator);
}

FunctionCallResult NormalizeVector(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "vector_l2_normalize expects one vector");
  if (AnyNull(request)) return ReturnNull(request, "dense_vector");
  auto parsed = ParseVectorArgument(request, 0);
  if (!parsed.ok()) return RefuseVectorParse(request, parsed);
  const double norm = std::sqrt(NormSquared(parsed.values));
  if (norm == 0.0) return RefuseFunctionInvalidInput(request, "vector_l2_normalize requires a non-zero vector");
  for (auto& value : parsed.values) value /= norm;
  return ReturnVector(request, parsed.values);
}

FunctionCallResult Subvector(const FunctionCallRequest& request) {
  if (request.arguments.size() != 3) return RefuseFunctionInvalidInput(request, "subvector expects vector, start, and length");
  if (AnyNull(request)) return ReturnNull(request, "dense_vector");
  const auto parsed = ParseVectorArgument(request, 0);
  if (!parsed.ok()) return RefuseVectorParse(request, parsed);
  const auto start_text = ValueAsText(request.arguments[1].value);
  const auto length_text = ValueAsText(request.arguments[2].value);
  char* start_end = nullptr;
  char* length_end = nullptr;
  const long start = std::strtol(start_text.c_str(), &start_end, 10);
  const long length = std::strtol(length_text.c_str(), &length_end, 10);
  if (start_end == start_text.c_str() || *start_end != '\0' || length_end == length_text.c_str() || *length_end != '\0') {
    return RefuseFunctionInvalidInput(request, "subvector start and length must be integers");
  }
  if (start < 1 || length < 1) return RefuseFunctionInvalidInput(request, "subvector uses one-based positive start and length");
  const auto zero_based = static_cast<std::size_t>(start - 1);
  const auto count = static_cast<std::size_t>(length);
  if (zero_based >= parsed.values.size() || count > parsed.values.size() - zero_based) {
    return RefuseFunctionInvalidInput(request, "subvector range exceeds vector dimensions");
  }
  return ReturnVector(request, std::vector<double>(parsed.values.begin() + static_cast<std::ptrdiff_t>(zero_based),
                                                  parsed.values.begin() + static_cast<std::ptrdiff_t>(zero_based + count)));
}

FunctionCallResult HammingDistance(const FunctionCallRequest& request) {
  if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "hamming_distance expects two bit vectors");
  if (AnyNull(request)) return ReturnNull(request, "int64");

  std::string error;
  const auto left = ParseBitVectorText(ValueAsText(request.arguments[0].value), &error);
  if (!left) return RefuseFunctionInvalidInput(request, error);
  const auto right = ParseBitVectorText(ValueAsText(request.arguments[1].value), &error);
  if (!right) return RefuseFunctionInvalidInput(request, error);
  if (left->size() != right->size()) return RefuseFunctionInvalidInput(request, "bit vector dimensions must match");

  std::int64_t distance = 0;
  for (std::size_t i = 0; i < left->size(); ++i) {
    if ((*left)[i] != (*right)[i]) ++distance;
  }
  return MakeFunctionSuccess(request, {MakeInt64Value("int64", distance)});
}

FunctionCallResult CastInt8(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "vector_cast_int8 expects one vector");
  if (AnyNull(request)) return ReturnNull(request, "int8_vector");
  const auto parsed = ParseVectorArgument(request, 0);
  if (!parsed.ok()) return RefuseVectorParse(request, parsed);

  std::vector<std::int64_t> out;
  out.reserve(parsed.values.size());
  for (const auto value : parsed.values) {
    const auto rounded = static_cast<std::int64_t>(std::llround(value));
    out.push_back(std::clamp<std::int64_t>(rounded, -128, 127));
  }
  return ReturnIntVector(request, out, "int8_vector");
}

FunctionCallResult CastFloat16(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "vector_cast_float16 expects one vector");
  if (AnyNull(request)) return ReturnNull(request, "float16_vector");
  auto parsed = ParseVectorArgument(request, 0);
  if (!parsed.ok()) return RefuseVectorParse(request, parsed);

  for (auto& value : parsed.values) {
    if (std::fabs(value) > 65504.0) return RefuseFunctionOverflow(request, "vector_cast_float16 element exceeds finite float16 range");
    value = static_cast<double>(HalfBitsToFloat(FloatToHalfBits(static_cast<float>(value))));
  }
  return ReturnVector(request, parsed.values, "float16_vector");
}

FunctionCallResult VectorSearch(const FunctionCallRequest& request) {
  if (request.arguments.size() < 2) return RefuseFunctionInvalidInput(request, "vector.search expects query vector and at least one candidate vector");
  if (AnyNull(request)) return ReturnNull(request, "uint64");
  const auto query = ParseVectorArgument(request, 0);
  if (!query.ok()) return RefuseVectorParse(request, query);
  double best_distance = 0.0;
  std::size_t best_index = 0;
  bool found = false;
  for (std::size_t index = 1; index < request.arguments.size(); ++index) {
    const auto candidate = ParseVectorArgument(request, index);
    if (!candidate.ok() || !SameDimension(query.values, candidate.values)) continue;
    double distance = 0.0;
    for (std::size_t dim = 0; dim < query.values.size(); ++dim) {
      const double delta = query.values[dim] - candidate.values[dim];
      distance += delta * delta;
    }
    distance = std::sqrt(distance);
    if (!found || distance < best_distance) {
      found = true;
      best_distance = distance;
      best_index = index - 1;
    }
  }
  if (!found) return ReturnNull(request, "uint64");
  return MakeFunctionSuccess(request, {MakeUint64Value("uint64", best_index)});
}

}  // namespace

bool IsVectorFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  return id == "vector" || id.rfind("vector.", 0) == 0 || id.rfind("sb.vector.", 0) == 0 ||
         id.rfind("sb.fn.vector.", 0) == 0 || IdIs(id, {"l2_distance", "cosine_distance", "inner_product",
                                                        "negative_inner_product", "hamming_distance", "vector_dims",
                                                        "vector_norm", "vector_sum", "vector_avg",
                                                        "vector_l2_normalize", "subvector",
                                                        "vector_cast_int8", "vector_cast_float16"});
}

FunctionCallResult DispatchVectorFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  if (id == "vector" || IdIs(id, {"vector", "construct"})) {
    return ConstructVector(request);
  }
  if (IdIs(id, {"dimension", "vector_dims"})) {
    return VectorDims(request);
  }
  if (IdIs(id, {"dot", "inner_product"}) || IdEndsWith(id, "vector_distance.distance_inner_product")) {
    return InnerProduct(request, false);
  }
  if (IdIs(id, {"distance", "l2_distance"}) || IdEndsWith(id, "vector_distance.distance_l2") ||
      IdEndsWith(id, "vector_distance.distance_generic")) {
    return L2Distance(request);
  }
  if (IdEndsWith(id, "vector_distance.distance_l1")) {
    return L1Distance(request);
  }
  if (IdEndsWith(id, "vector_distance.distance_jaccard")) {
    return JaccardDistance(request);
  }
  if (IdIs(id, {"cosine_similarity"})) {
    return CosineSimilarity(request, false);
  }
  if (IdIs(id, {"cosine_distance"}) || IdEndsWith(id, "vector_distance.distance_cosine")) {
    return CosineSimilarity(request, true);
  }
  if (IdIs(id, {"negative_inner_product"})) {
    return InnerProduct(request, true);
  }
  if (IdIs(id, {"vector_norm"}) || IdEndsWith(id, "vector_distance.distance_norm")) {
    return VectorNorm(request);
  }
  if (IdIs(id, {"vector_sum"}) || IdEndsWith(id, "vector_aggregate.aggregate_sum")) {
    return VectorAggregate(request, false);
  }
  if (IdIs(id, {"vector_avg"}) || IdEndsWith(id, "vector_aggregate.aggregate_avg")) {
    return VectorAggregate(request, true);
  }
  if (IdIs(id, {"normalize", "vector_l2_normalize"})) {
    return NormalizeVector(request);
  }
  if (IdIs(id, {"subvector"})) {
    return Subvector(request);
  }
  if (IdIs(id, {"hamming_distance"}) || IdEndsWith(id, "vector_distance.distance_hamming")) {
    return HammingDistance(request);
  }
  if (IdIs(id, {"vector_cast_int8"})) {
    return CastInt8(request);
  }
  if (IdIs(id, {"vector_cast_float16"})) {
    return CastFloat16(request);
  }
  if (IdIs(id, {"search"}) || IdEndsWith(id, "vector_search.search_ann") ||
      IdEndsWith(id, "vector_search.search_filter") ||
      IdEndsWith(id, "vector_search.search_hybrid") ||
      IdEndsWith(id, "vector_search.search_index_query") ||
      IdEndsWith(id, "vector_search.search_knn")) {
    return VectorSearch(request);
  }
  if (id == "vector.index_status" || id == "sb.fn.vector.index_status") {
    return MakeFunctionSuccess(request, {MakeTextValue("vector_index_status", "available:planner_test_surface")});
  }
  if (id == "vector.descriptor_accepts") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "vector descriptor_accepts expects descriptor id");
    const auto descriptor = ValueAsText(request.arguments[0].value);
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", (descriptor == "dense_vector" || descriptor == "sparse_vector" ||
                                                                    descriptor == "vector") ? 1 : 0)});
  }
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_VECTOR_FUNCTION_UNHANDLED",
                                      "vector helper id is not handled by the activated vector scalar surface");
}

}  // namespace scratchbird::engine::functions
