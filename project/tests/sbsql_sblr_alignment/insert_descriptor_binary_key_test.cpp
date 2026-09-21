// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/insert_descriptor_key.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <type_traits>

namespace api = scratchbird::engine::internal_api;

namespace {
std::size_t checks = 0;
void Require(bool value, const char* reason) {
  ++checks;
  if (!value) { std::cerr << reason << '\n'; std::exit(1); }
}
api::EngineUuid Id(std::uint8_t n) {
  api::EngineUuid value{};
  value.bytes = {1, 159, 48, 0, 0, 0, 112, 0, 128, 0, 0, 0, 0, 0, 0, n};
  return value;
}
struct Fixture {
  api::EngineInsertRowsRequest request;
  api::CrudTableRecord table;
  std::vector<api::CrudIndexRecord> indexes{1};
  api::InsertFeatureGates gates;
  api::SecondaryIndexDeltaLedgerPolicy delta;
  Fixture() {
    auto& c = request.context;
    c.database_uuid = Id(1); c.principal_uuid = Id(2);
    c.session_uuid = Id(3); c.transaction_uuid = Id(4);
    c.current_role_uuid = Id(5); c.statement_uuid = Id(6);
    c.statement_snapshot_uuid = Id(7); c.catalog_epoch_uuid = Id(8);
    c.local_transaction_id = 23; c.security_context_present = true;
    c.authorization_context.present = true;
    c.authorization_context.authority_uuid = Id(9);
    c.authorization_context.principal_uuid = c.principal_uuid;
    c.authorization_context.effective_subjects = {{Id(2), "principal"}, {Id(10), "group"}};
    api::EngineMaterializedAuthorizationGrant grant;
    grant.grant_uuid = Id(11); grant.subject_uuid = Id(2);
    grant.subject_kind = "principal"; grant.target_uuid = Id(12); grant.right = "INSERT";
    c.authorization_context.grants.push_back(grant);
    api::EngineMaterializedAuthorizationPolicy policy;
    policy.policy_uuid = Id(13); policy.subject_uuid = Id(2);
    policy.subject_kind = "principal"; policy.target_uuid = Id(12);
    policy.right = "INSERT"; policy.policy_kind = "rls_filter";
    policy.effective_policy_uuid = Id(14); policy.effective_expression_uuid = Id(15);
    c.authorization_context.policies.push_back(policy);
    c.authorization_context.evidence_tags = {"one", "two"};
    table.table_uuid = Id(12); table.columns = {{"x", "BIGINT"}, {"y", "TEXT"}};
    request.target_table.uuid = table.table_uuid;
    request.target_object.uuid = table.table_uuid;
    request.target_schema.uuid = Id(16);
    request.bound_object_identity.object_uuid = table.table_uuid;
    request.bound_object_identity.resolved_schema_uuid = Id(16);
    indexes[0].index_uuid = Id(17); indexes[0].table_uuid = table.table_uuid;
    indexes[0].family = "btree"; indexes[0].column_name = "x";
    indexes[0].key_envelopes = {"x", "y"}; indexes[0].include_columns = {"z"};
  }
  api::InsertDescriptorKey Key() const {
    return api::BuildInsertDescriptorKey(request, table, indexes, gates, delta,
        api::InsertBatchMode::singleton, api::InsertDuplicateMode::error, false);
  }
};
}  // namespace

