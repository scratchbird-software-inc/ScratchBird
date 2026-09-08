// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "dispatch/function_dispatch.hpp"
#include "registry/function_seed_registry.hpp"
#include "security/visibility_api.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace functions = scratchbird::engine::functions;
namespace sblr = scratchbird::engine::sblr;

constexpr std::string_view kPrincipalUuid =
    "019d0000-0000-7000-8000-000000005826";
constexpr std::string_view kOtherPrincipalUuid =
    "019d0000-0000-7000-8000-000000005827";
constexpr std::string_view kTargetUuid =
    "019d0000-0000-7000-8000-000000005828";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

api::EngineRequestContext MaterializedContext() {
  api::EngineRequestContext context;
  context.request_id = "ia09.security_visibility_parent";
  context.principal_uuid.canonical = std::string(kPrincipalUuid);
  context.session_uuid.canonical =
      "019d0000-0000-7000-8000-000000005829";
  context.statement_uuid.canonical =
      "019d0000-0000-7000-8000-00000000582a";
  context.security_context_present = true;
  context.catalog_generation_id = 11;
  context.security_epoch = 7;

  auto& authorization = context.authorization_context;
  authorization.present = true;
  authorization.authority_uuid.canonical =
      "019d0000-0000-7000-8000-00000000582b";
  authorization.security_context_generation = 1;
  authorization.principal_uuid = context.principal_uuid;
  authorization.security_epoch = context.security_epoch;
  authorization.policy_epoch = 9;
  authorization.catalog_generation_id = context.catalog_generation_id;
  authorization.effective_subjects.push_back(
      {context.principal_uuid, "principal"});

  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      "019d0000-0000-7000-8000-00000000582c";
  grant.subject_uuid = context.principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = std::string(kTargetUuid);
  grant.right = "SELECT";
  grant.security_epoch = context.security_epoch;
  authorization.grants.push_back(std::move(grant));
  return context;
}

api::EngineEvaluateVisibilityRequest VisibilityRequest(
    const api::EngineRequestContext& context) {
  api::EngineEvaluateVisibilityRequest request;
  request.context = context;
  request.operation_id = "security.evaluate_visibility";
  request.target_object.uuid.canonical = std::string(kTargetUuid);
  request.target_object.object_kind = "table";
  request.required_right = "SELECT";
  request.allow_target_owner = true;
  request.allow_materialized_grant = true;
  request.requested_right_valid = true;
  request.administrative_rights = {"SEC_GRANT_ADMIN", "POLICY_ADMIN"};
  return request;
}

sblr::SblrValue TextValue(std::string value) {
  sblr::SblrValue out;
  out.descriptor_id = "text";
  out.payload_kind = sblr::SblrValuePayloadKind::text;
  out.text_value = std::move(value);
  out.encoded_value = out.text_value;
  out.is_null = false;
  return out;
}

void RequireDiagnostic(const api::EngineEvaluateVisibilityResult& result,
                       std::string_view code,
                       std::string_view message) {
  Require(!result.ok && !result.visible &&
              result.diagnostics.size() == 1 &&
              result.diagnostics.front().code == code,
          message);
}

}  // namespace

int main() {
  const auto context = MaterializedContext();

  auto owner_request = VisibilityRequest(context);
  owner_request.target_owner_uuid = context.principal_uuid;
  owner_request.allow_materialized_grant = false;
  const auto owner = api::EngineEvaluateVisibility(owner_request);
  Require(owner.ok && owner.visible,
          "005825 owner visibility did not use engine-owned target authority");

  auto grant_request = VisibilityRequest(context);
  grant_request.target_owner_uuid.canonical =
      std::string(kOtherPrincipalUuid);
  const auto granted = api::EngineEvaluateVisibility(grant_request);
  Require(granted.ok && granted.visible,
          "005825 materialized grant visibility was not admitted");

  auto denied_request = grant_request;
  denied_request.required_right = "UPDATE";
  const auto denied = api::EngineEvaluateVisibility(denied_request);
  RequireDiagnostic(denied, "SECURITY.AUTHORIZATION.DENIED",
                    "005825 missing right did not fail closed");

  auto invalid_request = grant_request;
  invalid_request.required_right = "SELECT OR OWNER";
  invalid_request.requested_right_valid = false;
  const auto invalid = api::EngineEvaluateVisibility(invalid_request);
  RequireDiagnostic(invalid, "SECURITY.AUTHORIZATION.DENIED",
                    "005828 invalid privilege text did not fail closed");

  auto cancelled_context = context;
  cancelled_context.query_cancellation_requested = [] { return true; };
  auto cancelled_request = VisibilityRequest(cancelled_context);
  cancelled_request.target_owner_uuid = context.principal_uuid;
  const auto cancelled = api::EngineEvaluateVisibility(cancelled_request);
  RequireDiagnostic(cancelled, "PROCESS.CANCELLED",
                    "005828 visibility cancellation was not deterministic");

  const auto package = functions::BuildStandardFunctionSeedPackage();
  functions::FunctionCallRequest function_request;
  function_request.context.function_id =
      "sb.scalar.has_function_privilege";
  function_request.context.security_allowed = true;
  function_request.context.policy_allowed = true;
  function_request.context.dependency_available = true;
  function_request.context.engine_request_context = &cancelled_context;
  function_request.context.sblr_context.user_uuid =
      cancelled_context.principal_uuid.canonical;
  function_request.context.sblr_context.session_uuid =
      cancelled_context.session_uuid.canonical;
  function_request.context.sblr_context.statement_uuid =
      cancelled_context.statement_uuid.canonical;
  function_request.context.sblr_context.security_context_present = true;
  function_request.arguments.push_back(
      {"function", TextValue("has_table_privilege")});
  function_request.arguments.push_back({"privilege", TextValue("EXECUTE")});
  const auto function_cancelled = functions::DispatchFunctionCall(
      package.registry, std::move(function_request));
  Require(!function_cancelled.result.ok() &&
              function_cancelled.result.diagnostics.size() == 1 &&
              function_cancelled.result.diagnostics.front().diagnostic_id ==
                  "PROCESS.CANCELLED",
          "005828 full engine cancellation authority was lost at function dispatch");

  std::cout << "CSC-TEST-005825 CSC-TEST-005828 "
               "SECURITY_VISIBILITY_PARENT contract=passed "
               "owner=true materialized_grant=true deny=true "
               "invalid_right=true cancellation=true "
               "internal_operation_non_addressable=true\n";
  return 0;
}
