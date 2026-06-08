// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/nosql_kv_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <map>
#include <mutex>
#include <string>

namespace scratchbird::engine::functions {
namespace {

std::mutex& KvMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, std::string>& KvStore() {
  static std::map<std::string, std::string> store;
  return store;
}

std::string KeyFor(const FunctionCallRequest& request, std::size_t key_index) {
  const std::string key = ValueAsText(request.arguments[key_index].value);
  const std::string scope = request.context.sblr_context.database_uuid.empty()
                                ? "process"
                                : request.context.sblr_context.database_uuid;
  return scope + ":" + key;
}

bool DescriptorAcceptsKv(const std::string& descriptor_id) {
  return descriptor_id == "kv" || descriptor_id == "key_value" || descriptor_id == "map" ||
         descriptor_id == "document" || descriptor_id == "variant";
}

}  // namespace

bool IsNoSqlKvFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("nosql.kv.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.kv.", 0) == 0;
}

FunctionCallResult DispatchNoSqlKvFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  if (id == "nosql.kv.put" || id == "sb.fn.kv.put") {
    if (request.arguments.size() != 2 || IsSqlNull(request.arguments[0].value)) {
      return RefuseFunctionInvalidInput(request, "kv.put expects non-null key and value");
    }
    std::lock_guard<std::mutex> guard(KvMutex());
    KvStore()[KeyFor(request, 0)] = IsSqlNull(request.arguments[1].value) ? "<NULL>" : ValueAsText(request.arguments[1].value);
    return MakeFunctionSuccess(request, {MakeTextValue("kv_entry", ValueAsText(request.arguments[0].value))});
  }
  if (id == "nosql.kv.get" || id == "sb.fn.kv.get") {
    if (request.arguments.size() != 1 || IsSqlNull(request.arguments[0].value)) {
      return RefuseFunctionInvalidInput(request, "kv.get expects non-null key");
    }
    std::lock_guard<std::mutex> guard(KvMutex());
    const auto found = KvStore().find(KeyFor(request, 0));
    if (found == KvStore().end() || found->second == "<NULL>") return MakeFunctionSuccess(request, {MakeNullValue("kv_value")});
    return MakeFunctionSuccess(request, {MakeTextValue("kv_value", found->second)});
  }
  if (id == "nosql.kv.delete" || id == "sb.fn.kv.delete") {
    if (request.arguments.size() != 1 || IsSqlNull(request.arguments[0].value)) {
      return RefuseFunctionInvalidInput(request, "kv.delete expects non-null key");
    }
    std::lock_guard<std::mutex> guard(KvMutex());
    const auto erased = KvStore().erase(KeyFor(request, 0));
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", erased == 0 ? 0 : 1)});
  }
  if (id == "nosql.kv.scan" || id == "sb.fn.kv.scan") {
    std::lock_guard<std::mutex> guard(KvMutex());
    std::string out = "[";
    bool first = true;
    const std::string scope = request.context.sblr_context.database_uuid.empty() ? "process:" : request.context.sblr_context.database_uuid + ":";
    for (const auto& [key, value] : KvStore()) {
      if (key.rfind(scope, 0) != 0) continue;
      if (!first) out += ",";
      first = false;
      out += "\"" + key.substr(scope.size()) + "\"";
    }
    out += "]";
    return MakeFunctionSuccess(request, {MakeTextValue("array", out)});
  }
  if (id == "nosql.kv.descriptor_accepts") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "kv descriptor_accepts expects descriptor id");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", DescriptorAcceptsKv(ValueAsText(request.arguments[0].value)) ? 1 : 0)});
  }
  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_NOSQL_KV_FUNCTION_UNHANDLED",
                                      "key/value function id is not handled by the activated KV planner-test surface");
}
}  // namespace scratchbird::engine::functions
