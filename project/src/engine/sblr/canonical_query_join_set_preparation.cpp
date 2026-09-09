// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_correlated_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_join_composition.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_predicate_support.hpp"
#include "canonical_query_projection_registration.hpp"
#include "canonical_query_recursive_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_query_set_composition.hpp"
#include "canonical_query_set_registration.hpp"
#include "canonical_query_sort_registration.hpp"
#include "canonical_relational_expression.hpp"
#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_JOIN_SET_PREPARATION_AUTHORITY
// Join/set descriptor alignment, exact alias carriers, equality/work bounds,
// and bounded planning-state materialization over supplied typed inputs.
// Owns no plan selection, physical DAG dispatch, relation-store access,
// snapshot construction, transaction finality, or public route selection.

namespace {

LiveSetRegistrationProfiles MakeLiveSetRegistrationProfiles(
    const std::unordered_map<std::uint64_t, PreparedLiveSetNode>& prepared) {
  LiveSetRegistrationProfiles profiles;
  profiles.reserve(prepared.size());
  for (const auto& [node_id, node] : prepared) {
    LiveSetNodeRegistrationProfile profile;
    profile.operation = node.profile.operation;
    profile.alignment = node.profile.alignment;
    profile.quantifier = node.profile.quantifier;
    profile.equality_profile = node.profile.equality_profile;
    profile.type_profile = node.profile.type_profile;
    profile.implementation_id = node.profile.implementation_id;
    profile.result_columns = node.prepared.result_columns;
    profile.collation_bindings = node.prepared.collation_bindings;
    profile.maximum_output_row_count = node.maximum_output_row_count;
    profile.maximum_equality_comparison_count =
        node.maximum_equality_comparison_count;
    profiles.emplace(node_id, std::move(profile));
  }
  return profiles;
}

bool BoundSetOperationEqualityComparisons(
    const PreparedSetOperationRoot& prepared,
    const LiveSetOperationProfile& profile,
    const std::uint64_t maximum_input_row_count,
    std::uint64_t* comparison_bound,
    std::uint64_t* collation_comparison_count) {
  if (comparison_bound == nullptr ||
      collation_comparison_count == nullptr || !prepared.ok ||
      !profile.matched ||
      !CheckedMultiply(maximum_input_row_count, maximum_input_row_count,
                       comparison_bound)) {
    return false;
  }
  *collation_comparison_count = 0;
  if (profile.equality_profile !=
      exec::CanonicalSetOperationEqualityProfile::kNullEqualBoundCollation) {
    return true;
  }
  if (prepared.collation_bindings.size() >
      prepared.result_columns.size()) {
    return false;
  }
  std::uint64_t adjacent = maximum_input_row_count;
  std::uint64_t triangular = 0;
  if (adjacent != 0) --adjacent;
  if ((maximum_input_row_count & 1U) == 0) {
    if (!CheckedMultiply(maximum_input_row_count / 2, adjacent,
                         &triangular)) {
      return false;
    }
  } else if (!CheckedMultiply(maximum_input_row_count, adjacent / 2,
                              &triangular)) {
    return false;
  }
  if (!CheckedMultiply(prepared.collation_bindings.size(), triangular,
                       collation_comparison_count)) {
    return false;
  }
  *comparison_bound =
      std::max(*comparison_bound, *collation_comparison_count);
  return true;
}

MaterializedSetOperationPlanningState MaterializeSetOperationPlanningState(
    const PreparedSetOperationRoot& prepared,
    const LiveSetOperationProfile& profile,
    const MaterializedValues& left,
    const MaterializedValues& right) {
  MaterializedSetOperationPlanningState result;
  std::uint64_t collation_comparison_count = 0;
  if (!prepared.ok || !left.ok || !right.ok ||
      (profile.alignment != exec::CanonicalSetOperationAlignment::kOrdinal &&
       profile.alignment !=
           exec::CanonicalSetOperationAlignment::kByName) ||
      (profile.type_profile !=
           exec::CanonicalSetOperationTypeProfile::kExact &&
       profile.type_profile !=
           exec::CanonicalSetOperationTypeProfile::kLosslessImplicit) ||
      (profile.equality_profile !=
           exec::CanonicalSetOperationEqualityProfile::kExactTyped &&
       profile.equality_profile !=
           exec::CanonicalSetOperationEqualityProfile::
               kNullEqualBoundCollation) ||
      !CheckedAdd(left.batch.rows.size(), right.batch.rows.size(),
                  &result.output_bound) ||
      !BoundSetOperationEqualityComparisons(
          prepared, profile, result.output_bound,
          &result.comparison_bound, &collation_comparison_count)) {
    result.values.detail =
        "set-operation planning-state contract or bound is invalid";
    return result;
  }
  std::uint64_t base_work = result.output_bound;
  if ((profile.operation != exec::CanonicalSetOperationKind::kUnion ||
       profile.quantifier !=
           exec::CanonicalSetOperationQuantifier::kAll) &&
      !CheckedMultiply(result.output_bound, result.output_bound,
                       &base_work)) {
    result.values.detail = "set-operation base work bound overflowed";
    return result;
  }
  if (!CheckedAdd(base_work, collation_comparison_count,
                  &result.work_bound)) {
    result.values.detail =
        "set-operation collation work bound overflowed";
    return result;
  }

  result.values.ok = true;
  result.values.batch.columns = prepared.result_columns;
  result.values.result_bindings = prepared.result_bindings;
  const auto retagged_rows = [&](const exec::DescriptorBatch& batch) {
    std::vector<exec::DescriptorTuple> rows;
    rows.reserve(batch.rows.size());
    for (const auto& source : batch.rows) {
      auto row = source;
      for (std::size_t column = 0; column < row.values.size(); ++column) {
        row.values[column].descriptor =
            prepared.result_columns[column].descriptor;
      }
      rows.push_back(std::move(row));
    }
    return rows;
  };
  exec::DescriptorBatch reconciled_left = left.batch;
  exec::DescriptorBatch aligned_right = right.batch;
  if (profile.alignment ==
      exec::CanonicalSetOperationAlignment::kByName) {
    std::unordered_map<std::string, std::size_t> right_ordinals;
    for (std::size_t column = 0; column < right.batch.columns.size();
         ++column) {
      if (right.batch.columns[column].stable_name.empty() ||
          !right_ordinals
               .emplace(right.batch.columns[column].stable_name, column)
               .second) {
        result.values = {};
        result.values.detail =
            "BY NAME set-operation right names are not unique";
        return result;
      }
    }
    aligned_right.columns.clear();
    aligned_right.rows.assign(right.batch.rows.size(), {});
    for (const auto& result_column : prepared.result_columns) {
      const auto found = right_ordinals.find(result_column.stable_name);
      if (found == right_ordinals.end()) {
        result.values = {};
        result.values.detail =
            "BY NAME set-operation column sets differ";
        return result;
      }
      aligned_right.columns.push_back(right.batch.columns[found->second]);
      for (std::size_t row = 0; row < right.batch.rows.size(); ++row) {
        aligned_right.rows[row].values.push_back(
            right.batch.rows[row].values[found->second]);
      }
    }
  }
  if (profile.type_profile ==
      exec::CanonicalSetOperationTypeProfile::kLosslessImplicit) {
    std::string conversion_detail;
    const auto reconcile_batch = [&](exec::DescriptorBatch* batch) {
      if (batch == nullptr ||
          batch->columns.size() != prepared.result_columns.size()) {
        conversion_detail =
            "type-reconciled set-operation arity changed";
        return false;
      }
      for (std::size_t column = 0; column < batch->columns.size(); ++column) {
        const auto source_type = dt::CanonicalTypeIdFromStableName(
            batch->columns[column].descriptor.canonical_type_name);
        const auto target_type = dt::CanonicalTypeIdFromStableName(
            prepared.result_columns[column].descriptor.canonical_type_name);
        const auto category =
            dt::ClassifyDatatypeCast(source_type, target_type);
        if (source_type == dt::CanonicalTypeId::unknown ||
            target_type == dt::CanonicalTypeId::unknown ||
            (category != dt::DatatypeCastCategory::identity &&
             category !=
                 dt::DatatypeCastCategory::lossless_implicit)) {
          conversion_detail =
              "set-operation planning cast is not lossless implicit";
          return false;
        }
        for (auto& row : batch->rows) {
          dt::DatatypeCastRequest conversion;
          conversion.value.type_id = source_type;
          conversion.value.encoded_value =
              row.values[column].encoded_value;
          conversion.value.is_null =
              row.values[column].state ==
              api::EngineValueState::sql_null;
          conversion.target_type_id = target_type;
          const auto cast = dt::CastDatatypeValue(conversion);
          if (!cast.ok()) {
            conversion_detail =
                cast.diagnostic.diagnostic_code.empty()
                    ? "set-operation planning lossless cast refused"
                    : cast.diagnostic.diagnostic_code;
            return false;
          }
          row.values[column].descriptor =
              prepared.result_columns[column].descriptor;
          row.values[column].encoded_value = cast.value.encoded_value;
          row.values[column].binary_value.clear();
          row.values[column].is_null = cast.value.is_null;
          row.values[column].state =
              cast.value.is_null ? api::EngineValueState::sql_null
                                 : api::EngineValueState::value;
        }
        batch->columns[column].descriptor =
            prepared.result_columns[column].descriptor;
        batch->columns[column].nullable =
            prepared.result_columns[column].nullable;
      }
      return true;
    };
    if (!reconcile_batch(&reconciled_left) ||
        !reconcile_batch(&aligned_right)) {
      result.values = {};
      result.values.detail = std::move(conversion_detail);
      return result;
    }
    std::uint64_t conversion_count = 0;
    if (!CheckedMultiply(result.output_bound,
                         prepared.result_columns.size(),
                         &conversion_count) ||
        !CheckedAdd(result.work_bound, conversion_count,
                    &result.work_bound)) {
      result.values = {};
      result.values.detail =
          "set-operation conversion work bound overflowed";
      return result;
    }
  }
  const auto left_rows = retagged_rows(reconciled_left);
  const auto right_rows = retagged_rows(aligned_right);
  using SetRowKey = std::vector<std::string>;
  const auto row_key = [&](const exec::DescriptorTuple& row,
                           SetRowKey* key) {
    if (key == nullptr) return false;
    key->clear();
    key->reserve(row.values.size());
    for (const auto& value : row.values) {
      if (value.state == api::EngineValueState::sql_null) {
        key->emplace_back("null");
        continue;
      }
      std::string token;
      std::string detail;
      if (!EncodeCanonicalScalarEqualityKey(value, &token, &detail)) {
        result.values.detail =
            "set-operation planning equality key: " + detail;
        return false;
      }
      key->push_back(std::move(token));
    }
    return true;
  };
  std::vector<SetRowKey> left_keys;
  std::vector<SetRowKey> right_keys;
  left_keys.reserve(left_rows.size());
  right_keys.reserve(right_rows.size());
  for (const auto& row : left_rows) {
    SetRowKey key;
    if (!row_key(row, &key)) return result;
    left_keys.push_back(std::move(key));
  }
  for (const auto& row : right_rows) {
    SetRowKey key;
    if (!row_key(row, &key)) return result;
    right_keys.push_back(std::move(key));
  }

  if (profile.equality_profile ==
      exec::CanonicalSetOperationEqualityProfile::
          kNullEqualBoundCollation) {
    for (const auto& binding : prepared.collation_bindings) {
      if (binding.result_column >= prepared.result_columns.size()) {
        result.values = {};
        result.values.detail =
            "set-operation planning collation column is out of range";
        return result;
      }
      std::vector<const api::EngineTypedValue*> representatives;
      const auto classify = [&](const api::EngineTypedValue& value,
                                std::string* equality_key) {
        if (equality_key == nullptr) return false;
        if (value.state == api::EngineValueState::sql_null) {
          *equality_key = "collation:null";
          return true;
        }
        for (std::size_t index = 0; index < representatives.size(); ++index) {
          dt::DatatypeComparisonRequest comparison_request;
          comparison_request.left.type_id = dt::CanonicalTypeId::character;
          comparison_request.left.encoded_value =
              representatives[index]->encoded_value;
          comparison_request.right.type_id = dt::CanonicalTypeId::character;
          comparison_request.right.encoded_value = value.encoded_value;
          comparison_request.case_insensitive_character_compare =
              binding.text_seed.collation_case_insensitive;
          comparison_request.text_seed = binding.text_seed;
          const auto compared = dt::CompareDatatypeValues(comparison_request);
          if (!compared.ok()) {
            result.values.detail =
                compared.diagnostic.diagnostic_code.empty()
                    ? "set-operation planning collation comparison refused"
                    : compared.diagnostic.diagnostic_code;
            return false;
          }
          if (compared.comparison == 0) {
            *equality_key = "collation:" + std::to_string(index);
            return true;
          }
        }
        representatives.push_back(&value);
        *equality_key =
            "collation:" + std::to_string(representatives.size() - 1);
        return true;
      };
      const auto classify_rows = [&](const std::vector<exec::DescriptorTuple>& rows,
                                     std::vector<SetRowKey>* keys) {
        if (keys == nullptr || keys->size() != rows.size()) return false;
        for (std::size_t row = 0; row < rows.size(); ++row) {
          if (!classify(rows[row].values[binding.result_column],
                        &(*keys)[row][binding.result_column])) {
            return false;
          }
        }
        return true;
      };
      if (!classify_rows(left_rows, &left_keys) ||
          !classify_rows(right_rows, &right_keys)) {
        const auto detail = result.values.detail.empty()
                                ? "set-operation planning collation key "
                                  "classification refused"
                                : result.values.detail;
        result.values = {};
        result.values.detail = detail;
        return result;
      }
    }
  }

  result.values.batch.rows.reserve(result.output_bound);
  const bool distinct =
      profile.quantifier ==
      exec::CanonicalSetOperationQuantifier::kDistinct;
  if (distinct &&
      profile.operation == exec::CanonicalSetOperationKind::kUnion) {
    std::set<SetRowKey> emitted;
    for (std::size_t row = 0; row < left_rows.size(); ++row) {
      if (emitted.insert(left_keys[row]).second) {
        result.values.batch.rows.push_back(left_rows[row]);
      }
    }
    for (std::size_t row = 0; row < right_rows.size(); ++row) {
      if (emitted.insert(right_keys[row]).second) {
        result.values.batch.rows.push_back(right_rows[row]);
      }
    }
  } else if (distinct) {
    std::set<SetRowKey> right_membership(right_keys.begin(), right_keys.end());
    std::set<SetRowKey> emitted;
    for (std::size_t row = 0; row < left_rows.size(); ++row) {
      const bool present = right_membership.contains(left_keys[row]);
      const bool candidate =
          profile.operation == exec::CanonicalSetOperationKind::kIntersect
              ? present
              : !present;
      if (candidate && emitted.insert(left_keys[row]).second) {
        result.values.batch.rows.push_back(left_rows[row]);
      }
    }
  } else if (profile.operation ==
             exec::CanonicalSetOperationKind::kUnion) {
    result.values.batch.rows.insert(result.values.batch.rows.end(),
                                    left_rows.begin(), left_rows.end());
    result.values.batch.rows.insert(result.values.batch.rows.end(),
                                    right_rows.begin(), right_rows.end());
  } else {
    std::map<SetRowKey, std::size_t> right_multiplicity;
    for (const auto& key : right_keys) ++right_multiplicity[key];
    for (std::size_t row = 0; row < left_rows.size(); ++row) {
      auto found = right_multiplicity.find(left_keys[row]);
      const bool consumes =
          found != right_multiplicity.end() && found->second != 0;
      if (consumes) --found->second;
      const bool emit =
          profile.operation == exec::CanonicalSetOperationKind::kIntersect
              ? consumes
              : !consumes;
      if (emit) result.values.batch.rows.push_back(left_rows[row]);
    }
  }
  return result;
}

bool ExactCanonicalBooleanJoinAliasDescriptorV1(
    const api::RelationalTypeDescriptor& descriptor) {
  if (!descriptor.datatype_identity_authoritative ||
      (descriptor.nullability != api::RelationalNullability::kNonNull &&
       descriptor.nullability != api::RelationalNullability::kNullable) ||
      descriptor.descriptor_uuid != descriptor.type_uuid ||
      !CanonicalUuidText(descriptor.statement_receipt_uuid)) {
    return false;
  }
  const auto identity = dt::LookupCanonicalBooleanTypeCodecIdentityV1(
      descriptor.datatype_catalog_snapshot_uuid,
      descriptor.datatype_catalog_generation,
      descriptor.datatype_registry_generation);
  return identity.ok &&
         descriptor.descriptor_uuid == identity.row.descriptor_uuid &&
         descriptor.descriptor_generation ==
             identity.row.descriptor_generation &&
         descriptor.type_uuid == identity.row.type_uuid &&
         descriptor.type_generation == identity.row.type_generation &&
         descriptor.codec_id == identity.row.codec_id &&
         descriptor.codec_version == identity.row.codec_version &&
         descriptor.codec_generation == identity.row.codec_generation &&
         dt::IsExactCanonicalBooleanDescriptorTypeAliasV1(
             descriptor.descriptor_uuid, descriptor.descriptor_generation,
             descriptor.type_uuid, descriptor.type_generation,
             descriptor.codec_id, descriptor.codec_version,
             descriptor.codec_generation, true);
}

std::string ExactCanonicalBooleanJoinAliasRuntimeCarrierV1(
    const api::RelationalTypeDescriptor& descriptor,
    const bool nullable) {
  if (!ExactCanonicalBooleanJoinAliasDescriptorV1(descriptor) ||
      nullable != (descriptor.nullability ==
                   api::RelationalNullability::kNullable)) {
    return {};
  }
  return "datatype_descriptor_uuid=" + descriptor.descriptor_uuid +
         ";datatype_descriptor_generation=" +
         std::to_string(descriptor.descriptor_generation) +
         ";type_uuid=" + descriptor.type_uuid + ";type_generation=" +
         std::to_string(descriptor.type_generation) + ";codec_id=" +
         descriptor.codec_id + ";codec_version=" +
         std::to_string(descriptor.codec_version) + ";codec_generation=" +
         std::to_string(descriptor.codec_generation) +
         ";null_encoding=1;nullability=" +
         (nullable ? "nullable" : "non_null");
}

bool ProjectCanonicalBooleanJoinAliasRuntimeCarriersV1(
    const std::unordered_map<std::uint32_t,
                             const api::RelationalTypeDescriptor*>&
        descriptors,
    exec::DescriptorBatch* batch,
    std::string* detail) {
  if (batch == nullptr || detail == nullptr) return false;
  for (std::size_t ordinal = 0; ordinal < batch->columns.size(); ++ordinal) {
    auto& column = batch->columns[ordinal];
    const auto descriptor = descriptors.find(column.descriptor_id);
    if (descriptor == descriptors.end()) {
      *detail = "join input descriptor identity is unresolved";
      return false;
    }
    if (descriptor->second->descriptor_uuid != descriptor->second->type_uuid) {
      continue;
    }
    // The relational DAG tuple was validated against the live datatype
    // registry above. This projection closes the executor-only carrier; it
    // does not infer authority from the persisted descriptor text.
    if (!ExactCanonicalBooleanJoinAliasDescriptorV1(*descriptor->second) ||
        column.descriptor.descriptor_uuid.canonical !=
            descriptor->second->descriptor_uuid ||
        column.descriptor.descriptor_kind != "scalar" ||
        column.descriptor.canonical_type_name != "boolean" ||
        column.nullable !=
            (descriptor->second->nullability ==
             api::RelationalNullability::kNullable)) {
      *detail =
          "join descriptor and type identity domains are not independent";
      return false;
    }
    const auto encoded = ExactCanonicalBooleanJoinAliasRuntimeCarrierV1(
        *descriptor->second, column.nullable);
    if (encoded.empty()) {
      *detail = "join canonical boolean alias carrier is unresolved";
      return false;
    }
    for (auto& row : batch->rows) {
      if (ordinal >= row.values.size() ||
          row.values[ordinal].descriptor.descriptor_uuid.canonical !=
              column.descriptor.descriptor_uuid.canonical ||
          row.values[ordinal].descriptor.descriptor_kind !=
              column.descriptor.descriptor_kind ||
          row.values[ordinal].descriptor.canonical_type_name !=
              column.descriptor.canonical_type_name ||
          row.values[ordinal].descriptor.encoded_descriptor !=
              column.descriptor.encoded_descriptor) {
        *detail = "join canonical boolean row carrier differs from its column";
        return false;
      }
      row.values[ordinal].descriptor.encoded_descriptor = encoded;
    }
    column.descriptor.encoded_descriptor = encoded;
  }
  return true;
}

PreparedJoinRoot PrepareJoinRoot(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& left_node,
    const plan::CanonicalLogicalRelationalNode& right_node,
    MaterializedValues& left,
    MaterializedValues& right,
    const exec::CanonicalAcceptedJoinKind join_kind) {
  PreparedJoinRoot result;
  std::vector<std::uint32_t> predicate_descriptors =
      left_node.output_descriptor_ids;
  predicate_descriptors.insert(predicate_descriptors.end(),
                               right_node.output_descriptor_ids.begin(),
                               right_node.output_descriptor_ids.end());
  std::vector<std::uint32_t> expected_descriptors =
      left_node.output_descriptor_ids;
  const bool left_only =
      join_kind == exec::CanonicalAcceptedJoinKind::kLeftSemi ||
      join_kind == exec::CanonicalAcceptedJoinKind::kLeftAnti;
  if (!left_only) {
    expected_descriptors.insert(expected_descriptors.end(),
                                right_node.output_descriptor_ids.begin(),
                                right_node.output_descriptor_ids.end());
  }
  if (root.output_descriptor_ids.empty() ||
      root.output_descriptor_ids != expected_descriptors ||
      left.result_bindings.size() != left.batch.columns.size() ||
      right.result_bindings.size() != right.batch.columns.size()) {
    result.detail = "join output does not preserve its accepted bound shape";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail = "join root output lineage is not admitted by this profile";
    return result;
  }
  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }
  std::unordered_set<std::string_view> join_descriptor_uuids;
  std::unordered_set<std::string_view> join_type_uuids;
  for (const auto descriptor_id : predicate_descriptors) {
    const auto descriptor = descriptors.find(descriptor_id);
    if (descriptor == descriptors.end() ||
        descriptor->second->descriptor_uuid.empty() ||
        descriptor->second->type_uuid.empty()) {
      result.detail = "join input descriptor or type identity is unresolved";
      return result;
    }
    join_descriptor_uuids.insert(descriptor->second->descriptor_uuid);
    join_type_uuids.insert(descriptor->second->type_uuid);
  }
  if (std::ranges::any_of(join_descriptor_uuids,
                          [&](const auto descriptor_uuid) {
                            if (!join_type_uuids.contains(descriptor_uuid)) {
                              return false;
                            }
                            return std::ranges::any_of(
                                predicate_descriptors,
                                [&](const auto descriptor_id) {
                                  const auto descriptor =
                                      descriptors.find(descriptor_id);
                                  return descriptor == descriptors.end() ||
                                         ((descriptor->second
                                                   ->descriptor_uuid ==
                                               descriptor_uuid ||
                                           descriptor->second->type_uuid ==
                                               descriptor_uuid) &&
                                          !ExactCanonicalBooleanJoinAliasDescriptorV1(
                                              *descriptor->second));
                                });
                          })) {
    result.detail =
        "join descriptor and type identity domains are not independent";
    return result;
  }
  if (!ProjectCanonicalBooleanJoinAliasRuntimeCarriersV1(
          descriptors, &left.batch, &result.detail) ||
      !ProjectCanonicalBooleanJoinAliasRuntimeCarriersV1(
          descriptors, &right.batch, &result.detail)) {
    return result;
  }
  const bool left_null_extended =
      join_kind == exec::CanonicalAcceptedJoinKind::kRightOuter ||
      join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter;
  const bool right_null_extended =
      join_kind == exec::CanonicalAcceptedJoinKind::kLeftOuter ||
      join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter;
  if ((left_null_extended &&
       std::ranges::any_of(left.batch.columns,
                           [](const auto& column) {
                             return !column.nullable;
                           })) ||
      (right_null_extended &&
       std::ranges::any_of(right.batch.columns,
                           [](const auto& column) {
                             return !column.nullable;
                           }))) {
    result.detail =
        "outer join result lacks an exact nullable descriptor carrier";
    return result;
  }

