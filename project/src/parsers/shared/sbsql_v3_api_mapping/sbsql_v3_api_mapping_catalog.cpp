// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbsql_v3_api_mapping_catalog.hpp"

#include <utility>

namespace scratchbird::parser::sbsql_v3_api_mapping {
namespace {

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

void AddError(std::vector<std::string>* errors, std::string message) {
  if (errors) {
    errors->push_back(std::move(message));
  }
}

}  // namespace

bool IsUuidV7(std::string_view uuid_text) {
  return uuid_text.size() == 36 && uuid_text[8] == '-' && uuid_text[13] == '-' &&
         uuid_text[18] == '-' && uuid_text[23] == '-' && uuid_text[14] == '7';
}

bool ValidateApiMappingEntry(const ApiMappingEntry& entry, std::vector<std::string>* errors) {
  const auto before = errors ? errors->size() : 0;
  if (!StartsWith(entry.sblr_operation, "SBLR_")) {
    AddError(errors, "SBLR operation must use SBLR_ prefix");
  }
  if (entry.api_operation_id.empty()) {
    AddError(errors, "API operation ID is required");
  }
  if (entry.api_function_name.empty()) {
    AddError(errors, "API function name is required");
  }
  if (entry.request_type.empty()) {
    AddError(errors, "request type is required");
  }
  if (entry.result_type.empty()) {
    AddError(errors, "result type is required");
  }
  if (!entry.typed_request_mapping) {
    AddError(errors, "typed request mapping is required");
  }
  if (entry.raw_sql_fallback_allowed) {
    AddError(errors, "raw SQL fallback is forbidden");
  }
  if (entry.cluster_authority_required && !entry.fail_closed_without_cluster_authority) {
    AddError(errors, "cluster authority mappings must fail closed");
  }
  return !errors || errors->size() == before;
}

MappedApiRequest MapSblrToApiRequest(const ApiMappingEntry& entry, const SblrToApiRequestInput& input) {
  std::vector<std::string> errors;
  if (!ValidateApiMappingEntry(entry, &errors)) {
    return {.ok = false, .diagnostic_code = "SBSQL_V3_INVALID_API_MAPPING"};
  }
  if (input.sblr_operation != entry.sblr_operation) {
    return {.ok = false, .diagnostic_code = "SBSQL_V3_SBLR_API_OPERATION_MISMATCH"};
  }
  if (input.contains_raw_sql_text) {
    return {.ok = false, .diagnostic_code = "SBSQL_V3_RAW_SQL_FALLBACK_FORBIDDEN"};
  }
  if (!IsUuidV7(input.bound_root_uuid)) {
    return {.ok = false, .diagnostic_code = "SBSQL_V3_BOUND_ROOT_UUID_MUST_BE_V7"};
  }
  if (input.descriptor_digest.empty()) {
    return {.ok = false, .diagnostic_code = "SBSQL_V3_DESCRIPTOR_DIGEST_REQUIRED"};
  }
  if (entry.cluster_authority_required && !input.cluster_authority_present) {
    return {.ok = false, .diagnostic_code = "SBSQL_V3_CLUSTER_AUTHORITY_REQUIRED"};
  }
  return {.ok = true,
          .api_operation_id = entry.api_operation_id,
          .request_type = entry.request_type,
          .result_type = entry.result_type,
          .diagnostic_code = "SB_ENGINE_API_OK"};
}

}  // namespace scratchbird::parser::sbsql_v3_api_mapping
