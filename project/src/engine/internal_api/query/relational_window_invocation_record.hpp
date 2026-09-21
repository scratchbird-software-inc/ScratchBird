// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct RelationalWindowInvocationRecord {
  std::uint32_t invocation_id{0};
  std::uint32_t relation_node_id{0};
  std::uint32_t function_expression_id{0};
  std::uint32_t window_definition_id{0};
  std::uint16_t function_abi_version{0};
  std::string builtin_id;
  EngineUuid function_uuid;
  std::uint32_t result_descriptor_id{0};
  std::string output_name_utf8;
  std::vector<std::uint32_t> argument_expression_ids;
};

}  // namespace scratchbird::engine::internal_api