  if (join_kind != exec::CanonicalAcceptedJoinKind::kCross) {
    if (root.bound_expression_ids.size() != 1) {
      result.detail = "join predicate root identity is not exact";
      return result;
    }
    result.predicate_expression_id = root.bound_expression_ids.front();
    if (!PrepareInputRowBindingForComposition(
            dag, result.predicate_expression_id, predicate_descriptors,
            &result.predicate_row_binding, &result.detail)) {
      if (result.detail ==
          "row expression input descriptor identity is ambiguous") {
        result.detail = "join predicate input descriptor identity is ambiguous";
      } else if (result.detail ==
                 "row expression identifier is not supplied by its input") {
        result.detail =
            "join predicate identifier is not supplied by either input";
      } else if (result.detail ==
                 "row expression has a dangling expression child") {
        result.detail = "join predicate has a dangling expression child";
      }
      return result;
    }
  }

  std::size_t published_ordinal = 0;
  const auto append_bindings =
      [&](const std::vector<exec::CanonicalResultColumnBinding>& bindings,
          const std::size_t physical_base,
          const bool force_nullable) {
    for (const auto& source : bindings) {
      auto binding = source;
      binding.physical_column_ordinal += physical_base;
      if (binding.visible) {
        if (!binding.published_descriptor.has_value()) return false;
        binding.published_descriptor->ordinal =
            static_cast<std::uint32_t>(published_ordinal++);
        if (force_nullable) {
          binding.published_descriptor->nullability =
              exec::CanonicalResultNullability::kNullable;
        }
      }
      result.result_bindings.push_back(std::move(binding));
    }
    return true;
  };
  if (!append_bindings(left.result_bindings, 0, left_null_extended) ||
      (!left_only &&
       !append_bindings(right.result_bindings, left.batch.columns.size(),
                        right_null_extended))) {
    result.result_bindings.clear();
    result.detail = "join visible result binding is incomplete";
    return result;
  }
  result.ok = true;
  return result;
}

