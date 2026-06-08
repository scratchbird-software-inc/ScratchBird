// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_special_forms.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
#include "sblr_special_forms.hpp"

#include "datatype_operations.hpp"
#include "sblr_operator_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace scratchbird::engine::sblr {
namespace {

namespace datatypes = scratchbird::core::datatypes;

SblrValue NullValue(std::string descriptor_id = {}) {
  SblrValue value;
  value.descriptor_id = std::move(descriptor_id);
  value.payload_kind = SblrValuePayloadKind::none;
  value.is_null = true;
  return value;
}

SblrResult ScalarResult(std::string_view operation_id, SblrValue value) {
  SblrResult out = MakeSblrSuccess(std::string(operation_id));
  out.scalar_values.push_back(std::move(value));
  return out;
}

SblrResult SpecialFormFailure(std::string_view operation_id,
                              const SblrExecutionContext& context,
                              std::string diagnostic_id,
                              std::string detail) {
  auto diagnostic = MakeSblrRefusalDiagnostic(std::move(diagnostic_id), context, std::move(detail));
  diagnostic.fields.push_back({"operation_id", std::string(operation_id)});
  return MakeSblrFailure(SblrStatusCode::execution_failed, std::string(operation_id), std::move(diagnostic));
}

SblrResult EvaluateThunkScalar(std::string_view operation_id,
                               const SblrExecutionContext& context,
                               const SblrExpressionThunk& thunk,
                               SblrValue* out) {
  if (!thunk) {
    return SpecialFormFailure(operation_id, context, "SBLR.LAZY_EXPRESSION_REQUIRED", "lazy special form branch is missing an expression thunk");
  }
  SblrResult result = thunk();
  if (!result.ok()) return result;
  if (result.scalar_values.empty()) {
    return SpecialFormFailure(operation_id, context, "SBLR.LAZY_EXPRESSION_EMPTY", "lazy special form branch did not produce a scalar value");
  }
  *out = result.scalar_values.front();
  return MakeSblrSuccess(std::string(operation_id));
}

bool Truthy(const SblrValue& value) {
  if (value.is_null) return false;
  if (value.has_int64_value) return value.int64_value != 0;
  std::string text = value.text_value;
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  return text == "TRUE" || text == "T" || text == "YES" || text == "Y" || text == "1";
}

bool ValuesEqual(const SblrValue& left, const SblrValue& right) {
  if (left.is_null || right.is_null) return false;
  if (left.has_int64_value && right.has_int64_value) return left.int64_value == right.int64_value;
  if (left.has_uint64_value && right.has_uint64_value) return left.uint64_value == right.uint64_value;
  if (left.has_real64_value && right.has_real64_value) return left.real64_value == right.real64_value;
  if (!left.encoded_value.empty() || !right.encoded_value.empty()) return left.encoded_value == right.encoded_value;
  return left.text_value == right.text_value;
}

int CompareValues(const SblrValue& left, const SblrValue& right) {
  if (left.has_int64_value && right.has_int64_value) {
    return left.int64_value < right.int64_value ? -1 : (left.int64_value > right.int64_value ? 1 : 0);
  }
  if (left.has_uint64_value && right.has_uint64_value) {
    return left.uint64_value < right.uint64_value ? -1 : (left.uint64_value > right.uint64_value ? 1 : 0);
  }
  if (left.has_real64_value && right.has_real64_value) {
    return left.real64_value < right.real64_value ? -1 : (left.real64_value > right.real64_value ? 1 : 0);
  }
  if (!left.encoded_value.empty() || !right.encoded_value.empty()) {
    return left.encoded_value.compare(right.encoded_value);
  }
  return left.text_value.compare(right.text_value);
}

std::string Lower(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return out;
}

datatypes::CanonicalTypeId CanonicalTypeFromDescriptor(std::string_view descriptor_id) {
  std::string name = Lower(descriptor_id);
  if (name.empty()) return datatypes::CanonicalTypeId::unknown;
  if (name == "bool") name = "boolean";
  if (name == "string" || name == "text" || name == "varchar" || name == "char") name = "character";
  if (name == "bytes" || name == "varbinary") name = "binary";
  if (name == "json") name = "json_document";
  if (name == "jsonb") name = "binary_json_document";
  if (name == "object") name = "object_document";
  if (name == "flat_object") name = "flattened_object_document";
  if (name == "timestamp_tz" || name == "timestamp_epoch_ms" || name == "datetime") name = "timestamp";
  if (name == "timespan") name = "interval";
  if (name == "set") name = "set_value";
  return datatypes::CanonicalTypeIdFromStableName(name);
