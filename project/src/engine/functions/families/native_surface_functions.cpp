// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/native_surface_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace scratchbird::engine::functions {
namespace {

bool IdIs(std::string_view id, std::initializer_list<std::string_view> candidates) {
  for (const auto candidate : candidates) {
    if (id == candidate) return true;
  }
  return false;
}

std::string Trim(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base(), value.end());
  return value;
}

std::string JsonEscape(std::string_view value) {
  std::string out = "\"";
  for (const char ch : value) {
    switch (ch) {
      case '\\':
      case '"':
        out.push_back('\\');
        out.push_back(ch);
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  out.push_back('"');
  return out;
}

bool LooksJsonScalar(const std::string& value) {
  const std::string text = Trim(value);
  if (text.empty()) return false;
  if ((text.front() == '[' && text.back() == ']') ||
      (text.front() == '{' && text.back() == '}') ||
      (text.front() == '"' && text.back() == '"')) {
    return true;
  }
  if (text == "null" || text == "true" || text == "false") return true;
  return std::isdigit(static_cast<unsigned char>(text.front())) || text.front() == '-';
}

std::string JsonScalarFromValue(const scratchbird::engine::sblr::SblrValue& value) {
  if (IsSqlNull(value)) return "null";
  if (value.has_int64_value) return std::to_string(value.int64_value);
  if (value.has_uint64_value) return std::to_string(value.uint64_value);
  if (!value.encoded_value.empty() &&
      (value.descriptor_id == "array" || value.descriptor_id == "json_document") &&
      LooksJsonScalar(value.encoded_value)) {
    return Trim(value.encoded_value);
  }
  return JsonEscape(ValueAsText(value));
}

std::string JsonArrayFromArguments(const FunctionCallRequest& request) {
  std::string out = "[";
  for (std::size_t i = 0; i < request.arguments.size(); ++i) {
    if (i != 0) out += ",";
    out += JsonScalarFromValue(request.arguments[i].value);
  }
  out += "]";
  return out;
}

bool ParseInt64Strict(const scratchbird::engine::sblr::SblrValue& value, std::int64_t* out) {
  if (IsSqlNull(value)) return false;
  if (value.has_int64_value) {
    *out = value.int64_value;
    return true;
  }
  const std::string text = Trim(ValueAsText(value));
  if (text.empty()) return false;
  const char* begin = text.data();
  const char* end = begin + text.size();
  std::int64_t parsed = 0;
  const auto result = std::from_chars(begin, end, parsed, 10);
  if (result.ec != std::errc() || result.ptr != end) return false;
  *out = parsed;
  return true;
}

FunctionCallResult NoArgText(const FunctionCallRequest& request,
                             std::string_view function_name,
                             std::string_view descriptor,
                             std::string_view value) {
  if (!request.arguments.empty()) {
    return RefuseFunctionInvalidInput(request, std::string(function_name) + " expects no arguments");
  }
  return MakeFunctionSuccess(request, {MakeTextValue(std::string(descriptor), std::string(value))});
}

FunctionCallResult AcceptSurface(const FunctionCallRequest& request) {
  if (request.arguments.empty()) {
    return MakeFunctionSuccess(request, {MakeTextValue("character", "acceptance.surface")});
  }
  if (request.arguments.size() != 1) {
    return RefuseFunctionInvalidInput(request, "accept expects zero arguments or one feature descriptor");
  }
  const std::string feature = Trim(ValueAsText(request.arguments[0].value));
  return MakeFunctionSuccess(request, {MakeInt64Value("boolean", feature.empty() ? 0 : 1)});
}

FunctionCallResult AnyValue(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("character", "aggregate.any_value")});
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "any_value expects one expression");
  return MakeFunctionSuccess(request, {request.arguments[0].value});
}

FunctionCallResult Collect(const FunctionCallRequest& request) {
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", JsonArrayFromArguments(request))});
}

FunctionCallResult AtTimeZone(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("character", "temporal.at_time_zone")});
  if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "at_time_zone expects timestamp and zone");
  if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
    return MakeFunctionSuccess(request, {MakeNullValue("timestamp_tz")});
  }
  return MakeFunctionSuccess(
      request,
      {MakeTextValue("timestamp_tz", ValueAsText(request.arguments[0].value) + " " + ValueAsText(request.arguments[1].value))});
}