LiveSetOperationProfile MatchLiveSetOperationProfile(
    const std::string_view semantic_variant_id) {
  LiveSetOperationProfile profile;
  constexpr std::string_view kPrefix = "set-operation.";
  constexpr std::string_view kSuffix = ".v1";
  if (!semantic_variant_id.starts_with(kPrefix) ||
      !semantic_variant_id.ends_with(kSuffix) ||
      semantic_variant_id.size() <= kPrefix.size() + kSuffix.size()) {
    return profile;
  }
  const auto payload = semantic_variant_id.substr(
      kPrefix.size(),
      semantic_variant_id.size() - kPrefix.size() - kSuffix.size());
  const auto first_modifier = payload.find('.');
  const auto base = payload.substr(0, first_modifier);
  std::string operation_component;
  std::string quantifier_component;
  if (base == "union-all") {
    profile.operation = exec::CanonicalSetOperationKind::kUnion;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kAll;
    operation_component = "union";
    quantifier_component = "all";
    profile.operation_name = "UNION ALL";
  } else if (base == "union-distinct") {
    profile.operation = exec::CanonicalSetOperationKind::kUnion;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kDistinct;
    operation_component = "union";
    quantifier_component = "distinct";
    profile.operation_name = "UNION DISTINCT";
  } else if (base == "intersect-all") {
    profile.operation = exec::CanonicalSetOperationKind::kIntersect;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kAll;
    operation_component = "intersect";
    quantifier_component = "all";
    profile.operation_name = "INTERSECT ALL";
  } else if (base == "intersect-distinct") {
    profile.operation = exec::CanonicalSetOperationKind::kIntersect;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kDistinct;
    operation_component = "intersect";
    quantifier_component = "distinct";
    profile.operation_name = "INTERSECT DISTINCT";
  } else if (base == "except-all") {
    profile.operation = exec::CanonicalSetOperationKind::kExcept;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kAll;
    operation_component = "except";
    quantifier_component = "all";
    profile.operation_name = "EXCEPT ALL";
  } else if (base == "except-distinct") {
    profile.operation = exec::CanonicalSetOperationKind::kExcept;
    profile.quantifier = exec::CanonicalSetOperationQuantifier::kDistinct;
    operation_component = "except";
    quantifier_component = "distinct";
    profile.operation_name = "EXCEPT DISTINCT";
  } else {
    return profile;
  }

  bool by_name = false;
  bool type_reconciled = false;
  bool null_collation = false;
  std::size_t modifier_begin = first_modifier;
  while (modifier_begin != std::string_view::npos) {
    ++modifier_begin;
    const auto modifier_end = payload.find('.', modifier_begin);
    const auto modifier = payload.substr(
        modifier_begin,
        modifier_end == std::string_view::npos
            ? std::string_view::npos
            : modifier_end - modifier_begin);
    if (modifier == "by-name" && !by_name && !type_reconciled &&
        !null_collation) {
      by_name = true;
    } else if (modifier == "type-reconciled" && !type_reconciled &&
               !null_collation) {
      type_reconciled = true;
    } else if (modifier == "null-collation" && !null_collation) {
      null_collation = true;
    } else {
      return {};
    }
    modifier_begin = modifier_end;
  }

  profile.alignment =
      by_name ? exec::CanonicalSetOperationAlignment::kByName
              : exec::CanonicalSetOperationAlignment::kOrdinal;
  profile.type_profile =
      type_reconciled
          ? exec::CanonicalSetOperationTypeProfile::kLosslessImplicit
          : exec::CanonicalSetOperationTypeProfile::kExact;
  profile.equality_profile =
      null_collation
          ? exec::CanonicalSetOperationEqualityProfile::kNullEqualBoundCollation
          : exec::CanonicalSetOperationEqualityProfile::kExactTyped;
  profile.implementation_id = "setop." + operation_component + "-" +
                              quantifier_component + "." +
                              (by_name ? "by-name" : "ordinal");
  if (type_reconciled) {
    profile.implementation_id += ".type-reconciled";
  }
  if (null_collation) {
    profile.implementation_id += ".null-collation";
  }
  profile.implementation_id += ".typed.v1";
  profile.identity_component = operation_component + "-" +
                               quantifier_component +
                               (by_name ? ".by-name" : ".ordinal") +
                               (type_reconciled ? ".type-reconciled" : "") +
                               (null_collation ? ".null-collation" : "");
  profile.physical_semantic_id =
      "canonical.setop." + profile.identity_component + ".v1";
  if (by_name) profile.operation_name += " BY NAME";
  profile.matched = true;
  return profile;
}

