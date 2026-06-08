// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "query/plan_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

void RequireOk(const api::EnginePlanOperationResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "CDP-023 UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

api::EngineTypedValue IntValue(std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "int64";
  typed.descriptor.encoded_descriptor = "canonical=int64";
  typed.encoded_value = std::to_string(value);
  return typed;
}

api::EngineRowValue Row(std::int64_t key, std::int64_t payload) {
  api::EngineRowValue row;
  row.fields.push_back({"id", IntValue(key)});
  row.fields.push_back({"payload", IntValue(payload)});
  return row;
}

api::EngineQueryRelation Relation(const std::string& uuid_text,
                                  std::string name,
                                  std::int64_t first_key,
                                  std::int64_t row_count) {
  api::EngineQueryRelation relation;
  relation.relation_name = std::move(name);
  relation.source_object.uuid.canonical = uuid_text;
  relation.source_object.object_kind = "table";
  relation.descriptor_digest = "descriptor:" + uuid_text;
  for (std::int64_t i = 0; i < row_count; ++i) {
    relation.rows.push_back(Row(first_key + i, 1000 + i));
  }
  return relation;
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "cdp023-join-costing";
  context.database_uuid.canonical = NewUuidText(platform::UuidKind::database, 10);
  context.principal_uuid.canonical = NewUuidText(platform::UuidKind::principal, 11);
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object, 12);
  context.transaction_uuid.canonical = NewUuidText(platform::UuidKind::transaction, 13);
  context.local_transaction_id = 77;
  context.snapshot_visible_through_local_transaction_id = 77;
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EnginePlanOperationRequest JoinRequest(std::vector<std::string> options = {},
                                            std::string explicit_algorithm = {}) {
  const std::string left_uuid = NewUuidText(platform::UuidKind::object, 20);
  const std::string right_uuid = NewUuidText(platform::UuidKind::object, 21);
  api::EnginePlanOperationRequest request;
  request.context = Context();
  request.execute = true;
  request.query_operation = "join";
  request.join_algorithm = std::move(explicit_algorithm);
  request.left_key_field = "id";
  request.right_key_field = "id";
  request.relations.push_back(Relation(left_uuid, "left_orders", 1, 8));
  request.relations.push_back(Relation(right_uuid, "right_items", 1, 256));
  request.option_envelopes = std::move(options);
  return request;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == value) return true;
  }
  return false;
}

bool HasEvidencePrefix(const api::EngineApiResult& result,
                       std::string_view kind,
                       std::string_view prefix) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        evidence.evidence_id.rfind(std::string(prefix), 0) == 0) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> ResultSignature(const api::EnginePlanOperationResult& result) {
  std::vector<std::string> values;
  for (const auto& row : result.result_shape.rows) {
    Require(row.fields.size() >= 4, "CDP-023 joined row width mismatch");
    values.push_back(row.fields[0].second.encoded_value + ":" +
                     row.fields[2].second.encoded_value);
  }
  std::sort(values.begin(), values.end());
  return values;
}

}  // namespace

int main() {
  const auto planned_hash = api::EnginePlanOperation(JoinRequest());
  RequireOk(planned_hash, "CDP-023 planned hash join failed");
  Require(planned_hash.output_row_count == 8, "CDP-023 planned hash output count mismatch");
  Require(HasEvidence(planned_hash, "optimizer_selected_access", "join_hash"),
          "CDP-023 optimizer did not select hash join from cardinality stats");
  Require(HasEvidence(planned_hash, "query_join_algorithm", "hash"),
          "CDP-023 selected hash join was not routed to execution");
  Require(HasEvidencePrefix(planned_hash, "optimizer_candidate", "CAND-OPT-010:join_nested_loop"),
          "CDP-023 nested-loop candidate cost evidence missing");
  Require(HasEvidencePrefix(planned_hash, "optimizer_candidate", "CAND-OPT-011:join_hash"),
          "CDP-023 hash candidate cost evidence missing");
  Require(HasEvidence(planned_hash, "optimizer_join_left_cardinality", "8"),
          "CDP-023 left cardinality evidence missing");
  Require(HasEvidence(planned_hash, "optimizer_join_right_cardinality", "256"),
          "CDP-023 right cardinality evidence missing");

  const auto stale_fallback = api::EnginePlanOperation(JoinRequest({"statistics_stale:true"}));
  RequireOk(stale_fallback, "CDP-023 stale-stat fallback join failed");
  Require(HasEvidence(stale_fallback, "optimizer_selected_access", "join_nested_loop"),
          "CDP-023 stale stats did not fail safe to nested loop");
  Require(HasEvidence(stale_fallback, "query_join_algorithm", "nested_loop"),
          "CDP-023 stale-stat nested loop was not routed to execution");
  Require(HasEvidencePrefix(stale_fallback,
                            "optimizer_candidate_rejected",
                            "CAND-OPT-011:executor_hash_join_unavailable"),
          "CDP-023 stale-stat hash rejection evidence missing");
  Require(ResultSignature(planned_hash) == ResultSignature(stale_fallback),
          "CDP-023 stale-stat fallback changed join result");

  const auto disabled_costing = api::EnginePlanOperation(JoinRequest({"optimizer_join_costing:disabled"}));
  RequireOk(disabled_costing, "CDP-023 disabled join costing fallback failed");
  Require(HasEvidence(disabled_costing, "optimizer_join_costing", "disabled"),
          "CDP-023 disabled join costing evidence missing");
  Require(HasEvidence(disabled_costing, "query_join_algorithm", "nested_loop"),
          "CDP-023 disabled join costing did not use baseline nested loop");
  Require(ResultSignature(planned_hash) == ResultSignature(disabled_costing),
          "CDP-023 disabled join costing changed join result");

  const auto explicit_merge = api::EnginePlanOperation(JoinRequest({}, "merge"));
  RequireOk(explicit_merge, "CDP-023 explicit merge join failed");
  Require(HasEvidence(explicit_merge, "query_join_algorithm", "merge"),
          "CDP-023 explicit merge join was not routed to typed execution");
  Require(ResultSignature(planned_hash) == ResultSignature(explicit_merge),
          "CDP-023 explicit merge join changed join result");

  return EXIT_SUCCESS;
}
