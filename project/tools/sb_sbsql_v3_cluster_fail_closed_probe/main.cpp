// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster/cluster_control_api.hpp"
#include "cluster/cluster_inspect_api.hpp"
#include "cluster/placement_api.hpp"
#include "cluster/replication_api.hpp"

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

EngineRequestContext Context(const Args& args, bool embedded) {
  EngineRequestContext context;
  context.trust_mode = embedded ? EngineTrustMode::embedded_in_process : EngineTrustMode::server_isolated;
  context.security_context_present = true;
  context.cluster_authority_available = false;
  context.request_id = embedded ? "sbsql-v3-cluster-fail-closed-embedded" : "sbsql-v3-cluster-fail-closed-server";
  context.database_path = args.path;
  context.database_uuid.canonical = "00000000-0000-7000-8000-000000001601";
  context.session_uuid.canonical = "00000000-0000-7000-8000-000000001602";
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

bool IsFailClosed(const EngineApiResult& result, const std::string& operation_id, bool embedded) {
  return !result.ok &&
         result.cluster_authority_required &&
         result.operation_id == operation_id &&
         HasDiagnosticCode(result, "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE") &&
         HasEvidence(result, "cluster_placeholder", "fail_closed") &&
         HasEvidence(result, "cluster_authority", "unavailable") &&
         HasEvidence(result, "cluster_command", operation_id) &&
         (!embedded || result.embedded_trust_mode_observed);
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_cluster_fail_closed_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto embedded_context = Context(args, true);
  const auto server_context = Context(args, false);

  EngineInspectClusterStateRequest state;
  state.context = embedded_context;
  const auto state_result = EngineInspectClusterState(state);

  EngineInspectClusterRoutingPlanRequest routing;
  routing.context = embedded_context;
  const auto routing_result = EngineInspectClusterRoutingPlan(routing);

  EngineInspectReplicationRequest replication;
  replication.context = embedded_context;
  const auto replication_result = EngineInspectReplication(replication);

  EnginePlaceClusterObjectRequest placement;
  placement.context = embedded_context;
  placement.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001611";
  const auto placement_result = EnginePlaceClusterObject(placement);

  EngineControlClusterRequest control;
  control.context = server_context;
  control.option_envelopes.push_back("alter_cluster:route_publication");
  const auto control_result = EngineControlCluster(control);

  const bool inspect_state_ok = IsFailClosed(state_result, "cluster.inspect_state", true);
  const bool inspect_routing_ok = IsFailClosed(routing_result, "cluster.inspect_routing_plan", true);
  const bool replication_ok = IsFailClosed(replication_result, "cluster.inspect_replication", true);
  const bool placement_ok = IsFailClosed(placement_result, "cluster.place_object", true);
  const bool control_ok = IsFailClosed(control_result, "cluster.control_cluster", false);
  const bool ok = inspect_state_ok && inspect_routing_ok && replication_ok && placement_ok && control_ok;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("inspect_state_ok", inspect_state_ok, true);
  PrintBool("inspect_routing_ok", inspect_routing_ok, true);
  PrintBool("replication_ok", replication_ok, true);
  PrintBool("placement_ok", placement_ok, true);
  PrintBool("control_ok", control_ok, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