PreparedSetOperationRoot PrepareSetOperationRoot(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const MaterializedValues& left,
    const MaterializedValues& right,
    const LiveSetOperationProfile& profile) {
  PreparedSetOperationRoot result;
  if (!profile.matched || root.output_descriptor_ids.empty() ||
      left.batch.columns.size() != root.output_descriptor_ids.size() ||
      right.batch.columns.size() != root.output_descriptor_ids.size() ||
      left.result_bindings.size() != root.output_descriptor_ids.size() ||
      right.result_bindings.size() != root.output_descriptor_ids.size()) {
    result.detail = "set-operation input/result arity is inconsistent";
    return result;
  }
  if (std::ranges::any_of(dag.outputs, [&](const auto& output) {
        return output.relation_node_id == root.logical_node_id;
      })) {
    result.detail =
        "set-operation root output lineage is not admitted by this profile";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }
  std::unordered_set<std::string_view> set_operation_type_uuids;
  const auto collect_type_uuid = [&](const std::uint32_t descriptor_id) {
    const auto descriptor = descriptors.find(descriptor_id);
    if (descriptor == descriptors.end() ||
        descriptor->second->type_uuid.empty()) {
      return false;
    }
    set_operation_type_uuids.insert(descriptor->second->type_uuid);
    return true;
  };
  for (const auto descriptor_id : root.output_descriptor_ids) {
    if (!collect_type_uuid(descriptor_id)) {
      result.detail = "set-operation result type identity is unresolved";
      return result;
    }
  }
  for (const auto& column : left.batch.columns) {
    if (!collect_type_uuid(column.descriptor_id)) {
      result.detail = "set-operation left type identity is unresolved";
      return result;
    }
  }
  for (const auto& column : right.batch.columns) {
    if (!collect_type_uuid(column.descriptor_id)) {
      result.detail = "set-operation right type identity is unresolved";
      return result;
    }
  }

  std::unordered_map<std::string, std::size_t> right_name_ordinals;
  if (profile.alignment == exec::CanonicalSetOperationAlignment::kByName) {
    std::unordered_set<std::string> left_names;
    for (const auto& column : left.batch.columns) {
      if (column.stable_name.empty() ||
          !left_names.insert(column.stable_name).second) {
        result.detail = "set-operation BY NAME left names are not unique";
        return result;
      }
    }
    for (std::size_t column = 0; column < right.batch.columns.size();
         ++column) {
      if (right.batch.columns[column].stable_name.empty() ||
          !right_name_ordinals
               .emplace(right.batch.columns[column].stable_name, column)
               .second) {
        result.detail = "set-operation BY NAME right names are not unique";
        return result;
      }
    }
    if (left_names.size() != right_name_ordinals.size()) {
      result.detail = "set-operation BY NAME column sets differ";
      return result;
    }
  }

  const auto descriptor_has_type_uuid = [](const api::EngineDescriptor& value,
                                           const std::string_view type_uuid) {
    const auto token = "type_uuid=" + std::string(type_uuid);
    return value.encoded_descriptor == token ||
           value.encoded_descriptor.starts_with(token + ";");
  };
  std::size_t published_ordinal = 0;
  for (std::size_t column = 0; column < root.output_descriptor_ids.size();
       ++column) {
    const auto descriptor = descriptors.find(root.output_descriptor_ids[column]);
    const auto& left_column = left.batch.columns[column];
    std::size_t right_column_ordinal = column;
    if (profile.alignment == exec::CanonicalSetOperationAlignment::kByName) {
      const auto right_ordinal =
          right_name_ordinals.find(left_column.stable_name);
      if (right_ordinal == right_name_ordinals.end()) {
        result.detail = "set-operation BY NAME column sets differ";
        return result;
      }
      right_column_ordinal = right_ordinal->second;
    }
    const auto& right_column = right.batch.columns[right_column_ordinal];
    if (descriptor == descriptors.end() ||
        set_operation_type_uuids.contains(
            descriptor->second->descriptor_uuid) ||
        descriptor->second->nullability ==
            api::RelationalNullability::kUnknown ||
        left_column.descriptor.canonical_type_name.empty() ||
        right_column.descriptor.canonical_type_name.empty()) {
      result.detail = "set-operation descriptor reconciliation is unresolved";
      return result;
    }

    std::string result_type_name;
    if (descriptor_has_type_uuid(left_column.descriptor,
                                 descriptor->second->type_uuid)) {
      result_type_name = left_column.descriptor.canonical_type_name;
    } else if (descriptor_has_type_uuid(right_column.descriptor,
                                        descriptor->second->type_uuid)) {
      result_type_name = right_column.descriptor.canonical_type_name;
    } else if (left_column.descriptor.canonical_type_name ==
               right_column.descriptor.canonical_type_name) {
      result_type_name = left_column.descriptor.canonical_type_name;
    } else {
      result.detail =
          "set-operation common result type is not bound to either input";
      return result;
    }

    api::EngineDescriptor engine_descriptor;
    engine_descriptor.descriptor_uuid.canonical =
        descriptor->second->descriptor_uuid;
    engine_descriptor.descriptor_kind = "scalar";
    engine_descriptor.canonical_type_name = result_type_name;
    engine_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->second->type_uuid + ";nullability=" +
        (descriptor->second->nullability ==
                 api::RelationalNullability::kNullable
             ? "nullable"
             : "non_null");
    if (descriptor->second->collation_uuid.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";collation_uuid=" + *descriptor->second->collation_uuid;
    }
    if (descriptor->second->timezone_profile_id.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";timezone_profile_id=" + *descriptor->second->timezone_profile_id;
    }
    if (descriptor->second->width.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";width=" + std::to_string(*descriptor->second->width);
    }
    if (descriptor->second->precision.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";precision=" + std::to_string(*descriptor->second->precision);
    }
    if (descriptor->second->scale.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";scale=" + std::to_string(*descriptor->second->scale);
    }
    const bool nullable = descriptor->second->nullability ==
                          api::RelationalNullability::kNullable;
    if (nullable != (left_column.nullable || right_column.nullable)) {
      result.detail =
          "set-operation result nullability does not cover both inputs";
      return result;
    }
    if (profile.type_profile ==
            exec::CanonicalSetOperationTypeProfile::kExact &&
        (engine_descriptor.canonical_type_name !=
             left_column.descriptor.canonical_type_name ||
         engine_descriptor.canonical_type_name !=
             right_column.descriptor.canonical_type_name ||
         engine_descriptor.encoded_descriptor !=
             left_column.descriptor.encoded_descriptor ||
         engine_descriptor.encoded_descriptor !=
             right_column.descriptor.encoded_descriptor)) {
      result.detail =
          "set-operation exact descriptors do not share one bound type";
      return result;
    }

    const bool character_result_type =
        dt::CanonicalTypeIdFromStableName(result_type_name) ==
        dt::CanonicalTypeId::character;
    if (character_result_type &&
        profile.equality_profile ==
            exec::CanonicalSetOperationEqualityProfile::kNullEqualBoundCollation) {
      if (!descriptor->second->collation_uuid.has_value()) {
        result.detail =
            "set-operation character equality lacks a bound collation";
        return result;
      }
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
      (void)context;
      result.detail =
          "set-operation character equality requires the production engine "
          "resource catalog";
      return result;
#else
      api::EngineUuid collation_uuid;
      collation_uuid.canonical = *descriptor->second->collation_uuid;
      const auto resolved = api::LookupEngineResourceDescriptorByUuid(
          context, collation_uuid, "collation");
      if (!resolved.ok || !resolved.resource_descriptor.present ||
          resolved.resource_descriptor.resource_uuid.canonical !=
              collation_uuid.canonical) {
        result.detail =
            "set-operation character equality lacks current engine "
            "collation authority";
        return result;
      }
      exec::CanonicalSetOperationCollationBinding binding;
      binding.result_column = column;
      binding.collation_uuid = collation_uuid.canonical;
      binding.resource_epoch =
          resolved.resource_descriptor.resource_epoch;
      binding.collation_epoch = resolved.resource_descriptor.family_epoch;
      binding.text_seed.active = true;
      binding.text_seed.seed_pack_name =
          resolved.resource_descriptor.seed_pack_name;
      binding.text_seed.seed_pack_version =
          resolved.resource_descriptor.seed_pack_version;
      binding.text_seed.charset_name =
          resolved.resource_descriptor.parent_canonical_name;
      binding.text_seed.collation_name =
          resolved.resource_descriptor.canonical_name;
      binding.text_seed.collation_case_insensitive =
          resolved.resource_descriptor.case_insensitive;
      binding.text_seed.collation_accent_insensitive =
          resolved.resource_descriptor.accent_insensitive;
      result.collation_bindings.push_back(std::move(binding));
#endif
    } else if (descriptor->second->collation_uuid.has_value() &&
               !character_result_type) {
      result.detail = "set-operation collation targets a non-character type";
      return result;
    }

    result.result_columns.push_back(
        {left_column.stable_name, engine_descriptor,
         nullable,
         descriptor->second->descriptor_id});
    auto binding = left.result_bindings[column];
    if (binding.visible) {
      binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
          static_cast<std::uint32_t>(published_ordinal++),
          left_column.stable_name,
          descriptor->second->descriptor_uuid,
          descriptor->second->type_uuid,
          ResultNullability(descriptor->second->nullability),
          descriptor->second->collation_uuid,
          descriptor->second->timezone_profile_id};
    }
    result.result_bindings.push_back(std::move(binding));
  }
  result.ok = true;
  return result;
}

}  // namespace

