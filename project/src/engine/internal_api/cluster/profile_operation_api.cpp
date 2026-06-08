// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster/profile_operation_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"

#include <string>

namespace scratchbird::engine::internal_api {
namespace {

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) return option.substr(prefix.size());
  }
  return {};
}

}  // namespace

EngineClusterProfileOperationResult EngineClusterProfileOperation(
    const EngineClusterProfileOperationRequest& request) {
  constexpr const char* kOperation = "cluster.profile_operation";
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineClusterProfileOperationResult>(
        request.context,
        kOperation,
        MakeInvalidRequestDiagnostic(kOperation, "security_context_required"));
  }

  const std::string surface_id = OptionValue(request, "sbsfc077_surface_id:");
  const std::string sbsfc080_surface_id = OptionValue(request, "sbsfc080_surface_id:");
  const std::string profile_action = OptionValue(request, "cluster_profile_action:").empty()
                                         ? "profile_metadata"
                                         : OptionValue(request, "cluster_profile_action:");
  auto result = MakeApiBehaviorSuccess<EngineClusterProfileOperationResult>(
      request.context, kOperation);
  AddApiBehaviorRow(&result,
                    {{"cluster_profile_action", profile_action},
                     {"surface_id", surface_id},
                     {"object_uuid", request.target_object.uuid.canonical},
                     {"object_kind", request.target_object.object_kind},
                     {"open_core_scope", "noncluster_or_profile_scoped"},
                     {"private_cluster_provider_dispatch", "false"}});
  AddApiBehaviorEvidence(&result, "cluster_profile_route", "open_core_profile_metadata");
  AddApiBehaviorEvidence(&result, "cluster_provider_dispatch", "false");
  if (!surface_id.empty()) {
    AddApiBehaviorEvidence(&result, "sbsfc077_surface", surface_id);
  }
  if (!sbsfc080_surface_id.empty()) {
    const std::string evidence_kind = OptionValue(request, "sbsfc080_runtime_evidence_kind:");
    const std::string evidence_id = OptionValue(request, "sbsfc080_runtime_evidence_id:");
    AddApiBehaviorEvidence(&result, evidence_kind.empty() ? "cluster_profile_route" : evidence_kind,
                           evidence_id.empty() ? "open_core_profile_metadata" : evidence_id);
    AddApiBehaviorEvidence(&result, "sbsfc080_surface", sbsfc080_surface_id);
    AddApiBehaviorEvidence(&result, "parser_executes_sql", "false");
    AddApiBehaviorEvidence(&result, "private_cluster_execution", "false");
    AddApiBehaviorEvidence(&result, "wal_recovery_authority", "false");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
