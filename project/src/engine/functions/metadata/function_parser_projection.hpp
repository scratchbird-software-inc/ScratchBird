// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "registry/function_registry.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::functions {

enum class FunctionAliasSource {
  sb_native,
  donor,
  plugin_extension,
};

struct FunctionParserProjectionRequest {
  std::string parser_profile;
  bool metadata_visible = false;
  bool include_disabled = false;
  bool parser_claims_execution_authority = false;
  bool parser_claims_security_authority = false;
};

struct FunctionParserProjectionRow {
  std::string parser_profile;
  std::string alias_name;
  std::string canonical_function_id;
  std::string function_uuid;
  FunctionAliasSource alias_source = FunctionAliasSource::sb_native;
  std::string source_package;
  std::string projection_state;
  std::string result_descriptor_rule;
  std::string diagnostic_rendering_hint;
  std::string refusal_policy;
  bool parser_may_submit_sblr = true;
  bool parser_has_authority = false;
  bool metadata_redacted = false;
};

struct FunctionParserAuthorityDecision {
  bool allowed = false;
  std::string diagnostic_id;
  std::string detail;
};

std::vector<FunctionParserProjectionRow> BuildFunctionParserProjection(
    const FunctionRegistry& registry,
    const FunctionParserProjectionRequest& request);
FunctionParserAuthorityDecision ValidateFunctionParserProjectionAuthority(
    const FunctionParserProjectionRequest& request);
const char* ToString(FunctionAliasSource source);

}  // namespace scratchbird::engine::functions
