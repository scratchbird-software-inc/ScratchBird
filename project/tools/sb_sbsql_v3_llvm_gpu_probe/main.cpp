// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/gpu_api.hpp"
#include "extensibility/llvm_api.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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

EngineRequestContext Context(const Args& args, bool secure) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = secure;
  context.request_id = secure ? "sbsql-v3-llvm-gpu-probe-secure" : "sbsql-v3-llvm-gpu-probe-open";
  context.database_path = args.path;
  context.database_uuid.canonical = "00000000-0000-7000-8000-000000001401";
  context.session_uuid.canonical = "00000000-0000-7000-8000-000000001402";
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

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_llvm_gpu_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  EngineCompileLlvmModuleRequest open_llvm;
  open_llvm.context = Context(args, false);
  open_llvm.option_envelopes.push_back("compile:jit");
  const auto open_llvm_result = EngineCompileLlvmModule(open_llvm);

  EngineCompileLlvmModuleRequest secure_llvm;
  secure_llvm.context = Context(args, true);
  secure_llvm.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001411";
  secure_llvm.option_envelopes.push_back("compile:jit");
  secure_llvm.option_envelopes.push_back("module:sblr_lowered_ir");
  const auto secure_llvm_result = EngineCompileLlvmModule(secure_llvm);

  EngineInspectGpuCapabilityRequest open_gpu;
  open_gpu.context = Context(args, false);
  const auto open_gpu_result = EngineInspectGpuCapability(open_gpu);

  EngineInspectGpuCapabilityRequest controlled_gpu;
  controlled_gpu.context = Context(args, false);
  controlled_gpu.option_envelopes.push_back("enable_gpu_execution:true");
  const auto controlled_gpu_result = EngineInspectGpuCapability(controlled_gpu);

  EngineInspectGpuCapabilityRequest cluster_gpu;
  cluster_gpu.context = Context(args, true);
  cluster_gpu.option_envelopes.push_back("cluster_gpu_dispatch:true");
  const auto cluster_gpu_result = EngineInspectGpuCapability(cluster_gpu);

  const bool llvm_security_denied = !open_llvm_result.ok &&
                                    HasDiagnosticCode(open_llvm_result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED");
  const bool llvm_ok = secure_llvm_result.ok &&
                       HasEvidence(secure_llvm_result, "llvm_compile_contract", "validated_request_shape") &&
                       HasEvidence(secure_llvm_result, "extension_behavior", "jit_compiled");
  const bool gpu_inspect_ok = open_gpu_result.ok &&
                              HasEvidence(open_gpu_result, "gpu_capability", "inspected") &&
                              HasEvidence(open_gpu_result, "extension_behavior", "inspect_only_no_implicit_execution");
  const bool gpu_control_denied = !controlled_gpu_result.ok &&
                                  HasDiagnosticCode(controlled_gpu_result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED");
  const bool cluster_denied = !cluster_gpu_result.ok && cluster_gpu_result.cluster_authority_required;
  const bool ok = llvm_security_denied && llvm_ok && gpu_inspect_ok && gpu_control_denied && cluster_denied;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("llvm_security_denied", llvm_security_denied, true);
  PrintBool("llvm_ok", llvm_ok, true);
  PrintBool("gpu_inspect_ok", gpu_inspect_ok, true);
  PrintBool("gpu_control_denied", gpu_control_denied, true);
  PrintBool("cluster_denied", cluster_denied, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
