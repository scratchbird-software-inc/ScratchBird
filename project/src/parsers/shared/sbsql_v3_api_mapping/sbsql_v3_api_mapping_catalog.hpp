// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::sbsql_v3_api_mapping {

struct ApiMappingEntry {
  std::string sblr_operation;
  std::string api_operation_id;
  std::string api_function_name;
  std::string request_type;
  std::string result_type;
  std::string payload_class;
  std::string mapping_status;
  bool typed_request_mapping = true;
  bool raw_sql_fallback_allowed = false;
  bool cluster_authority_required = false;
  bool fail_closed_without_cluster_authority = false;
};

struct SblrToApiRequestInput {
  std::string sblr_operation;
  std::string bound_root_uuid;
  std::string descriptor_digest;
  bool contains_raw_sql_text = false;
  bool cluster_authority_present = false;
};

struct MappedApiRequest {
  bool ok = false;
  std::string api_operation_id;
  std::string request_type;
  std::string result_type;
  std::string diagnostic_code;
};

bool IsUuidV7(std::string_view uuid_text);
bool ValidateApiMappingEntry(const ApiMappingEntry& entry, std::vector<std::string>* errors);
MappedApiRequest MapSblrToApiRequest(const ApiMappingEntry& entry, const SblrToApiRequestInput& input);

}  // namespace scratchbird::parser::sbsql_v3_api_mapping
