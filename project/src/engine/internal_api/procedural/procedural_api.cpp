// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "procedural/procedural_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

std::string OptionValue(const EngineApiRequest& request, std::string_view prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) return option.substr(prefix.size());
  }
  return {};
}

void AddCommonEvidence(EngineApiResult* result,
                       const EngineApiRequest& request,
                       std::string_view route_kind) {
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "parser_finality", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "source_sql_payload", "absent");
  AddApiBehaviorEvidence(result, "procedural_ir_route", std::string(route_kind));
  if (const auto surface_id = OptionValue(request, "sbsfc078_surface_id:");
      !surface_id.empty()) {
    AddApiBehaviorEvidence(result, "sbsfc078_surface", surface_id);
  }
}

template <typename TResult>
TResult MakeProceduralError(const EngineApiRequest& request,
                            const std::string& operation_id,
                            EngineApiDiagnostic diagnostic) {
  TResult result;
  result.ok = false;
  result.operation_id = operation_id;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == EngineTrustMode::embedded_in_process;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

template <typename TResult>
TResult MakeProceduralOk(const EngineApiRequest& request,
                         const std::string& operation_id,
                         std::string_view evidence_kind,
                         std::string_view evidence_id) {
  TResult result;
  result.ok = true;
  result.operation_id = operation_id;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == EngineTrustMode::embedded_in_process;
  result.evidence.push_back({std::string(evidence_kind), std::string(evidence_id)});
  return result;
}

std::string RouteKind(const EngineApiRequest& request) {
  if (const auto value = OptionValue(request, "procedural_route_kind:");
      !value.empty()) {
    return value;
  }
  if (const auto value = OptionValue(request, "diagnostic_route_kind:");
      !value.empty()) {
    return value;
  }
  return "procedural_general";
}

}  // namespace

EngineGeneralProceduralOperationResult EngineGeneralProceduralOperation(
    const EngineGeneralProceduralOperationRequest& request) {
  const std::string operation_id = "general.procedural_operation";
  if (!request.context.security_context_present) {
    return MakeProceduralError<EngineGeneralProceduralOperationResult>(
        request,
        operation_id,
        MakeSecurityContextRequiredDiagnostic(operation_id));
  }
  if (request.context.local_transaction_id == 0 &&
      request.context.transaction_uuid.canonical.empty()) {
    return MakeProceduralError<EngineGeneralProceduralOperationResult>(
        request,
        operation_id,
        MakeInvalidRequestDiagnostic(operation_id, "transaction_context_required"));
  }

  const auto route_kind = RouteKind(request);
  auto result = MakeProceduralOk<EngineGeneralProceduralOperationResult>(
      request, operation_id, "procedural_general_route", route_kind);
  AddCommonEvidence(&result, request, route_kind);
  AddApiBehaviorEvidence(&result, "engine_transaction_context", "present");
  AddApiBehaviorEvidence(&result, "procedural_compile_validate", "accepted");
  AddApiBehaviorEvidence(&result, "runtime_authority", "engine_internal_api");
  return result;
}

EngineSignalDiagnosticResult EngineSignalDiagnostic(
    const EngineSignalDiagnosticRequest& request) {
  const std::string operation_id = "general.signal_diagnostic";
  if (!request.context.security_context_present) {
    return MakeProceduralError<EngineSignalDiagnosticResult>(
        request,
        operation_id,
        MakeSecurityContextRequiredDiagnostic(operation_id));
  }
  const auto route_kind = RouteKind(request);
  auto result = MakeProceduralOk<EngineSignalDiagnosticResult>(
      request, operation_id, "diagnostic_signal", route_kind);
  AddCommonEvidence(&result, request, route_kind);
  AddApiBehaviorEvidence(&result, "diagnostic_emit", "signal");
  return result;
}

EngineRaiseDiagnosticResult EngineRaiseDiagnostic(
    const EngineRaiseDiagnosticRequest& request) {
  const std::string operation_id = "general.raise_diagnostic";
  if (!request.context.security_context_present) {
    return MakeProceduralError<EngineRaiseDiagnosticResult>(
        request,
        operation_id,
        MakeSecurityContextRequiredDiagnostic(operation_id));
  }
  const auto route_kind = RouteKind(request);
  auto result = MakeProceduralOk<EngineRaiseDiagnosticResult>(
      request, operation_id, "diagnostic_raise", route_kind);
  AddCommonEvidence(&result, request, route_kind);
  AddApiBehaviorEvidence(&result, "diagnostic_emit", "raise");
  return result;
}

EngineResignalDiagnosticResult EngineResignalDiagnostic(
    const EngineResignalDiagnosticRequest& request) {
  const std::string operation_id = "general.resignal_diagnostic";
  if (!request.context.security_context_present) {
    return MakeProceduralError<EngineResignalDiagnosticResult>(
        request,
        operation_id,
        MakeSecurityContextRequiredDiagnostic(operation_id));
  }
  const auto route_kind = RouteKind(request);
  auto result = MakeProceduralOk<EngineResignalDiagnosticResult>(
      request, operation_id, "diagnostic_resignal", route_kind);
  AddCommonEvidence(&result, request, route_kind);
  AddApiBehaviorEvidence(&result, "diagnostic_emit", "resignal");
  return result;
}

}  // namespace scratchbird::engine::internal_api
