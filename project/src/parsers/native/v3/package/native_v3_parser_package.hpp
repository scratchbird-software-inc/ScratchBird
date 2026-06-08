// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "diagnostics/diagnostic_rendering.hpp"

#include <string>
#include <vector>

namespace scratchbird::parser::native_v3_package {

// SEARCH_KEY: SB_NATIVE_V3_PARSER_PACKAGE_PIPELINE
// SBsql parser package integration. Parser package output is translation
// evidence only until the engine accepts SBLR and dispatches through its own
// API and security/transaction gates.

struct NativeV3ParserPackageRequest {
  std::string command_text;
  std::string database_path;
  std::string database_uuid;
  std::string session_uuid;
  std::string principal_uuid;
  std::string parser_package_uuid;
  std::string parser_package_version;
  std::string registry_snapshot_uuid;
  std::string catalog_epoch;
  std::string client_dialect = "sbsql_v3";
  std::string language_tag = "en";
  bool security_context_present = true;
  bool cluster_authority_available = false;
};

struct NativeV3ParserPackageResult {
  bool ok = false;
  std::string failed_stage;
  bool parser_is_trusted = false;
  bool sblr_contains_sql_text = false;
  bool sblr_parser_resolved_names_to_uuids = false;
  bool sblr_envelope_validated = false;
  bool dispatched_to_engine_api = false;
  bool rendered_for_parser_package = false;
  std::string ast_json;
  std::string bound_ast_json;
  std::string logical_envelope_json;
  std::string sblr_envelope_json;
  std::string dispatch_json;
  scratchbird::engine::internal_api::EngineRenderedResultEnvelope rendered_result;
  std::vector<std::string> diagnostics;
};

NativeV3ParserPackageResult ExecuteNativeV3ParserPackageRequest(const NativeV3ParserPackageRequest& request);

std::string SerializeNativeV3ParserPackageResultToJson(const NativeV3ParserPackageResult& result);

}  // namespace scratchbird::parser::native_v3_package