FunctionCallResult BitString(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "bit_string expects one argument");
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("bit_string")});
  std::string text = Trim(ValueAsText(request.arguments[0].value));
  if ((text.rfind("B'", 0) == 0 || text.rfind("b'", 0) == 0) && text.size() >= 3 && text.back() == '\'') {
    text = text.substr(2, text.size() - 3);
  }
  for (const char ch : text) {
    if (ch != '0' && ch != '1') return RefuseFunctionInvalidInput(request, "bit_string accepts only 0/1 bits");
  }
  return MakeFunctionSuccess(request, {MakeTextValue("bit_string", text)});
}

FunctionCallResult BulkExceptions(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("json_document", "[]")});
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "bulk_exceptions expects zero or one descriptor");
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeTextValue("json_document", "[]")});
  const std::string text = Trim(ValueAsText(request.arguments[0].value));
  if (!text.empty() && text.front() == '[' && text.back() == ']') {
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", text)});
  }
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", "[" + JsonEscape(text) + "]")});
}

FunctionCallResult DomainStack(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("json_document", "[]")});
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "domain_stack expects zero or one value");
  std::string out = "[{\"value\":";
  out += JsonScalarFromValue(request.arguments[0].value);
  out += ",\"domain\":\"";
  out += request.arguments[0].value.descriptor_id.empty() ? "unknown" : request.arguments[0].value.descriptor_id;
  out += "\"}]";
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", std::move(out))});
}

FunctionCallResult MatchRecognize(const FunctionCallRequest& request) {
  if (request.arguments.size() > 1) return RefuseFunctionInvalidInput(request, "match_recognize expects optional pattern descriptor");
  const std::string pattern = request.arguments.empty() || IsSqlNull(request.arguments[0].value)
                                  ? std::string()
                                  : ValueAsText(request.arguments[0].value);
  std::string out = "{\"kind\":\"match_recognize\",\"version\":\"v1\",\"pattern\":";
  out += JsonEscape(pattern);
  out += "}";
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", std::move(out))});
}

FunctionCallResult Treat(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("character", "special_form.treat")});
  if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "treat expects expression and subtype descriptor");
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue(ValueAsText(request.arguments[1].value))});
  auto value = request.arguments[0].value;
  const std::string descriptor = Trim(ValueAsText(request.arguments[1].value));
  if (!descriptor.empty()) value.descriptor_id = descriptor;
  return MakeFunctionSuccess(request, {std::move(value)});
}

FunctionCallResult IntegerSurface(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("type_descriptor", "integer")});
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "integer expects zero or one value");
  std::int64_t parsed = 0;
  if (!ParseInt64Strict(request.arguments[0].value, &parsed)) {
    return RefuseFunctionInvalidInput(request, "integer expects an exact int64-compatible value");
  }
  return MakeFunctionSuccess(request, {MakeInt64Value("int64", parsed)});
}

FunctionCallResult Nvl(const FunctionCallRequest& request) {
  if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "nvl expects two arguments");
  return MakeFunctionSuccess(request, {IsSqlNull(request.arguments[0].value) ? request.arguments[1].value : request.arguments[0].value});
}

FunctionCallResult VoidSurface(const FunctionCallRequest& request) {
  if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "void expects no arguments");
  return MakeFunctionSuccess(request, {MakeNullValue("void")});
}

FunctionCallResult TabularSurface(const FunctionCallRequest& request) {
  if (!request.arguments.empty()) return RefuseFunctionInvalidInput(request, "tabular expects no arguments");
  return MakeFunctionSuccess(
      request,
      {MakeTextValue("json_document", "{\"kind\":\"tabular\",\"columns\":[],\"rows\":[],\"row_count\":0}")});
}

}  // namespace

bool IsNativeSurfaceFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  return request.context.package_name == "surface.scalar" ||
         IdIs(id, {"sb.aggregate.any_value", "sb.aggregate.any_value_expr",
                   "sb.aggregate.collect", "sb.aggregate.collect_expr", "sb.scalar.accept",
                   "sb.scalar.accept_sql2016_timeseries", "sb.scalar.at_time_zone",
                   "sb.scalar.bit_string", "sb.scalar.bulk_exceptions", "sb.scalar.close",
                   "sb.scalar.domain_stack", "sb.scalar.domain_stack_value",
                   "sb.scalar.reference_only", "sb.scalar.reference_rewrite",
                   "sb.scalar.future_version", "sb.scalar.gap", "sb.scalar.immutable",
                   "sb.scalar.match_recognize", "sb.scalar.native_future", "sb.scalar.native_now",
                   "sb.scalar.nvl", "sb.scalar.open", "sb.scalar.private_only", "sb.scalar.reserved",
                   "sb.scalar.sbsql_syntax_future_version", "sb.scalar.sbsql_syntax_reserved",
                   "sb.scalar.stable", "sb.scalar.tabular", "sb.scalar.treat",
                   "sb.scalar.treat_typed", "sb.scalar.volatile",
                   "sb.scalar.void", "sb.expr.match_recognize.v1", "sb.type.integer"});
}

FunctionCallResult DispatchNativeSurfaceFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;

  if (IdIs(id, {"sb.scalar.accept", "sb.scalar.accept_sql2016_timeseries"})) return AcceptSurface(request);
  if (IdIs(id, {"sb.aggregate.any_value", "sb.aggregate.any_value_expr"})) return AnyValue(request);
  if (IdIs(id, {"sb.aggregate.collect", "sb.aggregate.collect_expr"})) return Collect(request);
  if (IdIs(id, {"sb.scalar.at_time_zone"})) return AtTimeZone(request);
  if (IdIs(id, {"sb.scalar.bit_string"})) return BitString(request);
  if (IdIs(id, {"sb.scalar.bulk_exceptions"})) return BulkExceptions(request);
  if (IdIs(id, {"sb.scalar.domain_stack", "sb.scalar.domain_stack_value"})) return DomainStack(request);
  if (IdIs(id, {"sb.scalar.match_recognize", "sb.expr.match_recognize.v1"})) return MatchRecognize(request);
  if (IdIs(id, {"sb.scalar.treat", "sb.scalar.treat_typed"})) return Treat(request);
  if (IdIs(id, {"sb.type.integer"})) return IntegerSurface(request);
  if (IdIs(id, {"sb.scalar.nvl"})) return Nvl(request);
  if (IdIs(id, {"sb.scalar.tabular"})) return TabularSurface(request);
  if (IdIs(id, {"sb.scalar.void"})) return VoidSurface(request);

  if (IdIs(id, {"sb.scalar.close"})) return NoArgText(request, "close", "character", "keyword.close");
  if (IdIs(id, {"sb.scalar.open"})) return NoArgText(request, "open", "character", "keyword.open");
  if (IdIs(id, {"sb.scalar.future_version"})) return NoArgText(request, "future_version", "character", "syntax.future_version");
  if (IdIs(id, {"sb.scalar.gap"})) return NoArgText(request, "gap", "character", "surface.gap");
  if (IdIs(id, {"sb.scalar.immutable"})) return NoArgText(request, "immutable", "character", "volatility.immutable");
  if (IdIs(id, {"sb.scalar.reserved"})) return NoArgText(request, "reserved", "character", "syntax.reserved");
  if (IdIs(id, {"sb.scalar.sbsql_syntax_future_version"})) {
    return NoArgText(request, "sbsql_syntax_future_version", "character", "sbsql.syntax.future_version");
  }
  if (IdIs(id, {"sb.scalar.sbsql_syntax_reserved"})) {
    return NoArgText(request, "sbsql_syntax_reserved", "character", "sbsql.syntax.reserved");
  }
  if (IdIs(id, {"sb.scalar.stable"})) return NoArgText(request, "stable", "character", "volatility.stable");
  if (IdIs(id, {"sb.scalar.volatile"})) return NoArgText(request, "volatile", "character", "volatility.volatile");
  if (IdIs(id, {"sb.scalar.reference_only"})) return NoArgText(request, "reference_only", "character", "surface.reference_only");
  if (IdIs(id, {"sb.scalar.reference_rewrite"})) return NoArgText(request, "reference_rewrite", "character", "surface.reference_rewrite");
  if (IdIs(id, {"sb.scalar.native_future"})) return NoArgText(request, "native_future", "character", "status.native_future");
  if (IdIs(id, {"sb.scalar.native_now"})) return NoArgText(request, "native_now", "character", "status.native_now");
  if (IdIs(id, {"sb.scalar.private_only"})) return NoArgText(request, "private_only", "character", "surface.private_only");

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_FUNCTION_NOT_IMPLEMENTED",
                                      "native surface function is registered but has no implementation route");
}

}  // namespace scratchbird::engine::functions
