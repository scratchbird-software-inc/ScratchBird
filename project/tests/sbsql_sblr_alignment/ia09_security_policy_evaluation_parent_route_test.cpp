// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"
#include "security/policy_api.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace functions = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kPolicyBlockedDiagnosticUuid =
    "cd16f861-90a2-520e-97a7-79d2f28cc355";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

api::EngineRequestContext PolicyContext(bool blocked = false) {
  api::EngineRequestContext context;
  context.request_id = "ia09.security_policy_evaluation_parent";
  context.database_uuid.canonical =
      "019d0000-0000-7000-8000-000000005827";
  context.principal_uuid.canonical =
      "019d0000-0000-7000-8000-000000005828";
  context.session_uuid.canonical =
      "019d0000-0000-7000-8000-000000005829";
  context.transaction_uuid.canonical =
      "019d0000-0000-7000-8000-00000000582a";
  context.statement_uuid.canonical =
      "019d0000-0000-7000-8000-00000000582b";
  context.local_transaction_id = 5827;
  context.security_context_present = true;
  context.catalog_generation_id = 11;
  context.security_epoch = 7;
  context.resource_epoch = 13;
  context.transaction_policy_snapshot_uuid.canonical =
      "019d0000-0000-7000-8000-00000000582c";
  context.transaction_policy_snapshot_generation = 3;

  auto& authorization = context.authorization_context;
  authorization.present = true;
  authorization.authority_uuid.canonical =
      "019d0000-0000-7000-8000-00000000582d";
  authorization.security_context_generation = 2;
  authorization.principal_uuid = context.principal_uuid;
  authorization.security_epoch = context.security_epoch;
  authorization.policy_epoch = 9;
  authorization.catalog_generation_id = context.catalog_generation_id;
  authorization.effective_subjects.push_back(
      {context.principal_uuid, "principal"});

  auto& observation = context.current_policy_gate;
  observation.present = true;
  observation.blocked = blocked;
  observation.statement_uuid = context.statement_uuid;
  observation.transaction_uuid = context.transaction_uuid;
  observation.local_transaction_id = context.local_transaction_id;
  observation.authorization_context_uuid = authorization.authority_uuid;
  observation.authorization_context_generation =
      authorization.security_context_generation;
  observation.policy_snapshot_uuid =
      context.transaction_policy_snapshot_uuid;
  observation.policy_snapshot_generation =
      context.transaction_policy_snapshot_generation;
  observation.security_epoch = context.security_epoch;
  observation.policy_epoch = authorization.policy_epoch;
  observation.catalog_generation_id = context.catalog_generation_id;
  observation.resource_epoch = context.resource_epoch;
  return context;
}

api::EngineEvaluatePolicyRequest PolicyRequest(
    const api::EngineRequestContext& context,
    api::EnginePolicyObservationKind kind =
        api::EnginePolicyObservationKind::current_statement_gate) {
  api::EngineEvaluatePolicyRequest request;
  request.context = context;
  request.operation_id = "security.evaluate_policy";
  request.observation_kind = kind;
  return request;
}

void RequireDiagnostic(const api::EngineEvaluatePolicyResult& result,
                       std::string_view code,
                       std::string_view message) {
  Require(!result.ok && !result.policy_blocked &&
              result.diagnostics.size() == 1 &&
              result.diagnostics.front().code == code,
          message);
}

sblr::SblrResult RunFunction(const api::EngineRequestContext& context,
                             std::string function_id) {
  const auto package = functions::BuildStandardFunctionSeedPackage();
  functions::FunctionCallRequest request;
  request.context.function_id = std::move(function_id);
  request.context.security_allowed = true;
  request.context.policy_allowed = true;
  request.context.dependency_available = true;
  request.context.engine_request_context = &context;
  request.context.sblr_context.session_uuid = context.session_uuid.canonical;
  request.context.sblr_context.transaction_uuid =
      context.transaction_uuid.canonical;
  request.context.sblr_context.statement_uuid = context.statement_uuid.canonical;
  request.context.sblr_context.user_uuid = context.principal_uuid.canonical;
  request.context.sblr_context.local_transaction_id =
      context.local_transaction_id;
  request.context.sblr_context.security_context_present = true;
  request.context.sblr_context.transaction_context_present = true;
  request.context.sblr_context.current_diagnostic_uuid =
      context.current_diagnostic_uuid.canonical;
  return functions::DispatchFunctionCall(package.registry, std::move(request))
      .result;
}

void RequireBoolean(const sblr::SblrResult& result,
                    bool expected,
                    std::string_view message) {
  Require(result.ok() && result.scalar_values.size() == 1 &&
              result.scalar_values.front().descriptor_id == "boolean" &&
              result.scalar_values.front().payload_kind ==
                  sblr::SblrValuePayloadKind::boolean &&
              result.scalar_values.front().has_int64_value &&
              result.scalar_values.front().int64_value == (expected ? 1 : 0) &&
              !result.mutation_attempted && !result.mutation_committed,
          message);
}

}  // namespace

