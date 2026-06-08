// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/search_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace scratchbird::engine::functions {
namespace {

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::vector<std::string> Tokenize(std::string value) {
  std::istringstream in(LowerAscii(std::move(value)));
  std::vector<std::string> tokens;
  std::string token;
  while (in >> token) tokens.push_back(token);
  return tokens;
}

std::string TokensAsArray(const std::vector<std::string>& tokens) {
  std::string out = "[";
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    if (i != 0) out += ",";
    out += "\"" + tokens[i] + "\"";
  }
  out += "]";
  return out;
}

std::uint64_t CountOccurrences(std::string text, std::string query) {
  text = LowerAscii(std::move(text));
  query = LowerAscii(std::move(query));
  if (query.empty()) return 0;
  std::uint64_t count = 0;
  std::size_t pos = 0;
  while ((pos = text.find(query, pos)) != std::string::npos) {
    ++count;
    pos += query.size();
  }
  return count;
}

bool IdEndsWith(const std::string& id, std::string_view suffix) {
  return id.size() >= suffix.size() && id.compare(id.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

bool IsSearchFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("search.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.search.", 0) == 0;
}

FunctionCallResult DispatchSearchFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  if (id == "search.normalize" || id == "sb.fn.search.normalize") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "search.normalize expects text");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", LowerAscii(ValueAsText(request.arguments[0].value)))});
  }
  if (id == "search.tokenize" || id == "sb.fn.search.tokenize") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "search.tokenize expects text");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("array")});
    return MakeFunctionSuccess(request, {MakeTextValue("array", TokensAsArray(Tokenize(ValueAsText(request.arguments[0].value))))});
  }
  if (id == "search.match" || id == "sb.fn.search.match" || id == "sb.fn.search.sb_search_query") {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "search.match expects text and query");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", CountOccurrences(ValueAsText(request.arguments[0].value), ValueAsText(request.arguments[1].value)) > 0 ? 1 : 0)});
  }
  if (IdEndsWith(id, "search_query.query_match") || IdEndsWith(id, "search_query.query_phrase") ||
      IdEndsWith(id, "search_query.query_boolean")) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "search query helper expects text and query");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", CountOccurrences(ValueAsText(request.arguments[0].value), ValueAsText(request.arguments[1].value)) > 0 ? 1 : 0)});
  }
  if (id == "search.rank" || id == "sb.fn.search.rank") {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "search.rank expects text and query");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", static_cast<double>(CountOccurrences(ValueAsText(request.arguments[0].value), ValueAsText(request.arguments[1].value))))});
  }
  if (IdEndsWith(id, "search_rank.rank_bm25") || IdEndsWith(id, "search_rank.rank_score")) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "search rank helper expects text and query");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("real64")});
    return MakeFunctionSuccess(request, {MakeReal64Value("real64", static_cast<double>(CountOccurrences(ValueAsText(request.arguments[0].value), ValueAsText(request.arguments[1].value))))});
  }
  if (id == "search.query" || id == "sb.fn.search.query") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "search.query expects a query string");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("search_query")});
    return MakeFunctionSuccess(request, {MakeTextValue("search_query", TokensAsArray(Tokenize(ValueAsText(request.arguments[0].value))))});
  }
  if (id == "search.index_status" || id == "sb.fn.search.index_status") {
    return MakeFunctionSuccess(request, {MakeTextValue("search_index_status", "available:planner_test_surface")});
  }
  if (IdEndsWith(id, "search_dictionary.dictionary_lookup")) {
    if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "search dictionary_lookup expects a token");
    return MakeFunctionSuccess(request, {MakeTextValue("search_dictionary_term", LowerAscii(ValueAsText(request.arguments.back().value)))});
  }
  if (IdEndsWith(id, "search_dictionary.dictionary_manage") ||
      IdEndsWith(id, "search_index.index_create") ||
      IdEndsWith(id, "search_index.index_manage")) {
    return RefuseFunctionWithDiagnostic(request,
                                        scratchbird::engine::sblr::SblrStatusCode::policy_refused,
                                        "SB_DIAG_SEARCH_CATALOG_POLICY_REQUIRED",
                                        "search dictionary and index lifecycle operations require catalog DDL authority and cannot execute as scalar helpers");
  }
  if (IdEndsWith(id, "search_snippet.snippet_extract")) {
    if (request.arguments.size() < 2) return RefuseFunctionInvalidInput(request, "search snippet_extract expects text and query");
    const auto text = ValueAsText(request.arguments[0].value);
    const auto query = ValueAsText(request.arguments[1].value);
    const auto pos = LowerAscii(text).find(LowerAscii(query));
    return MakeFunctionSuccess(request, {MakeTextValue("character",
                                                       pos == std::string::npos ? text.substr(0, std::min<std::size_t>(text.size(), 80))
                                                                                : text.substr(pos, std::min<std::size_t>(text.size() - pos, 80)))});
  }
  if (IdEndsWith(id, "search_suggest.suggest_complete")) {
    if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "search suggest_complete expects a prefix");
    return MakeFunctionSuccess(request, {MakeTextValue("array", "[\"" + LowerAscii(ValueAsText(request.arguments[0].value)) + "\"]")});
  }
  if (id == "search.descriptor_accepts") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "search descriptor_accepts expects descriptor id");
    const auto descriptor = ValueAsText(request.arguments[0].value);
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", (descriptor == "search_document" || descriptor == "text" ||
                                                                    descriptor == "character" || descriptor == "fulltext") ? 1 : 0)});
  }
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_SEARCH_FUNCTION_UNHANDLED",
                                      "search helper id is not handled by the activated search scalar surface");
}

}  // namespace scratchbird::engine::functions
