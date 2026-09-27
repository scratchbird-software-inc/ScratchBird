// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define QOW_QRY_016_TYPE_FIXTURE_ONLY
#include "../sbsql_parser_worker/qow_qry_016_type.cpp"
#include "canonical_query_set_composition.hpp"
#include <map>
#include <optional>
#include <stdexcept>

namespace s = scratchbird::engine::sblr;
using Bytes = std::vector<std::uint8_t>;
using Cell = std::optional<Bytes>;
using Bag = std::map<Cell, std::size_t>;
void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
Bag Counts(const std::vector<Cell>& cells) {
  Bag result;
  for (const auto& cell : cells) ++result[cell];
  return result;
}
Bag Oracle(const std::vector<Cell>& left, const std::vector<Cell>& right,
           exec::CanonicalSetOperationKind operation, bool distinct) {
  const auto a = Counts(left), b = Counts(right);
  Bag expected;
  auto keys = a;
  for (const auto& [key, count] : b) keys[key] += count;
  for (const auto& [key, ignored] : keys) {
    const auto l = a.contains(key) ? a.at(key) : 0;
    const auto r = b.contains(key) ? b.at(key) : 0;
    std::size_t count = 0;
    if (operation == exec::CanonicalSetOperationKind::kUnion) count = distinct ? bool(l || r) : l + r;
    if (operation == exec::CanonicalSetOperationKind::kIntersect) count = distinct ? bool(l && r) : std::min(l, r);
    if (operation == exec::CanonicalSetOperationKind::kExcept) count = distinct ? bool(l && !r) : l > r ? l - r : 0;
    if (count) expected[key] = count;
  }
  return expected;
}
void Fill(exec::DescriptorBatch& batch, const std::vector<Cell>& cells, const char* type) {
  auto descriptor = batch.columns.front().descriptor;
  descriptor.canonical_type_name = type;
  descriptor.type_uuid = exec::MakeExecutorDescriptor(type).type_uuid;
  Check(!descriptor.type_uuid.is_nil(), "native datatype has no Core identity");
  batch.columns.front().descriptor = descriptor;
  batch.rows.clear();
  for (const auto& cell : cells) {
    auto value = cell ? Value(descriptor, {}) : Null(descriptor);
    if (cell) value.binary_value = *cell;
    batch.rows.push_back({{std::move(value)}});
  }
}
std::size_t Verify(const exec::DescriptorBatch& batch, const api::EngineDescriptor& descriptor,
                  const Bag& expected) {
  Bag actual;
  std::size_t payload = 0;
  for (const auto& row : batch.rows) {
    Check(row.values.size() == 1, "set output row width changed");
    const auto& value = row.values.front();
    Check(value.descriptor == descriptor && value.encoded_value.empty(),
          "set output lost target binding or leaked binary through text");
    if (value.state == api::EngineValueState::sql_null) {
      Check(value.is_null && value.binary_value.empty(), "set NULL has payload");
      ++actual[std::nullopt];
    } else {
      Check(value.state == api::EngineValueState::value && !value.is_null, "set output is not a value");
      ++actual[value.binary_value];
      payload += value.binary_value.size();
    }
  }
  Check(actual == expected, "native set membership or multiplicity differs from independent bag oracle");
  return payload;
}
auto Execute(const exec::CanonicalSetOperationAllRequest& request) {
  return request.quantifier == exec::CanonicalSetOperationQuantifier::kAll
      ? exec::ExecuteCanonicalSetOperationAll(request) : exec::ExecuteCanonicalSetOperationDistinct(request);
}
void NativeSets(const char* type) {
  std::vector<Cell> left, right;
  for (unsigned pattern = 0; pattern < 130; ++pattern) {
    Bytes bits(16);
    if (pattern == 1) std::fill(bits.begin(), bits.end(), 0xff);
    if (pattern >= 2) bits[(pattern - 2) / 8] = std::uint8_t(1u << ((pattern - 2) % 8));
    left.emplace_back(bits);
    if (pattern % 2 == 0) right.emplace_back(bits);
  }
  if (std::string_view(type) == "binary") {
    for (const auto& bits : {Bytes{}, Bytes{0}, Bytes{0, 0}, Bytes{0xff}, Bytes{0x80, 0, 0xff}})
      left.emplace_back(bits);
    right.emplace_back(Bytes{});
  }
  left.push_back(left[2]); left.push_back(left[2]); left.push_back(std::nullopt);
  right.push_back(left[2]); right.push_back(std::nullopt); right.push_back(std::nullopt);
  for (auto operation : {exec::CanonicalSetOperationKind::kUnion,
                         exec::CanonicalSetOperationKind::kIntersect,
                         exec::CanonicalSetOperationKind::kExcept})
  for (bool distinct : {false, true}) for (bool by_name : {false, true})
  for (bool reconcile : {false, true}) {
    auto request = Request(operation, distinct ? exec::CanonicalSetOperationQuantifier::kDistinct
                                              : exec::CanonicalSetOperationQuantifier::kAll);
    Fill(request.left_batch, left, type); Fill(request.right_batch, right, type);
    request.result_columns.front().descriptor.canonical_type_name = type;
    request.result_columns.front().descriptor.type_uuid = request.left_batch.columns.front().descriptor.type_uuid;
    const auto semantic = "set-operation." + OperationName(operation) + (distinct ? "-distinct" : "-all") +
        (by_name ? ".by-name" : "") + (reconcile ? ".type-reconciled" : "") + ".v1";
    const auto profile = s::ResolveLiveSetOperationProfileForComposition(semantic);
    Check(profile.matched, "native set semantic profile missing");
    request.type_profile = profile.type_profile; request.alignment = profile.alignment;
    request.physical_dag.nodes.back().implementation_id = profile.implementation_id;
    request.physical_dag.memory_budget_bytes = 8 * 1024 * 1024;
    request.physical_dag.nodes.back().memory_bytes_required = 2 * 1024 * 1024;
    request.maximum_output_row_count = 1024;
    request.enforce_payload_memory_grant = true;
    const auto expected = Oracle(left, right, operation, distinct);
    const auto result = Execute(request);
    if (!result.diagnostic.ok) std::cerr << semantic << ':' << result.diagnostic.detail << '\n';
    Check(result.diagnostic.ok, "native physical set execution failed");
    const auto bytes = Verify(result.output_batch, request.result_columns.front().descriptor, expected);
    // The logical-payload ABI charges one byte for each retained batch,
    // including an empty one, in addition to its actual value bytes.
    Check(result.output_payload_bytes == bytes + 1 && result.peak_live_memory_bytes >= bytes + 1 &&
          result.memory_grant_bytes == 2 * 1024 * 1024 && result.executed_physical_node_id == 4703 &&
          result.selected_plan_uuid == request.physical_dag.selected_plan_uuid,
          "native set lost payload accounting or execution identity");
    auto constrained = request;
    constrained.physical_dag.nodes.back().memory_bytes_required = result.peak_live_memory_bytes - 1;
    const auto limited = Execute(constrained);
    Check(!limited.diagnostic.ok && limited.diagnostic.diagnostic_code == "SBLR.PLAN_TREE.RESOURCE_LIMIT" &&
          limited.output_batch.rows.empty(), "native set exceeded its memory grant");

    s::MaterializedValues l, r;
    l.ok = r.ok = true; l.batch = request.left_batch; r.batch = request.right_batch;
    l.result_bindings.resize(1); r.result_bindings.resize(1);
    api::TypedRelationalDag dag;
    for (const auto& column : {l.batch.columns.front(), r.batch.columns.front(), request.result_columns.front()}) {
      api::RelationalTypeDescriptor descriptor;
      descriptor.descriptor_id = column.descriptor_id;
      descriptor.descriptor_uuid = column.descriptor.descriptor_uuid;
      descriptor.type_uuid = column.descriptor.type_uuid;
      descriptor.nullability = api::RelationalNullability::kNullable;
      dag.descriptors.push_back(descriptor);
    }
    s::plan::CanonicalLogicalRelationalNode root;
    root.logical_node_id = 4703; root.output_descriptor_ids = {4703};
    const auto prepared = s::PrepareSetOperationRootForComposition({}, dag, root, l, r, profile);
    Check(prepared.ok, "native set root preparation failed");
    const auto planned = s::MaterializeSetOperationPlanningStateForComposition(prepared, profile, l, r);
    if (!planned.values.ok) std::cerr << planned.values.detail << '\n';
    Check(planned.values.ok, "native set planning failed");
    Verify(planned.values.batch, prepared.result_columns.front().descriptor, expected);
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
      auto bad = request;
      auto& value = bad.left_batch.rows.front().values.front();
      if (mutation == 0) value.encoded_value = std::string(16, '\0');
      if (mutation == 1) { value.is_null = true; value.state = api::EngineValueState::sql_null; }
      if (mutation == 2) value.state = api::EngineValueState::error;
      if (mutation == 3) value.binary_value.resize(15);
      if (mutation == 4) value.binary_value.resize(17);
      if (mutation == 5) { value.binary_value.clear(); value.encoded_value = std::string(16, '\0'); }
      if (mutation == 6) value.is_null = true;
      if (mutation == 7) bad.left_batch.rows.front().values.clear();
      if ((mutation == 3 || mutation == 4) && std::string_view(type) == "binary") continue;
      const auto refused = Execute(bad);
      Check(!refused.diagnostic.ok && refused.output_batch.rows.empty() && refused.selected_plan_uuid.is_nil(),
            "malformed native set operand published physical results");
      auto bad_left = l; bad_left.batch = bad.left_batch;
      const auto refused_plan = s::MaterializeSetOperationPlanningStateForComposition(prepared, profile, bad_left, r);
      Check(!refused_plan.values.ok && refused_plan.values.batch.rows.empty(),
            "malformed native set operand produced a successful planning result");
    }
  }
}
int main() {
  Check(ValidateSetOperationTypeReconciliation(), "existing numeric set reconciliation regressed");
  NativeSets("uuid"); NativeSets("binary");
}