int main() {
  static_assert(std::is_same_v<decltype(api::PreparedInsertRow::row_uuid), api::EngineUuid>);
  static_assert(std::is_same_v<decltype(api::InsertBatchContext::statement_uuid), api::EngineUuid>);
  static_assert(std::is_same_v<decltype(api::InsertRowEncoderPlan::table_uuid), api::EngineUuid>);
  // Independent wire-shape oracle: type tags, LE64 number, exact raw16 UUID,
  // and length-delimited user text including NUL and delimiter bytes.
  api::InsertDescriptorKeyEncoder encoder(9);
  encoder.Uuid(Id(42)); encoder.Text(std::string("a\0|;", 4));
  api::InsertDescriptorKey expected{1,9,0,0,0,0,0,0,0,2,
      1,159,48,0,0,0,112,0,128,0,0,0,0,0,0,42,
      3,4,0,0,0,0,0,0,0,'a',0,'|',';'};
  Require(std::move(encoder).Finish() == expected, "binary key layout drift");
  Fixture base;
  const auto original = base.Key();
  api::EngineInsertDescriptorExpectation expected_binding;
  const auto expectation_failure = [&] {
    return api::InsertDescriptorExpectationFailure(expected_binding,
        base.request.context.principal_uuid, base.request.context.current_role_uuid,
        base.request.context.session_uuid, original);
  };
  const auto failure_text = [&] {
    const char* failure = expectation_failure();
    return std::string_view(failure ? failure : "");
  };
  Require(expectation_failure() == nullptr, "absent expectation refused");
  expected_binding.principal_uuid = base.request.context.principal_uuid;
  expected_binding.role_uuid = base.request.context.current_role_uuid;
  expected_binding.session_uuid = base.request.context.session_uuid;
  expected_binding.content_key = original;
  Require(expectation_failure() == nullptr, "exact binary binding refused");
  expected_binding.principal_uuid = Id(99);
  Require(failure_text() == "cross_user", "principal mismatch admitted");
  expected_binding.principal_uuid = base.request.context.principal_uuid;
  expected_binding.role_uuid = Id(99);
  Require(failure_text() == "cross_role", "role mismatch admitted");
  expected_binding.role_uuid = base.request.context.current_role_uuid;
  expected_binding.session_uuid = Id(99);
  Require(failure_text() == "cross_session", "session mismatch admitted");
  expected_binding.session_uuid = base.request.context.session_uuid;
  for (std::size_t n = 0; n < original.size(); ++n) {
    (*expected_binding.content_key)[n] ^= 128;
    Require(failure_text() == "stale_descriptor_key", "content key byte ignored");
    (*expected_binding.content_key)[n] ^= 128;
  }
  expected_binding.content_key->pop_back();
  Require(failure_text() == "stale_descriptor_key", "truncated key admitted");
  const auto differs = [&](auto change, const char* reason) {
    Fixture other = base; change(other);
    Require(other.Key() != original, reason);
  };
  const std::vector<api::EngineUuid api::EngineRequestContext::*> identities{
      &api::EngineRequestContext::database_uuid, &api::EngineRequestContext::session_uuid,
      &api::EngineRequestContext::principal_uuid, &api::EngineRequestContext::current_role_uuid,
      &api::EngineRequestContext::transaction_uuid, &api::EngineRequestContext::statement_snapshot_uuid,
      &api::EngineRequestContext::catalog_epoch_uuid};
  // Every byte is retained, including embedded zeroes and high-bit bytes.
  for (auto member : identities) for (std::size_t byte = 0; byte < 16; ++byte)
    for (unsigned bit = 0; bit < 8; ++bit)
      differs([&](auto& f) { (f.request.context.*member).bytes[byte] ^= 1u << bit; },
              "owner UUID bit lost from cache key");
  differs([](auto& f) { ++f.request.context.authorization_context.security_context_generation; }, "auth generation omitted");
  differs([](auto& f) { ++f.request.context.local_transaction_id; }, "local transaction omitted");
  differs([](auto& f) { ++f.request.context.statement_snapshot_generation; }, "snapshot generation omitted");
  differs([](auto& f) { ++f.request.context.catalog_generation_id; }, "catalog epoch omitted");
  differs([](auto& f) { ++f.request.context.security_epoch; }, "security epoch omitted");
  differs([](auto& f) { ++f.request.context.resource_epoch; }, "resource epoch omitted");
  differs([](auto& f) { ++f.request.context.name_resolution_epoch; }, "name epoch omitted");
  differs([](auto& f) { ++f.request.bound_object_identity.object_descriptor_generation; }, "bound generation omitted");
  differs([](auto& f) { ++f.table.bound_column_generation; }, "column generation omitted");
  differs([](auto& f) { ++f.table.bound_relation_generation; }, "relation generation omitted");
  differs([](auto& f) { f.table.temporary_session_uuid = Id(99); }, "temporary owner omitted");
  differs([](auto& f) { f.request.require_generated_row_uuid = false; }, "row issuance policy omitted");
  differs([](auto& f) { f.gates.sorted_run_shadow_load = api::InsertFeatureState::enabled; }, "index plan gate omitted");
  differs([](auto& f) { f.indexes[0].approximate = true; }, "approximation flag omitted");
  differs([](auto& f) { std::swap(f.indexes[0].key_envelopes[0], f.indexes[0].key_envelopes[1]); }, "index key order collapsed");
  differs([](auto& f) { f.indexes[0].include_columns.insert(f.indexes[0].include_columns.begin(), "y"); f.indexes[0].key_envelopes.pop_back(); }, "index key/include boundary collapsed");
  differs([](auto& f) { std::swap(f.indexes[0].column_name, f.indexes[0].family); }, "index field roles collapsed");
  auto policy_differs = [&](auto change) {
    differs([&](auto& f) { change(f.request.context.authorization_context.policies[0]); }, "policy authority omitted");
  };
  policy_differs([](auto& p) { ++p.source_policy_generation; });
  policy_differs([](auto& p) { ++p.effective_policy_generation; });
  policy_differs([](auto& p) { ++p.effective_expression_generation; });
  policy_differs([](auto& p) { p.effective_policy_uuid = Id(99); });
  policy_differs([](auto& p) { p.effective_expression_uuid = Id(99); });
  policy_differs([](auto& p) { p.deny = true; });
  policy_differs([](auto& p) { p.requires_runtime_recheck = true; });
  policy_differs([](auto& p) { ++p.update_policy_phase; });
  for (std::size_t n = 0; n < 32; ++n)
    policy_differs([&](auto& p) { p.effective_expression_evidence_sha256[n] ^= 128; });
  Fixture left = base, right = base;
  left.table.columns = {{"a|b", "c"}}; right.table.columns = {{"a", "b|c"}};
  Require(left.Key() != right.Key(), "column delimiter collision");
  left = base; right = base;
  auto& lp = left.request.context.authorization_context.policies[0];
  auto& rp = right.request.context.authorization_context.policies[0];
  lp.right = "a|b"; lp.policy_kind = "c";
  rp.right = "a"; rp.policy_kind = "b|c";
  Require(left.Key() != right.Key(), "policy delimiter collision");
  left = base; right = base;
  left.request.option_envelopes = {"a=1", "a=2"};
  right.request.option_envelopes = {"a=2", "a=1"};
  Require(left.Key() != right.Key(), "option precedence lost");
  left = base;
  std::reverse(left.request.context.authorization_context.effective_subjects.begin(),
               left.request.context.authorization_context.effective_subjects.end());
  std::reverse(left.request.context.authorization_context.evidence_tags.begin(),
               left.request.context.authorization_context.evidence_tags.end());
  Require(left.Key() == original, "unordered authorization changed key");
  left.request.option_envelopes = {"prepared_descriptor.cache_limit=1"};
  Require(left.Key() == original, "cache lifecycle control changed plan content");
  std::map<api::InsertDescriptorKey, int, api::InsertDescriptorKeyLess> actual_key_map;
  actual_key_map.emplace(original, 1); actual_key_map.emplace(right.Key(), 2);
  Require(actual_key_map.size() == 2 && actual_key_map.at(original) == 1, "exact cache-key comparison failed");
  Require(api::InsertDescriptorKeyLabel("auth", original).size() == 69, "SHA256 label extent");
  Require(api::InsertDescriptorKeyLabel("auth", original) != api::InsertDescriptorKeyLabel("auth", right.Key()), "different key labels coincide");
  std::cout << "insert_descriptor_binary_key=passed checks=" << checks << '\n';
}