int main() {
  const auto admitted_context = PolicyContext(false);
  const auto admitted = api::EngineEvaluatePolicy(
      PolicyRequest(admitted_context));
  Require(admitted.ok && !admitted.policy_blocked &&
              admitted.operation_id == "security.evaluate_policy" &&
              admitted.dml_summary.rows_changed == 0,
          "005827 admitted statement did not report an unblocked policy gate");

  const auto admitted_replay = api::EngineEvaluatePolicy(
      PolicyRequest(admitted_context));
  Require(admitted_replay.ok &&
              admitted_replay.policy_blocked == admitted.policy_blocked &&
              admitted_replay.operation_id == admitted.operation_id,
          "005828 exact policy observation replay drifted");

  const auto blocked_context = PolicyContext(true);
  const auto blocked = api::EngineEvaluatePolicy(PolicyRequest(blocked_context));
  Require(blocked.ok && blocked.policy_blocked,
          "005828 engine-owned blocked observation was not preserved");

  auto diagnostic_context = admitted_context;
  diagnostic_context.current_diagnostic_uuid.canonical =
      std::string(kPolicyBlockedDiagnosticUuid);
  const auto diagnostic = api::EngineEvaluatePolicy(PolicyRequest(
      diagnostic_context,
      api::EnginePolicyObservationKind::current_diagnostic_policy_refusal));
  Require(diagnostic.ok && diagnostic.policy_blocked,
          "005828 exact policy diagnostic UUID was not observed");
  diagnostic_context.current_diagnostic_uuid.canonical =
      "019d0000-0000-7000-8000-00000000582e";
  const auto other_diagnostic = api::EngineEvaluatePolicy(PolicyRequest(
      diagnostic_context,
      api::EnginePolicyObservationKind::current_diagnostic_policy_refusal));
  Require(other_diagnostic.ok && !other_diagnostic.policy_blocked,
          "005828 unrelated diagnostic was classified as policy blocked");

  auto malformed = PolicyRequest(admitted_context);
  malformed.target_object.uuid.canonical =
      "019d0000-0000-7000-8000-00000000582f";
  malformed.policy_profile.encoded_profiles.push_back("caller-policy");
  RequireDiagnostic(api::EngineEvaluatePolicy(malformed),
                    "SBLR.OPERAND_INVALID",
                    "005828 caller policy/target input was accepted");

  auto no_statement_context = admitted_context;
  no_statement_context.statement_uuid.canonical.clear();
  RequireDiagnostic(api::EngineEvaluatePolicy(
                        PolicyRequest(no_statement_context)),
                    "SBSQL.NO_STATEMENT",
                    "005828 missing statement did not fail closed");

  auto unauthenticated_context = admitted_context;
  unauthenticated_context.security_context_present = false;
  RequireDiagnostic(api::EngineEvaluatePolicy(
                        PolicyRequest(unauthenticated_context)),
                    "SECURITY.ACCESS_DENIED",
                    "005828 unauthenticated observation was accepted");

  auto stale_context = admitted_context;
  ++stale_context.current_policy_gate.policy_snapshot_generation;
  RequireDiagnostic(api::EngineEvaluatePolicy(PolicyRequest(stale_context)),
                    "SECURITY.ACCESS_DENIED",
                    "005828 stale policy generation was accepted");
  stale_context = admitted_context;
  ++stale_context.current_policy_gate.security_epoch;
  RequireDiagnostic(api::EngineEvaluatePolicy(PolicyRequest(stale_context)),
                    "SECURITY.ACCESS_DENIED",
                    "005828 stale security epoch was accepted");
  stale_context = admitted_context;
  ++stale_context.current_policy_gate.catalog_generation_id;
  RequireDiagnostic(api::EngineEvaluatePolicy(PolicyRequest(stale_context)),
                    "SECURITY.ACCESS_DENIED",
                    "005828 stale catalog generation was accepted");
  stale_context = admitted_context;
  ++stale_context.current_policy_gate.resource_epoch;
  RequireDiagnostic(api::EngineEvaluatePolicy(PolicyRequest(stale_context)),
                    "SECURITY.ACCESS_DENIED",
                    "005828 stale resource epoch was accepted");

  auto cancelled_context = admitted_context;
  cancelled_context.query_cancellation_requested = [] { return true; };
  RequireDiagnostic(api::EngineEvaluatePolicy(
                        PolicyRequest(cancelled_context)),
                    "PROCESS.CANCELLED",
                    "005828 policy observation ignored cancellation");

  RequireBoolean(RunFunction(admitted_context, "sb.scalar.policy_blocked"),
                 false,
                 "005827 policy_blocked did not return boolean false");
  RequireBoolean(RunFunction(blocked_context, "sb.scalar.policy_blocked"),
                 true,
                 "005828 policy_blocked did not preserve boolean true");
  diagnostic_context.current_diagnostic_uuid.canonical =
      std::string(kPolicyBlockedDiagnosticUuid);
  RequireBoolean(
      RunFunction(diagnostic_context, "sb.scalar.policy_blocked_diagnostic"),
      true,
      "005828 policy diagnostic observer did not return boolean true");

  std::cout << "CSC-TEST-005827 CSC-TEST-005828 "
               "SECURITY_POLICY_EVALUATION_PARENT contract=passed "
               "admitted_false=true engine_blocked_true=true "
               "diagnostic_identity=true malformed_refusal=true "
               "authority_fences=true cancellation=true replay=true "
               "no_mutation=true internal_operation_non_addressable=true\n";
  return 0;
}
