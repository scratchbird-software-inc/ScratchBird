// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_unsupported.hpp"
#include "diagnostics/diagnostic_rendering.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::string path;
  bool overwrite = false;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") {
      args->path = value;
    } else {
      return false;
    }
  }
  return !args->path.empty();
}

EngineTypedValue TextValue(const std::string& value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = value;
  typed.is_null = false;
  return typed;
}

EngineApiResult MakeSuccessResult() {
  EngineApiResult result;
  result.ok = true;
  result.operation_id = "diagnostics.render_success";
  result.result_shape.result_kind = "canonical_test_rows";
  EngineDescriptor column;
  column.descriptor_kind = "scalar";
  column.canonical_type_name = "text";
  column.encoded_descriptor = "name=answer;type=text";
  result.result_shape.columns.push_back(column);
  EngineRowValue row;
  row.requested_row_uuid.canonical = "00000000-0000-7000-8000-000000001711";
  row.fields.push_back({"answer", TextValue("forty_two")});
  result.result_shape.rows.push_back(row);
  result.evidence.push_back({"canonical_result_shape", "engine_api"});
  return result;
}

EngineApiResult MakeFailureResult() {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  EngineApiRequest request;
  request.context = context;
  return MakeClusterAuthorityUnavailableResult<EngineApiResult>(request, "cluster.inspect_state");
}

EngineParserPackageRenderOptions Options() {
  EngineParserPackageRenderOptions options;
  options.parser_package_uuid = "00000000-0000-7000-8000-000000001701";
  options.parser_package_version = "native-v3-stage14";
  options.client_dialect = "sbsql_v3";
  options.language_tag = "en";
  options.redact_internal_detail = true;
  options.include_evidence = true;
  return options;
}

bool HasRenderedDiagnosticCode(const EngineRenderedResultEnvelope& envelope, const std::string& code) {
  for (const auto& diagnostic : envelope.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasRenderedEvidence(const EngineRenderedResultEnvelope& envelope, const std::string& kind, const std::string& id = {}) {
  for (const auto& evidence : envelope.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
  }
  return false;
}

bool HasRedactedDetail(const EngineRenderedResultEnvelope& envelope) {
  for (const auto& diagnostic : envelope.diagnostics) {
    if (diagnostic.internal_detail_redacted && diagnostic.detail == "redacted") { return true; }
  }
  return false;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_diagnostic_rendering_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  std::vector<std::string> errors;
  const auto success = RenderEngineApiResultForParserPackage(MakeSuccessResult(), Options());
  const bool success_valid = ValidateEngineRenderedResultEnvelope(success, &errors);
  const bool success_ok = success.ok &&
                          success_valid &&
                          success.parser_package_rendering_required &&
                          success.canonical_diagnostics &&
                          success.canonical_result_shape &&
                          success.rows.size() == 1 &&
                          success.rows.front().fields.size() == 1 &&
                          success.rows.front().fields.front().name == "answer" &&
                          HasRenderedEvidence(success, "canonical_result_shape", "engine_api");

  errors.clear();
  const auto failure = RenderEngineApiResultForParserPackage(MakeFailureResult(), Options());
  const bool failure_valid = ValidateEngineRenderedResultEnvelope(failure, &errors);
  const bool failure_ok = !failure.ok &&
                          failure_valid &&
                          HasRenderedDiagnosticCode(failure, "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE") &&
                          HasRenderedDiagnosticCode(failure, "SB_ENGINE_API_EMBEDDED_TRUST_MODE") &&
                          HasRedactedDetail(failure) &&
                          HasRenderedEvidence(failure, "cluster_placeholder", "fail_closed");

  auto invalid_options = Options();
  invalid_options.parser_package_uuid.clear();
  errors.clear();
  const auto invalid = RenderEngineApiResultForParserPackage(MakeSuccessResult(), invalid_options);
  const bool invalid_validation_failed = !ValidateEngineRenderedResultEnvelope(invalid, &errors);
  const bool invalid_ok = !invalid.ok &&
                          !invalid.render_context_valid &&
                          invalid_validation_failed &&
                          HasRenderedDiagnosticCode(invalid, "SB_ENGINE_API_INVALID_REQUEST");

  const bool ok = success_ok && failure_ok && invalid_ok;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("success_ok", success_ok, true);
  PrintBool("failure_ok", failure_ok, true);
  PrintBool("invalid_ok", invalid_ok, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