PreparedJoinRoot PrepareJoinRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& left_node,
    const plan::CanonicalLogicalRelationalNode& right_node,
    MaterializedValues& left,
    MaterializedValues& right,
    const exec::CanonicalAcceptedJoinKind join_kind) {
  return PrepareJoinRoot(
      dag, root, left_node, right_node, left, right, join_kind);
}

LiveSetOperationProfile ResolveLiveSetOperationProfileForComposition(
    const std::string_view semantic_variant_id) {
  return MatchLiveSetOperationProfile(semantic_variant_id);
}

PreparedSetOperationRoot PrepareSetOperationRootForComposition(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const MaterializedValues& left,
    const MaterializedValues& right,
    const LiveSetOperationProfile& profile) {
  return PrepareSetOperationRoot(context, dag, root, left, right, profile);
}

bool BoundSetOperationEqualityComparisonsForComposition(
    const PreparedSetOperationRoot& prepared,
    const LiveSetOperationProfile& profile,
    const std::uint64_t maximum_input_row_count,
    std::uint64_t* comparison_bound,
    std::uint64_t* collation_comparison_count) {
  return BoundSetOperationEqualityComparisons(
      prepared, profile, maximum_input_row_count, comparison_bound,
      collation_comparison_count);
}

LiveSetRegistrationProfiles MakeLiveSetRegistrationProfilesForComposition(
    const std::unordered_map<std::uint64_t, PreparedLiveSetNode>& prepared) {
  return MakeLiveSetRegistrationProfiles(prepared);
}

MaterializedSetOperationPlanningState
MaterializeSetOperationPlanningStateForComposition(
    const PreparedSetOperationRoot& prepared,
    const LiveSetOperationProfile& profile,
    const MaterializedValues& left,
    const MaterializedValues& right) {
  return MaterializeSetOperationPlanningState(
      prepared, profile, left, right);
}

}  // namespace scratchbird::engine::sblr
