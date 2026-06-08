// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "management/config_api.hpp"
#include "management/management_api.hpp"
#include "management/support_bundle_api.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args { std::string path; bool overwrite = false; };

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") { args->overwrite = true; continue; }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") { args->path = value; } else { return false; }
  }
  return !args->path.empty();
}

EngineRequestContext Context(const Args& args, bool secure) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = secure;
  context.request_id = secure ? "sbsql-v3-management-probe-secure" : "sbsql-v3-management-probe-open";
  context.database_path = args.path;
  return context;
}

bool HasDiagnosticCode(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind, const std::string& id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
  }
  return false;
}

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool AnyPayload(const EngineApiResult& result, const std::string& value) {
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, "payload") == value) { return true; }
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
    std::cerr << "usage: sb_sbsql_v3_management_observability_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto open_context = Context(args, false);
  EngineSetConfigRequest denied_set;
  denied_set.context = open_context;
  const auto denied_set_result = EngineSetConfig(denied_set);
  const bool config_control_requires_security = !denied_set_result.ok &&
                                                HasDiagnosticCode(denied_set_result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED");

  EngineControlManagementRuntimeRequest denied_control;
  denied_control.context = open_context;
  const auto denied_control_result = EngineControlManagementRuntime(denied_control);
  const bool management_control_requires_security = !denied_control_result.ok &&
                                                    HasDiagnosticCode(denied_control_result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED");

  const auto secure_context = Context(args, true);
  EngineSetConfigRequest set_config;
  set_config.context = secure_context;
  set_config.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001011";
  set_config.target_object.object_kind = "config";
  set_config.localized_names.push_back({"en", "default", "network.secret", "network.secret", true});
  set_config.option_envelopes.push_back("password:supersecret");
  const auto set_config_result = EngineSetConfig(set_config);

  EngineInspectConfigRequest inspect_config;
  inspect_config.context = open_context;
  const auto inspect_result = EngineInspectConfig(inspect_config);
  const bool config_redacted = inspect_result.ok && AnyPayload(inspect_result, "<redacted:sensitive_config>") &&
                               HasEvidence(inspect_result, "config_redaction", "key_based");

  EngineInspectManagementRuntimeRequest inspect_runtime;
  inspect_runtime.context = open_context;
  const auto inspect_runtime_result = EngineInspectManagementRuntime(inspect_runtime);

  EngineControlManagementRuntimeRequest cluster_control;
  cluster_control.context = secure_context;
  cluster_control.option_envelopes.push_back("cluster:rebalance");
  const auto cluster_control_result = EngineControlManagementRuntime(cluster_control);
  const bool cluster_control_fail_closed = !cluster_control_result.ok &&
                                           HasDiagnosticCode(cluster_control_result, "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE");

  EngineControlManagementRuntimeRequest local_control;
  local_control.context = secure_context;
  local_control.option_envelopes.push_back("listener:drain");
  const auto local_control_result = EngineControlManagementRuntime(local_control);

  EnginePrepareSupportBundleRequest bundle;
  bundle.context = open_context;
  const auto bundle_result = EnginePrepareSupportBundle(bundle);
  const bool bundle_redaction = bundle_result.ok && HasEvidence(bundle_result, "support_bundle_manifest", "local_node_redacted");

  const bool ok = config_control_requires_security && management_control_requires_security && set_config_result.ok &&
                  config_redacted && inspect_runtime_result.ok && cluster_control_fail_closed &&
                  local_control_result.ok && bundle_redaction;
  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("config_control_requires_security", config_control_requires_security, true);
  PrintBool("config_redacted", config_redacted, true);
  PrintBool("cluster_control_fail_closed", cluster_control_fail_closed, true);
  PrintBool("bundle_redaction", bundle_redaction, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

