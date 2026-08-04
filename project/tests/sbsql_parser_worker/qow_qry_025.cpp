// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/expression_api.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace api = scratchbird::engine::internal_api;

namespace scratchbird::engine::internal_api {
bool QowBindCanonicalExpressionReferenceV1(
    const EngineBindExpressionRequest& request,
    EngineObjectReference* bound_reference,
    EngineDescriptor* bound_descriptor,
    std::string* refusal_reason,
    std::string* refusal_detail);
}  // namespace scratchbird::engine::internal_api

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

api::EngineBindExpressionRequest CanonicalRequest() {
  api::EngineBindExpressionRequest request;
  request.operation_id = "query.bind_expression";
  request.context.principal_uuid.canonical =
      "019f0000-0000-7200-8000-000000002501";
  request.context.statement_uuid.canonical =
      "019f0000-0000-7200-8000-000000002502";
  request.context.transaction_uuid.canonical =
      "019f0000-0000-7200-8000-000000002503";
  request.context.statement_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000002504";
  request.context.statement_metadata_snapshot_engine_owned = true;
  request.context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000002505";
  request.context.catalog_generation_id = 25;
  request.context.security_epoch = 26;
  request.context.resource_epoch = 27;
  request.context.security_context_present = true;

  auto& authorization = request.context.authorization_context;
  authorization.present = true;
  authorization.authority_uuid.canonical =
      "019f0000-0000-7200-8000-000000002506";
  authorization.principal_uuid = request.context.principal_uuid;
  authorization.security_epoch = request.context.security_epoch;
  authorization.policy_epoch = 28;
  authorization.catalog_generation_id =
      request.context.catalog_generation_id;
  authorization.effective_subjects.push_back(
      {request.context.principal_uuid, "principal"});

  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      "019f0000-0000-7200-8000-000000002507";
  grant.subject_uuid = request.context.principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical =
      "019f0000-0000-7200-8000-000000002508";
  grant.right = "SELECT";
  grant.security_epoch = request.context.security_epoch;
  authorization.grants.push_back(grant);

  request.sql_object_reference.expected_object_type = "table";
  request.sql_object_reference.path_type = "unqualified";
  request.sql_object_reference.object_name.raw_text = "orders";
  request.sql_object_reference.object_name.normalized_lookup_key = "orders";
  request.sql_object_reference.object_name.source_span = "7:13";

  request.bound_object_identity.object_uuid = grant.target_uuid;
  request.bound_object_identity.resolved_object_type = "table";
  request.bound_object_identity.resolved_schema_uuid.canonical =
      "019f0000-0000-7200-8000-000000002509";
  request.bound_object_identity.catalog_generation_id =
      request.context.catalog_generation_id;
  request.bound_object_identity.security_epoch = request.context.security_epoch;
  request.bound_object_identity.resource_epoch = request.context.resource_epoch;

  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000002510";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7200-8000-000000002511;"
      "nullability=non_null;width=64";
  request.descriptors.push_back(std::move(descriptor));
  return request;
}

bool Bind(const api::EngineBindExpressionRequest& request,
          api::EngineObjectReference* reference,
          api::EngineDescriptor* descriptor,
          std::string* reason,
          std::string* detail) {
  return api::QowBindCanonicalExpressionReferenceV1(
      request, reference, descriptor, reason, detail);
}

bool AtomicRefusal(const api::EngineBindExpressionRequest& request,
                   const std::string_view expected_reason) {
  api::EngineObjectReference reference;
  api::EngineDescriptor descriptor;
  std::string reason;
  std::string detail;
  const bool ok = Bind(request, &reference, &descriptor, &reason, &detail);
  return !ok && reason == expected_reason && !detail.empty() &&
         reference.uuid.canonical.empty() && reference.object_kind.empty() &&
         descriptor.descriptor_uuid.canonical.empty() &&
         descriptor.canonical_type_name.empty();
}

bool ValidatePositiveBinding() {
  const auto request = CanonicalRequest();
  api::EngineObjectReference reference;
  api::EngineDescriptor descriptor;
  std::string reason;
  std::string detail;
  const bool ok = Bind(request, &reference, &descriptor, &reason, &detail);
  return Require(ok, "canonical expression reference was refused") &&
         Require(reason.empty() && detail.empty(),
                 "successful canonical binding emitted refusal state") &&
         Require(reference.uuid.canonical ==
                     request.bound_object_identity.object_uuid.canonical &&
                     reference.object_kind == "table",
                 "bound expression did not preserve object UUID identity") &&
         Require(descriptor.descriptor_uuid.canonical ==
                     request.descriptors.front().descriptor_uuid.canonical &&
                     descriptor.canonical_type_name == "int64",
                 "bound expression did not preserve descriptor identity");
}

bool ValidateRefusals() {
  bool passed = true;
  {
    auto request = CanonicalRequest();
    request.sql_object_reference.object_name.raw_text.clear();
    passed &= Require(
        AtomicRefusal(request, "malformed_reference"),
        "malformed reference did not refuse atomically");
  }
  {
    auto request = CanonicalRequest();
    request.related_objects.push_back(
        {{"019f0000-0000-7200-8000-000000002512"}, "table"});
    passed &= Require(
        AtomicRefusal(request, "ambiguous_reference"),
        "ambiguous reference did not refuse atomically");
  }
  {
    auto request = CanonicalRequest();
    request.bound_object_identity.object_uuid.canonical.clear();
    passed &= Require(
        AtomicRefusal(request, "unresolved_reference"),
        "unresolved reference did not refuse atomically");
  }
  {
    auto request = CanonicalRequest();
    ++request.bound_object_identity.catalog_generation_id;
    passed &= Require(
        AtomicRefusal(request, "stale_reference"),
        "stale catalog binding did not refuse atomically");
  }
  {
    auto request = CanonicalRequest();
    request.descriptors.front().canonical_type_name = "unknown_type";
    passed &= Require(
        AtomicRefusal(request, "ill_typed_reference"),
        "ill-typed descriptor did not refuse atomically");
  }
  {
    auto request = CanonicalRequest();
    request.context.authorization_context.grants.clear();
    passed &= Require(
        AtomicRefusal(request, "unauthorized_reference"),
        "unauthorized reference did not refuse atomically");
  }
  return passed;
}

}  // namespace

// QOW-TEST-QRY-025-V1
int main() {
  bool passed = true;
  passed &= ValidatePositiveBinding();
  passed &= ValidateRefusals();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
