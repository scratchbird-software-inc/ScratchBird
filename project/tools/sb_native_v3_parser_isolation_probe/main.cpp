// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "native_v3_parser_package.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::parser::native_v3_package;

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

NativeV3ParserPackageRequest BaseRequest(const Args& args, std::string command) {
  NativeV3ParserPackageRequest request;
  request.command_text = std::move(command);
  request.database_path = args.path;
  request.database_uuid = "00000000-0000-7000-8000-000000001801";
  request.session_uuid = "00000000-0000-7000-8000-000000001802";
  request.principal_uuid = "00000000-0000-7000-8000-000000001803";
  request.parser_package_uuid = "00000000-0000-7000-8000-000000001804";
  request.parser_package_version = "native-v3-stage15";
  request.registry_snapshot_uuid = "00000000-0000-7000-8000-000000001805";
  request.catalog_epoch = "stage15-catalog-epoch";
  request.client_dialect = "sbsql_v3";
  request.language_tag = "en";
  request.security_context_present = true;
  request.cluster_authority_available = false;
  return request;
}

bool HasEvidence(const scratchbird::engine::internal_api::EngineRenderedResultEnvelope& envelope,
                 const std::string& kind,
                 const std::string& id = {}) {
  for (const auto& evidence : envelope.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
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
    std::cerr << "usage: sb_native_v3_parser_isolation_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto show_version = ExecuteNativeV3ParserPackageRequest(BaseRequest(args, "SHOW VERSION;"));
  const auto show_database = ExecuteNativeV3ParserPackageRequest(BaseRequest(args, " SHOW DATABASE "));

  auto bad_parser_context = BaseRequest(args, "SHOW VERSION");
  bad_parser_context.parser_package_uuid.clear();
  const auto missing_parser_uuid = ExecuteNativeV3ParserPackageRequest(bad_parser_context);

  const auto unsupported_command = ExecuteNativeV3ParserPackageRequest(BaseRequest(args, "DROP DATABASE anything"));

  const bool show_version_ok = show_version.ok &&
                               !show_version.parser_is_trusted &&
                               !show_version.sblr_contains_sql_text &&
                               show_version.sblr_parser_resolved_names_to_uuids &&
                               show_version.sblr_envelope_validated &&
                               show_version.dispatched_to_engine_api &&
                               show_version.rendered_for_parser_package &&
                               show_version.rendered_result.parser_package_rendering_required &&
                               show_version.rendered_result.canonical_diagnostics &&
                               show_version.rendered_result.canonical_result_shape &&
                               show_version.rendered_result.operation_id == "observability.show_version" &&
                               HasEvidence(show_version.rendered_result, "observability", "observability.show_version");

  const bool show_database_ok = show_database.ok &&
                                show_database.rendered_result.operation_id == "observability.show_database" &&
                                show_database.rendered_result.rows.size() == 1 &&
                                show_database.rendered_result.parser_package_uuid == "00000000-0000-7000-8000-000000001804" &&
                                show_database.rendered_result.parser_package_version == "native-v3-stage15";

  const bool missing_context_denied = !missing_parser_uuid.ok &&
                                      missing_parser_uuid.failed_stage == "parser_package_context";
  const bool unsupported_parse_denied = !unsupported_command.ok &&
                                        unsupported_command.failed_stage == "parse" &&
                                        !unsupported_command.dispatched_to_engine_api;

  const bool ok = show_version_ok && show_database_ok && missing_context_denied && unsupported_parse_denied;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("show_version_ok", show_version_ok, true);
  PrintBool("show_database_ok", show_database_ok, true);
  PrintBool("missing_context_denied", missing_context_denied, true);
  PrintBool("unsupported_parse_denied", unsupported_parse_denied, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

