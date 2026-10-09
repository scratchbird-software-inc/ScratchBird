// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define QOW_QRY_016_TYPE_FIXTURE_ONLY
#include "../sbsql_parser_worker/qow_qry_016_type.cpp"
#include "canonical_query_set_composition.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "catalog/datatype_bootstrap_identity.hpp"
#include "datatype_catalog_manifest.hpp"
#include <map>
#include <optional>
#include <stdexcept>

namespace s = scratchbird::engine::sblr;
using Bytes = std::vector<std::uint8_t>;
using Cell = std::optional<Bytes>;
using Bag = std::map<Cell, std::size_t>;
unsigned set_checks = 0;
void Check(bool ok, const char* message) {
  ++set_checks;
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
  const auto builtin = exec::MakeExecutorDescriptor(type);
  descriptor.type_uuid = builtin.type_uuid;
  descriptor.datatype_descriptor_uuid = builtin.datatype_descriptor_uuid;
  descriptor.datatype_descriptor_generation = builtin.datatype_descriptor_generation;
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
    request.result_columns.front().descriptor.datatype_descriptor_uuid = request.left_batch.columns.front().descriptor.datatype_descriptor_uuid;
    request.result_columns.front().descriptor.datatype_descriptor_generation = request.left_batch.columns.front().descriptor.datatype_descriptor_generation;
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
      const auto identity = scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
          api::kBootstrapDatatypeCatalogUuid, api::kBootstrapDatatypeCatalogGeneration,
          api::kBootstrapDatatypeRegistryGeneration, column.descriptor.datatype_descriptor_uuid,
          column.descriptor.datatype_descriptor_generation);
      Check(identity.ok, "set fixture requires actual current datatype codec binding");
      const auto& row = identity.row;
      descriptor.descriptor_uuid = row.descriptor_uuid;
      descriptor.datatype_identity_authoritative = true;
      descriptor.descriptor_generation = row.descriptor_generation;
      descriptor.type_generation = row.type_generation;
      descriptor.codec_id = row.codec_id;
      descriptor.codec_version = row.codec_version;
      descriptor.codec_generation = row.codec_generation;
      descriptor.statement_receipt_uuid = scratchbird::tests::FixtureUuid(2087, 1);
      descriptor.datatype_catalog_snapshot_uuid = row.catalog_snapshot_uuid;
      descriptor.datatype_catalog_generation = row.catalog_generation;
      descriptor.datatype_registry_generation = row.registry_generation;
      dag.descriptors.push_back(descriptor);
    }
    s::plan::CanonicalLogicalRelationalNode root;
    root.logical_node_id = 4703; root.output_descriptor_ids = {4703};
    const auto prepared = s::PrepareSetOperationRootForComposition({}, dag, root, l, r, profile);
    Check(prepared.ok, "native set root preparation failed");
    for (unsigned mutation = 0; mutation < 12; ++mutation) {
      auto invalid_dag = dag;
      auto& target = invalid_dag.descriptors.back();
      switch (mutation) {
        case 0: target.datatype_identity_authoritative = false; break;
        case 1: ++target.descriptor_generation; break;
        case 2: ++target.type_generation; break;
        case 3: target.codec_id += ".stale"; break;
        case 4: ++target.codec_generation; break;
        case 5: ++target.datatype_registry_generation; break;
        case 6: target.datatype_catalog_snapshot_uuid = {}; break;
        case 7: target.statement_receipt_uuid = {}; break;
        case 8: target.width = 16; break;
        case 9: target.precision = 1; break;
        case 10: target.collation_uuid = target.type_uuid; break;
        case 11: target.nullability = api::RelationalNullability::kUnknown; break;
      }
      Check(!s::PrepareSetOperationRootForComposition({}, invalid_dag, root, l, r, profile).ok,
            "set preparation admitted missing/stale/modified datatype binding");
    }
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
void NumericResultBindings() {
  namespace dt = scratchbird::core::datatypes;
  for (const char* name : {"int32", "int64", "int128"}) {
    const auto builtin = exec::MakeExecutorDescriptor(name);
    const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
        api::kBootstrapDatatypeCatalogUuid, api::kBootstrapDatatypeCatalogGeneration,
        api::kBootstrapDatatypeRegistryGeneration, builtin.datatype_descriptor_uuid,
        builtin.datatype_descriptor_generation);
    Check(identity.ok, "numeric result binding has no current codec row");
    const auto& row = identity.row;
    api::RelationalTypeDescriptor source;
    source.descriptor_id = 1;
    source.descriptor_uuid = row.descriptor_uuid;
    source.type_uuid = row.type_uuid;
    source.nullability = api::RelationalNullability::kNullable;
    source.datatype_identity_authoritative = true;
    source.descriptor_generation = row.descriptor_generation;
    source.type_generation = row.type_generation;
    source.codec_id = row.codec_id;
    source.codec_version = row.codec_version;
    source.codec_generation = row.codec_generation;
    source.statement_receipt_uuid = scratchbird::tests::FixtureUuid(2087, 2);
    source.datatype_catalog_snapshot_uuid = row.catalog_snapshot_uuid;
    source.datatype_catalog_generation = row.catalog_generation;
    source.datatype_registry_generation = row.registry_generation;
    const auto type = dt::CanonicalTypeIdFromStableName(name);
    api::EngineDescriptor output;
    Check(s::BuildExactCanonicalScalarRuntimeDescriptorV1(source, type, &output) &&
          output.datatype_descriptor_uuid == row.descriptor_uuid &&
          output.datatype_descriptor_generation == row.descriptor_generation &&
          output.type_uuid == row.type_uuid && output.canonical_type_name == name &&
          output.encoded_descriptor == "nullability=nullable",
          "numeric result projection lost exact supplied binding");
    const auto good = output;
    // A statement-local SBLP occurrence is not the canonical datatype UUID.
    auto occurrence = source;
    occurrence.descriptor_uuid = scratchbird::tests::FixtureUuid(2087, 31);
    Check(occurrence.descriptor_uuid != row.descriptor_uuid &&
          s::BuildExactCanonicalScalarRuntimeDescriptorV1(occurrence, type, &output) &&
          output.descriptor_uuid == occurrence.descriptor_uuid &&
          output.datatype_descriptor_uuid == row.descriptor_uuid &&
          output.datatype_descriptor_generation == row.descriptor_generation &&
          output.type_uuid == row.type_uuid,
          "occurrence identity was confused with canonical datatype identity");
    for (unsigned mutation = 0; mutation < 18; ++mutation) {
      auto invalid = source;
      switch (mutation) {
        case 0: invalid.datatype_identity_authoritative = false; break;
        case 1: invalid.descriptor_uuid = {}; break;
        case 2: ++invalid.descriptor_generation; break;
        case 3: invalid.type_uuid = {}; break;
        case 4: ++invalid.type_generation; break;
        case 5: invalid.codec_id += "stale"; break;
        case 6: ++invalid.codec_version; break;
        case 7: ++invalid.codec_generation; break;
        case 8: invalid.datatype_catalog_snapshot_uuid = {}; break;
        case 9: ++invalid.datatype_catalog_generation; break;
        case 10: ++invalid.datatype_registry_generation; break;
        case 11: invalid.statement_receipt_uuid = {}; break;
        case 12: invalid.nullability = api::RelationalNullability::kUnknown; break;
        case 13: invalid.width = 4; break;
        case 14: invalid.precision = 1; break;
        case 15: invalid.scale = 1; break;
        case 16: invalid.collation_uuid = row.type_uuid; break;
        case 17: invalid.descriptor_uuid.bytes[6] = 0x40; break;
      }
      output = good;
      Check(!s::BuildExactCanonicalScalarRuntimeDescriptorV1(invalid, type, &output) &&
            output == api::EngineDescriptor{}, "refused numeric binding retained prior output or acquired authority");
    }
  }
}

int main() {
  NumericResultBindings();
  Check(ValidateSetOperationTypeReconciliation(), "existing numeric set reconciliation regressed");
  NativeSets("uuid"); NativeSets("binary");
  std::cout << "native_set_value checks=" << set_checks << " native_profiles=48 failures=0\n";
}
