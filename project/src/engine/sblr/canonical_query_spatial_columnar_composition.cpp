// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_spatial_columnar_composition.hpp"

#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_join_registration.hpp"
#include "canonical_query_model_family_composition_support.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_projection_registration.hpp"
#include "canonical_query_relational_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_relational_expression.hpp"
#include "sblr_dispatch.hpp"

#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "engine/executor/executor_foundation.hpp"
#include "engine/executor/model_family_executor.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_descriptor.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_store.hpp"
#include "engine/optimizer/model_family_coordinator.hpp"
#include "engine/optimizer/model_family_profile_factory.hpp"
#include "engine/optimizer/optimizer_contract.hpp"
#include "engine/optimizer/relational_planner.hpp"
#include "nosql/columnar_api.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "nosql/spatial_api.hpp"
#include "query/canonical_heap_optimizer_admission.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <numeric>
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
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_SPATIAL_COLUMNAR_COMPOSITION_AUTHORITY
// Owns admitted production spatial and columnar source routes, their exact
// two-source columnar join, and contextual-TEXT columnar-filter admission.
// It only consumes engine-issued MGA statement authority and cannot begin,
// commit, roll back, recover, or manufacture a transaction snapshot.

void BindCanonicalPersistedRowDescriptorAuthorityForSpatialColumnarV1(
    const api::EngineRequestContext& context,
    CanonicalRelationalExpressionRuntimeServices* services) {
  if (services == nullptr) return;
  services->persisted_row_descriptor_authority =
      [context = &context](
          const std::uint32_t,
          const api::RelationalTypeDescriptor& bound,
          const api::EngineDescriptor& persisted,
          const api::RelationalNullability effective_nullability,
          std::string* refusal_detail) {
        return ValidateCanonicalPersistedTextRowDescriptorAuthorityV1(
            *context, bound, persisted, effective_nullability,
            refusal_detail);
      };
}

using PersistedRowDescriptorAuthorityCallbackV1 = std::function<bool(
    std::uint32_t,
    const api::RelationalTypeDescriptor&,
    const api::EngineDescriptor&,
    api::RelationalNullability,
    std::string*)>;

bool MatchesExactBoundRelationalDescriptorFieldsV2(
    const std::array<std::string, 17>& fields,
    const api::RelationalTypeDescriptor& bound,
    const api::RelationalNullability effective_nullability) {
  std::array<char, 32> numeric{};
  const auto exact_u64 = [&](const std::string& actual,
                             const std::uint64_t expected) {
    const auto encoded = std::to_chars(numeric.data(),
                                       numeric.data() + numeric.size(),
                                       expected);
    return encoded.ec == std::errc{} &&
           actual.size() ==
               static_cast<std::size_t>(encoded.ptr - numeric.data()) &&
           std::equal(actual.begin(), actual.end(), numeric.begin());
  };
  const auto exact_optional_u32 = [&](const std::string& actual,
                                      const std::optional<std::uint32_t>& value) {
    return value.has_value() ? exact_u64(actual, *value) : actual == "-";
  };
  const auto exact_optional_string = [](
                                         const std::string& actual,
                                         const std::optional<std::string>& value) {
    return value.has_value() ? actual == *value : actual == "-";
  };
  return fields[0] == bound.descriptor_uuid &&
         exact_u64(fields[1], bound.descriptor_generation) &&
         fields[2] == bound.type_uuid &&
         exact_u64(fields[3], bound.type_generation) &&
         fields[4] == bound.codec_id &&
         exact_u64(fields[5], bound.codec_version) &&
         exact_u64(fields[6], bound.codec_generation) &&
         fields[7] ==
             (effective_nullability == api::RelationalNullability::kNullable
                  ? "1"
                  : "0") &&
         exact_optional_string(fields[8], bound.collation_uuid) &&
         exact_optional_string(fields[9], bound.timezone_profile_id) &&
         exact_optional_u32(fields[10], bound.width) &&
         exact_optional_u32(fields[11], bound.precision) &&
         exact_optional_u32(fields[12], bound.scale) &&
         fields[13] == bound.statement_receipt_uuid &&
         fields[14] == bound.datatype_catalog_snapshot_uuid &&
         exact_u64(fields[15], bound.datatype_catalog_generation) &&
         exact_u64(fields[16], bound.datatype_registry_generation);
}

struct ContextualTextDirectRouteTargetV2 {
  std::uint32_t descriptor_handle{0};
  std::size_t row_ordinal{0};
  api::RelationalNullability effective_nullability{
      api::RelationalNullability::kUnknown};
  api::RelationalTypeDescriptor bound_descriptor;
  api::EngineDescriptor persisted_descriptor;
};

struct ContextualTextDirectRouteEqualityV2 {
  std::uint64_t literal_occurrence{0};
  std::uint64_t node_id{0};
  std::uint32_t literal_expression_id{0};
  std::uint32_t comparison_expression_id{0};
  std::uint32_t target_expression_id{0};
  std::uint32_t source_node_id{0};
  std::uint32_t literal_descriptor_handle{0};
  std::uint32_t target_descriptor_handle{0};
  std::uint8_t literal_argument_ordinal{0};
  std::uint8_t target_argument_ordinal{0};
  ContextualTextUuidV2 equality_operation_uuid{};
  std::uint64_t equality_operation_generation{0};
};

const char* Rcp079ContextualCancellationDiagnosticV2(
    const bool contextual, const bool model_probe_failed) noexcept {
  if (contextual) return "PROCESS.CANCELLED";
  return model_probe_failed ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                            : "SB_MODEL_EXECUTION_CANCELLED_V1";
}

bool ActivateContextualTextDirectRouteV2(
    const std::shared_ptr<ContextualTextDispatchActivationV2>& activation,
    std::string* diagnostic_id,
    std::string* refusal_detail) {
  if (diagnostic_id == nullptr || refusal_detail == nullptr) return false;
  diagnostic_id->clear();
  refusal_detail->clear();
  if (!activation || activation->joint_consumed ||
      activation->lease.valid() || !activation->prepared.valid() ||
      activation->joint_consume_locked == nullptr ||
      activation->joint_consume_context == nullptr) {
    *diagnostic_id = "SBLR.CONTEXTUAL_TEXT_LITERAL.REPLAY";
    *refusal_detail =
        "contextual TEXT direct-route activation is absent or replayed";
    return false;
  }
  auto consumed = activation->joint_consume_locked(
      activation->joint_consume_context, &activation->prepared);
  if (!consumed.ok || !consumed.lease.valid()) {
    *diagnostic_id = consumed.diagnostic.code.empty()
                         ? "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE"
                         : consumed.diagnostic.code;
    *refusal_detail = consumed.diagnostic.detail.empty()
                          ? "contextual TEXT joint transition was refused"
                          : consumed.diagnostic.detail;
    return false;
  }
  activation->lease = std::move(consumed.lease);
  activation->joint_consumed = true;
  return true;
}

bool PrepareContextualTextDirectRouteAuthorityV2(
    const std::shared_ptr<ContextualTextDispatchActivationV2>& activation,
    const api::TypedRelationalDag& dag, const std::uint32_t source_node_id,
    const CanonicalRelationalExpressionRowBinding& row_binding,
    const std::vector<std::uint32_t>& logical_descriptor_ids,
    const api::MgaRelationStorageDescriptor& persisted,
    const exec::DescriptorBatch& logical_rows,
    const PersistedRowDescriptorAuthorityCallbackV1& persisted_delegate,
    std::vector<ContextualTextDirectRouteTargetV2>* targets,
    std::vector<ContextualTextDirectRouteEqualityV2>* equalities,
    std::string* diagnostic_id, std::string* refusal_detail) {
  if (targets == nullptr || equalities == nullptr || diagnostic_id == nullptr ||
      refusal_detail == nullptr) {
    return false;
  }
  targets->clear();
  equalities->clear();
  diagnostic_id->clear();
  refusal_detail->clear();
  if (!activation || activation->joint_consumed || activation->lease.valid() ||
      !activation->prepared.valid() || !persisted_delegate) {
    *diagnostic_id = "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE";
    *refusal_detail =
        "contextual TEXT prepared direct-route authority is unavailable";
    return false;
  }
  const auto prepared_entries =
      api::ViewPreparedContextualTextLiteralSetV2(activation->prepared);
  const auto contextual_expression_count = std::ranges::count_if(
      dag.expressions, [](const auto& expression) {
        return expression.contextual_text_literal_v2.has_value();
      });
  if (prepared_entries.empty() ||
      prepared_entries.size() != contextual_expression_count) {
    *diagnostic_id = "SBLR.CONTEXTUAL_TEXT_LITERAL.BINDING_STALE";
    *refusal_detail =
        "contextual TEXT prepared entries differ from the exact graph";
    return false;
  }
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.expression_id == expression_id;
    });
  };
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
      return descriptor.descriptor_id == descriptor_id;
    });
  };
  try {
    targets->reserve(prepared_entries.size());
    equalities->reserve(prepared_entries.size());
  } catch (const std::bad_alloc&) {
    *diagnostic_id = "ENGINE.RESOURCE.EXHAUSTED";
    *refusal_detail =
        "contextual TEXT route-authority allocation was refused";
    return false;
  }
  for (const auto& entry : prepared_entries) {
    const auto& runtime = entry.runtime_materialization;
    const auto& binding = runtime.graph_binding;
    const auto* exact_prepared =
        api::FindPreparedContextualTextExecutionEntryV2(
            activation->prepared, entry.prepared_value.literal_occurrence,
            entry.prepared_value.node_id, binding.literal_expression_id,
            binding.comparison_expression_id, binding.target_expression_id,
            binding.source_node_id, binding.literal_descriptor_handle,
            binding.target_descriptor_handle, entry.literal_argument_ordinal,
            entry.target_argument_ordinal, entry.equality_operation_uuid,
            entry.equality_operation_generation);
    const auto literal = expression_for(binding.literal_expression_id);
    const auto comparison = expression_for(binding.comparison_expression_id);
    const auto target = expression_for(binding.target_expression_id);
    const auto literal_descriptor =
        descriptor_for(binding.literal_descriptor_handle);
    const auto target_descriptor =
        descriptor_for(binding.target_descriptor_handle);
    const bool argument_ordinals_exact =
        (entry.literal_argument_ordinal == 1 ||
         entry.literal_argument_ordinal == 2) &&
        (entry.target_argument_ordinal == 1 ||
         entry.target_argument_ordinal == 2) &&
        entry.literal_argument_ordinal != entry.target_argument_ordinal;
    const bool literal_is_left = entry.literal_argument_ordinal == 1;
    const bool exact_comparison_children =
        comparison != dag.expressions.end() &&
        comparison->child_expression_ids.size() == 2 &&
        comparison->child_expression_ids[literal_is_left ? 0 : 1] ==
            binding.literal_expression_id &&
        comparison->child_expression_ids[literal_is_left ? 1 : 0] ==
            binding.target_expression_id;
    const bool literal_body_matches =
        runtime.value.encoded_value.size() ==
            entry.prepared_value.canonical_body.size() &&
        std::equal(runtime.value.encoded_value.begin(),
                   runtime.value.encoded_value.end(),
                   entry.prepared_value.canonical_body.begin(),
                   [](const char left, const std::uint8_t right) {
                     return static_cast<std::uint8_t>(
                                static_cast<unsigned char>(left)) == right;
                   });
    if (exact_prepared != &entry ||
        binding.source_node_id != source_node_id ||
        entry.comparison_occurrence != binding.comparison_expression_id ||
        entry.target_descriptor_handle != binding.target_descriptor_handle ||
        !argument_ordinals_exact ||
        literal == dag.expressions.end() ||
        literal->expression_kind != api::RelationalExpressionKind::kLiteral ||
        literal->literal_kind != api::RelationalLiteralKind::kString ||
        literal->result_descriptor_id != binding.literal_descriptor_handle ||
        !literal->contextual_text_literal_v2.has_value() ||
        literal->contextual_text_literal_v2->literal_occurrence !=
            entry.prepared_value.literal_occurrence ||
        literal->contextual_text_literal_v2->node_id !=
            entry.prepared_value.node_id ||
        literal->contextual_text_literal_v2->literal_descriptor_handle !=
            binding.literal_descriptor_handle ||
        comparison == dag.expressions.end() ||
        comparison->expression_kind !=
            api::RelationalExpressionKind::kBinary ||
        comparison->operator_name != std::optional<std::string>("=") ||
        !exact_comparison_children || target == dag.expressions.end() ||
        target->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        target->result_descriptor_id != binding.target_descriptor_handle ||
        literal_descriptor == dag.descriptors.end() ||
        target_descriptor == dag.descriptors.end() ||
        !literal_descriptor->datatype_identity_authoritative ||
        !target_descriptor->datatype_identity_authoritative ||
        !MatchesExactBoundRelationalDescriptorFieldsV2(
            binding.exact_relational_descriptor_v2_fields,
            *literal_descriptor, literal_descriptor->nullability) ||
        !MatchesExactBoundRelationalDescriptorFieldsV2(
            binding.exact_target_relational_descriptor_v2_fields,
            *target_descriptor, target_descriptor->nullability) ||
        binding.canonical_type_name != "text" ||
        !binding.element_profile_empty ||
        binding.target_canonical_type_name != "text" ||
        !binding.target_element_profile_empty ||
        runtime.value.state != api::EngineValueState::value ||
        runtime.value.is_null || !runtime.value.binary_value.empty() ||
        !literal_body_matches ||
        runtime.value.descriptor.descriptor_uuid !=
            binding.exact_relational_descriptor_v2_fields[0] ||
        runtime.value.descriptor.canonical_type_name != "text") {
      *diagnostic_id = "SBLR.CONTEXTUAL_TEXT_LITERAL.TARGET_MISMATCH";
      *refusal_detail =
          "contextual TEXT prepared entry differs from its exact graph";
      targets->clear();
      equalities->clear();
      return false;
    }

    const CanonicalRelationalExpressionRowSlotBinding* target_slot = nullptr;
    for (const auto& slot : row_binding.slots) {
      if (slot.expression_id != binding.target_expression_id) continue;
      if (target_slot != nullptr) {
        *diagnostic_id = "SBLR.CONTEXTUAL_TEXT_LITERAL.TARGET_MISMATCH";
        *refusal_detail =
            "contextual TEXT target row occurrence is ambiguous";
        targets->clear();
        equalities->clear();
        return false;
      }
      target_slot = &slot;
    }
    if (target_slot == nullptr ||
        target_slot->slot_kind !=
            CanonicalRelationalExpressionRowSlotKind::input_identifier ||
        target_slot->descriptor_id != binding.target_descriptor_handle ||
        target_slot->row_ordinal >= logical_descriptor_ids.size() ||
        target_slot->row_ordinal >= persisted.columns.size() ||
        target_slot->row_ordinal >= logical_rows.columns.size() ||
        logical_descriptor_ids[target_slot->row_ordinal] !=
            binding.target_descriptor_handle ||
        logical_rows.columns[target_slot->row_ordinal].descriptor_id !=
            binding.target_descriptor_handle) {
      *diagnostic_id = "SBLR.CONTEXTUAL_TEXT_LITERAL.TARGET_MISMATCH";
      *refusal_detail =
          "contextual TEXT target handle is not its exact input column";
      targets->clear();
      equalities->clear();
      return false;
    }
    const auto effective_nullability =
        row_binding.row_nullable.empty()
            ? target_descriptor->nullability
            : (row_binding.row_nullable[target_slot->row_ordinal]
                   ? api::RelationalNullability::kNullable
                   : api::RelationalNullability::kNonNull);
    const auto& persisted_descriptor =
        logical_rows.columns[target_slot->row_ordinal].descriptor;
    auto expected_live_descriptor = runtime.target_descriptor;
    expected_live_descriptor.descriptor_uuid =
        target_descriptor->descriptor_uuid;
    auto expected_persisted_descriptor =
        persisted.columns[target_slot->row_ordinal].value_descriptor;
    expected_persisted_descriptor.descriptor_kind = "scalar";
    if (!SameExactEngineDescriptorV1(
            runtime.target_descriptor, expected_persisted_descriptor) ||
        !SameExactEngineDescriptorV1(persisted_descriptor,
                                     expected_live_descriptor)) {
      *diagnostic_id = "CTB.TEXT.DESCRIPTOR_INVALID";
      *refusal_detail = "contextual TEXT target descriptor is stale";
      targets->clear();
      equalities->clear();
      return false;
    }
    const auto existing_target = std::ranges::find_if(
        *targets, [&](const auto& candidate) {
          return candidate.descriptor_handle ==
                 binding.target_descriptor_handle;
        });
    if (existing_target == targets->end()) {
      std::string persisted_detail;
      if (!persisted_delegate(binding.target_descriptor_handle,
                              *target_descriptor,
                              expected_persisted_descriptor,
                              effective_nullability, &persisted_detail)) {
        *diagnostic_id = "CTB.TEXT.DESCRIPTOR_INVALID";
        *refusal_detail = persisted_detail.empty()
                              ? "contextual TEXT target descriptor is stale"
                              : std::move(persisted_detail);
        targets->clear();
        equalities->clear();
        return false;
      }
      try {
        targets->push_back(
            {binding.target_descriptor_handle, target_slot->row_ordinal,
             effective_nullability, *target_descriptor,
             persisted_descriptor});
      } catch (const std::bad_alloc&) {
        *diagnostic_id = "ENGINE.RESOURCE.EXHAUSTED";
        *refusal_detail =
            "contextual TEXT target-record allocation was refused";
        targets->clear();
        equalities->clear();
        return false;
      }
    } else if (existing_target->row_ordinal != target_slot->row_ordinal ||
               existing_target->effective_nullability !=
                   effective_nullability ||
               !SameExactRelationalTypeDescriptorV2(
                   existing_target->bound_descriptor, *target_descriptor) ||
               !SameExactEngineDescriptorV1(
                   existing_target->persisted_descriptor,
                   persisted_descriptor)) {
      *diagnostic_id = "SBLR.CONTEXTUAL_TEXT_LITERAL.TARGET_MISMATCH";
      *refusal_detail =
          "contextual TEXT target handle crossed prepared authorities";
      targets->clear();
      equalities->clear();
      return false;
    }
    if (std::ranges::any_of(*equalities, [&](const auto& candidate) {
          return candidate.comparison_expression_id ==
                     binding.comparison_expression_id ||
                 candidate.literal_expression_id ==
                     binding.literal_expression_id;
        })) {
      *diagnostic_id = "SBLR.CONTEXTUAL_TEXT_LITERAL.TARGET_MISMATCH";
      *refusal_detail =
          "contextual TEXT equality occurrence is duplicated";
      targets->clear();
      equalities->clear();
      return false;
    }
    equalities->push_back(
        {entry.prepared_value.literal_occurrence,
         entry.prepared_value.node_id,
         binding.literal_expression_id,
         binding.comparison_expression_id,
         binding.target_expression_id,
         binding.source_node_id,
         binding.literal_descriptor_handle,
         binding.target_descriptor_handle,
         entry.literal_argument_ordinal,
         entry.target_argument_ordinal,
         entry.equality_operation_uuid,
         entry.equality_operation_generation});
  }
  if (targets->empty() || equalities->empty()) {
    *diagnostic_id = "SBLR.CONTEXTUAL_TEXT_LITERAL.TARGET_MISMATCH";
    *refusal_detail = "contextual TEXT direct-route authority is empty";
    return false;
  }
  return true;
}

std::optional<bool> MatchPrevalidatedContextualTextTargetV2(
    const std::vector<ContextualTextDirectRouteTargetV2>& targets,
    const std::uint32_t bound_descriptor_id,
    const api::RelationalTypeDescriptor& bound,
    const api::EngineDescriptor& persisted,
    const api::RelationalNullability effective_nullability) {
  const ContextualTextDirectRouteTargetV2* found = nullptr;
  for (const auto& target : targets) {
    if (target.descriptor_handle != bound_descriptor_id) continue;
    if (found != nullptr) return false;
    found = &target;
  }
  if (found == nullptr) return std::nullopt;
  return found->effective_nullability == effective_nullability &&
         SameExactRelationalTypeDescriptorV2(found->bound_descriptor, bound) &&
         SameExactEngineDescriptorV1(found->persisted_descriptor, persisted);
}

const ContextualTextDirectRouteEqualityV2*
FindPrevalidatedContextualTextEqualityV2(
    const std::vector<ContextualTextDirectRouteEqualityV2>& equalities,
    const std::uint32_t comparison_expression_id,
    const std::uint32_t left_expression_id,
    const std::uint32_t right_expression_id,
    bool* ambiguous) noexcept {
  if (ambiguous != nullptr) *ambiguous = false;
  const ContextualTextDirectRouteEqualityV2* found = nullptr;
  for (const auto& candidate : equalities) {
    if (candidate.comparison_expression_id != comparison_expression_id ||
        (candidate.literal_argument_ordinal == 1
             ? (candidate.literal_expression_id != left_expression_id ||
                candidate.target_expression_id != right_expression_id)
             : (candidate.literal_expression_id != right_expression_id ||
                candidate.target_expression_id != left_expression_id))) {
      continue;
    }
    if (found != nullptr) {
      if (ambiguous != nullptr) *ambiguous = true;
      return nullptr;
    }
    found = &candidate;
  }
  return found;
}

bool AuthorizePrevalidatedContextualTextEqualityTypeV2(
    const std::vector<ContextualTextDirectRouteEqualityV2>& equalities,
    const std::uint32_t comparison_expression_id,
    const std::uint32_t left_expression_id,
    const std::uint32_t right_expression_id,
    const std::uint32_t literal_expression_id,
    const std::uint64_t literal_occurrence, const std::uint64_t node_id,
    const std::uint32_t literal_descriptor_handle,
    std::string* refusal_detail) noexcept {
  if (refusal_detail == nullptr) return false;
  refusal_detail->clear();
  bool ambiguous = false;
  const auto* expected = FindPrevalidatedContextualTextEqualityV2(
      equalities, comparison_expression_id, left_expression_id,
      right_expression_id, &ambiguous);
  if (ambiguous || expected == nullptr ||
      expected->literal_expression_id != literal_expression_id ||
      expected->literal_occurrence != literal_occurrence ||
      expected->node_id != node_id ||
      expected->literal_descriptor_handle != literal_descriptor_handle) {
    *refusal_detail =
        "contextual TEXT literal type key differs from its prevalidated equality";
    return false;
  }
  return true;
}

bool EvaluateContextualTextEqualityV2(
    const std::shared_ptr<ContextualTextDispatchActivationV2>& activation,
    const std::vector<ContextualTextDirectRouteEqualityV2>& equalities,
    const std::uint32_t comparison_expression_id,
    const std::uint32_t left_expression_id,
    const std::uint32_t right_expression_id,
    const api::EngineTypedValue& target_value,
    api::EngineSqlTruthValue* truth,
    std::string* diagnostic_id,
    std::string* refusal_detail) {
  if (truth == nullptr || diagnostic_id == nullptr ||
      refusal_detail == nullptr) {
    return false;
  }
  *truth = api::EngineSqlTruthValue::unknown;
  diagnostic_id->clear();
  refusal_detail->clear();
  if (!activation || !activation->joint_consumed ||
      !activation->lease.valid()) {
    *diagnostic_id = "ENGINE.INTERNAL.ERROR";
    *refusal_detail =
        "prevalidated contextual TEXT equality lost its consumed lease";
    return false;
  }
  bool ambiguous = false;
  const auto* expected = FindPrevalidatedContextualTextEqualityV2(
      equalities, comparison_expression_id, left_expression_id,
      right_expression_id, &ambiguous);
  if (ambiguous) {
    *diagnostic_id = "ENGINE.INTERNAL.ERROR";
    *refusal_detail =
        "prevalidated contextual TEXT equality key is ambiguous";
    return false;
  }
  if (expected == nullptr) {
    *diagnostic_id = "ENGINE.INTERNAL.ERROR";
    *refusal_detail =
        "prevalidated contextual TEXT equality key is absent";
    return false;
  }
  const auto* selected = api::FindContextualTextExecutionAuthorityEntryV2(
      activation->lease, expected->literal_expression_id,
      expected->comparison_expression_id, expected->target_expression_id,
      expected->source_node_id, expected->literal_descriptor_handle,
      expected->target_descriptor_handle, expected->equality_operation_uuid,
      expected->equality_operation_generation);
  if (selected == nullptr) {
    *diagnostic_id = "ENGINE.INTERNAL.ERROR";
    *refusal_detail =
        "prevalidated contextual TEXT lease entry was not promoted";
    return false;
  }
  const auto& runtime = selected->runtime_materialization;
  const auto& literal_value = runtime.value;
  const auto& left_value = expected->literal_argument_ordinal == 1
                               ? literal_value
                               : target_value;
  const auto& right_value = expected->literal_argument_ordinal == 2
                                ? literal_value
                                : target_value;
  int comparison = 0;
  if (!left_value.isSqlNull() && !right_value.isSqlNull() &&
      !api::QowCompareCanonicalCollatedScalarsV1(
          left_value, right_value,
          runtime.comparison_resources.collation_uuid_canonical,
          runtime.comparison_resources.collation_resource_epoch,
          runtime.comparison_resources.collation_family_epoch,
          runtime.comparison_resources.text_seed, &comparison,
          refusal_detail)) {
    *diagnostic_id = "QOW-DIAG-QRY-008-COLLATION-REFUSAL-V1";
    return false;
  }
  if (!api::QowEvaluateCanonicalComparisonTruthV1(
          left_value, right_value, comparison,
          api::EngineComparisonPredicateOperator::equal, truth,
          refusal_detail)) {
    *diagnostic_id = "CTB.TEXT.COMPARISON_REFUSED";
    return false;
  }
  return true;
}

bool Rcp079ExactContextualTextDirectRouteCandidateV2(
    const api::TypedRelationalDag& dag) noexcept {
  if (dag.wire_version != 2 || dag.nodes.size() != 1) {
    return false;
  }
  const auto& source = dag.nodes.front();
  if (dag.root_node_id != source.node_id ||
      source.node_kind != api::RelationalDagNodeKind::kScan ||
      source.semantic_variant_id != "SBLR_MODEL_SOURCE_V1" ||
      !source.input_node_ids.empty()) {
    return false;
  }

  constexpr std::array<std::string_view, 3> kOperationOrder{
      "COLUMNAR_SOURCE", "COLUMNAR_FILTER", "COLUMNAR_PROJECT"};
  const auto route_operation_ordinal = [](const std::string_view name)
      -> std::optional<std::size_t> {
    constexpr std::array<std::string_view, 6> kRouteOperations{
        "COLUMNAR_SOURCE", "COLUMNAR_FILTER", "COLUMNAR_PROJECT",
        "SPATIAL_SOURCE", "SPATIAL_MATCH", "SPATIAL_NEAREST"};
    const auto found = std::ranges::find(kRouteOperations, name);
    if (found == kRouteOperations.end()) return std::nullopt;
    return static_cast<std::size_t>(found - kRouteOperations.begin());
  };
  std::array<const api::RelationalExpressionRecord*, 3> bound_operations{};
  std::size_t bound_operation_count = 0;
  for (std::size_t binding_ordinal = 0;
       binding_ordinal < source.bound_expression_ids.size();
       ++binding_ordinal) {
    const auto expression_id =
        source.bound_expression_ids[binding_ordinal];
    if (expression_id == 0) return false;
    for (std::size_t prior = 0; prior < binding_ordinal; ++prior) {
      if (source.bound_expression_ids[prior] == expression_id) return false;
    }
    std::size_t occurrences = 0;
    const api::RelationalExpressionRecord* bound_expression = nullptr;
    for (const auto& expression : dag.expressions) {
      if (expression.expression_id == expression_id) {
        ++occurrences;
        bound_expression = &expression;
      }
    }
    if (occurrences != 1 || bound_expression == nullptr) {
      return false;
    }
    if (!bound_expression->operator_name.has_value() ||
        !route_operation_ordinal(*bound_expression->operator_name).has_value()) {
      continue;
    }
    if (bound_operation_count == bound_operations.size()) return false;
    bound_operations[bound_operation_count++] = bound_expression;
  }
  if (bound_operation_count != 2 && bound_operation_count != 3) return false;
  for (std::size_t ordinal = 0; ordinal < bound_operation_count; ++ordinal) {
    const auto* operation = bound_operations[ordinal];
    if (operation == nullptr ||
        operation->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        operation->operator_name != kOperationOrder[ordinal]) {
      return false;
    }
  }

  for (const auto& expression : dag.expressions) {
    if (!expression.operator_name.has_value() ||
        !route_operation_ordinal(*expression.operator_name).has_value()) {
      continue;
    }
    bool is_exact_bound_operation = false;
    for (std::size_t ordinal = 0; ordinal < bound_operation_count; ++ordinal) {
      if (expression.expression_id ==
              bound_operations[ordinal]->expression_id &&
          expression.operator_name == kOperationOrder[ordinal]) {
        is_exact_bound_operation = true;
        break;
      }
    }
    if (!is_exact_bound_operation) return false;
  }
  return true;
}


}  // namespace

bool IsCanonicalSpatialColumnarContextualRouteCandidate(
    const api::TypedRelationalDag& dag) noexcept {
  return Rcp079ExactContextualTextDirectRouteCandidateV2(dag);
}

struct Rcp079ColumnarJoinSourceV1 {
  std::uint32_t logical_node_id{0};
  std::string object_uuid;
  api::MgaRelationStorageDescriptor persisted;
  std::vector<exec::ExecutorColumnDescriptor> columns;
  std::vector<std::uint32_t> output_descriptor_ids;
  std::vector<std::uint32_t> output_expression_ids;
  std::string implementation_id{"physical_columnar_zone_scan_v1"};
  std::string capability_uuid;
  std::string provider_uuid;
  std::string result_handle_uuid;
  std::string property_uuid;
  std::string security_receipt_uuid;
};

std::optional<std::map<std::string_view, std::string_view>>
Rcp079ExactDescriptorFieldsV1(const api::EngineDescriptor& descriptor) {
  std::map<std::string_view, std::string_view> fields;
  const auto encoded = std::string_view(descriptor.encoded_descriptor);
  std::size_t offset = 0;
  while (offset <= encoded.size()) {
    const auto separator = encoded.find(';', offset);
    const auto field = encoded.substr(
        offset, separator == std::string_view::npos
                    ? std::string_view::npos
                    : separator - offset);
    const auto equal = field.find('=');
    if (field.empty() || equal == std::string_view::npos || equal == 0 ||
        equal + 1 == field.size() ||
        !fields.emplace(field.substr(0, equal), field.substr(equal + 1))
             .second) {
      return std::nullopt;
    }
    if (separator == std::string_view::npos) break;
    offset = separator + 1;
  }
  return fields;
}

struct Rcp079ResolvedDatatypeAuthorityV1 {
  std::string descriptor_uuid;
  std::string type_uuid;
  bool exact_canonical_text{false};
};

std::optional<Rcp079ResolvedDatatypeAuthorityV1>
Rcp079ResolvePersistedDatatypeAuthorityV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationColumnStorageDescriptor& persisted,
    const std::map<std::string_view, std::string_view>& fields) {
  const auto field = [&](const std::string_view name)
      -> std::optional<std::string_view> {
    const auto found = fields.find(name);
    return found == fields.end()
               ? std::optional<std::string_view>{}
               : std::optional<std::string_view>{found->second};
  };
  static const auto datatype_manifest =
      dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto canonical_type_id =
      persisted.value_descriptor.canonical_type_name == "timestamp_tz"
          ? dt::CanonicalTypeId::timestamp
          : dt::CanonicalTypeIdFromStableName(
                persisted.value_descriptor.canonical_type_name);
  const auto canonical_type =
      datatype_manifest.ok()
          ? dt::LookupDatatypeCatalogRow(datatype_manifest.manifest,
                                         canonical_type_id)
          : dt::DatatypeCatalogManifestResult{};
  if (canonical_type_id == dt::CanonicalTypeId::unknown ||
      !canonical_type.ok() ||
      canonical_type.manifest.descriptor_rows.size() != 1 ||
      !canonical_type.manifest.descriptor_rows.front()
           .descriptor_uuid.valid()) {
    return std::nullopt;
  }
  const auto& manifest_row =
      canonical_type.manifest.descriptor_rows.front();
  const auto descriptor_uuid = scratchbird::core::uuid::UuidToString(
      manifest_row.descriptor_uuid.value);
  constexpr std::array<std::string_view, 8> kRegistrySuffixFields{
      "datatype_descriptor_uuid", "datatype_descriptor_generation",
      "type_generation", "codec_uuid", "codec_id", "codec_version",
      "codec_generation", "null_encoding"};
  const auto is_registry_suffix_key = [&](const std::string_view key) {
    return key.starts_with("datatype_") || key == "type_generation" ||
           key.starts_with("codec_") || key == "null_encoding";
  };
  const bool carries_registry_authority =
      std::ranges::any_of(fields, [&](const auto& entry) {
        return is_registry_suffix_key(entry.first);
      });

  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid,
      context.datatype_catalog_generation,
      context.datatype_registry_generation, descriptor_uuid,
      manifest_row.descriptor_epoch);
  if (!identity.ok) {
    const auto registered_identity = dt::LookupDatatypeTypeCodecIdentityV1(
        "019d0000-0000-7000-8000-00000000d701",
        datatype_manifest.manifest.catalog_epoch, 1, descriptor_uuid,
        manifest_row.descriptor_epoch);
    // A live registered identity may not be downgraded to a legacy
    // descriptor/type alias merely because its receipt is stale.  Only rows
    // that have no registered distinct type identity and carry no registry
    // authority retain the legacy descriptor UUID fallback.
    if (registered_identity.ok || carries_registry_authority) {
      return std::nullopt;
    }
    return Rcp079ResolvedDatatypeAuthorityV1{
        descriptor_uuid, descriptor_uuid, false};
  }
  if (identity.row.catalog_snapshot_uuid !=
          context.datatype_catalog_snapshot_uuid ||
      identity.row.catalog_generation !=
          context.datatype_catalog_generation ||
      identity.row.registry_generation !=
          context.datatype_registry_generation ||
      identity.row.descriptor_uuid != descriptor_uuid ||
      identity.row.descriptor_generation != manifest_row.descriptor_epoch ||
      !CanonicalUuidText(identity.row.type_uuid)) {
    return std::nullopt;
  }

  const bool exact_canonical_text =
      dt::IsExactCanonicalTextTypeCodecIdentityV1(identity.row);
  if (canonical_type_id == dt::CanonicalTypeId::character &&
      !exact_canonical_text) {
    return std::nullopt;
  }
  if (!exact_canonical_text) {
    if (carries_registry_authority) {
      constexpr std::array<std::string_view, 7>
          kRequiredFixedRegistrySuffix{
              "datatype_descriptor_uuid",
              "datatype_descriptor_generation", "type_generation",
              "codec_id", "codec_version", "codec_generation",
              "null_encoding"};
      if (std::ranges::any_of(kRequiredFixedRegistrySuffix,
                              [&](const auto key) {
                                return !fields.contains(key);
                              }) ||
          fields.contains("codec_uuid") != !identity.row.codec_uuid.empty() ||
          std::ranges::any_of(fields, [&](const auto& entry) {
            return is_registry_suffix_key(entry.first) &&
                   std::ranges::find(kRegistrySuffixFields, entry.first) ==
                       kRegistrySuffixFields.end();
          })) {
        return std::nullopt;
      }
      const auto exact = [&](const std::string_view key,
                             const std::string_view expected) {
        const auto found = fields.find(key);
        return found != fields.end() && found->second == expected;
      };
      const auto parse_u64 = [&](const std::string_view key,
                                 std::uint64_t* value) {
        const auto found = fields.find(key);
        if (value == nullptr || found == fields.end() ||
            found->second.empty()) {
          return false;
        }
        const auto parsed = std::from_chars(
            found->second.data(),
            found->second.data() + found->second.size(), *value, 10);
        return parsed.ec == std::errc{} &&
               parsed.ptr == found->second.data() + found->second.size() &&
               *value != 0 && std::to_string(*value) == found->second;
      };
      std::uint64_t descriptor_generation = 0;
      std::uint64_t type_generation = 0;
      std::uint64_t codec_version = 0;
      std::uint64_t codec_generation = 0;
      std::uint64_t null_encoding = 0;
      if (!exact("datatype_descriptor_uuid", identity.row.descriptor_uuid) ||
          !parse_u64("datatype_descriptor_generation",
                     &descriptor_generation) ||
          descriptor_generation != identity.row.descriptor_generation ||
          !parse_u64("type_generation", &type_generation) ||
          type_generation != identity.row.type_generation ||
          (identity.row.codec_uuid.empty()
               ? fields.contains("codec_uuid")
               : !exact("codec_uuid", identity.row.codec_uuid)) ||
          !exact("codec_id", identity.row.codec_id) ||
          !parse_u64("codec_version", &codec_version) ||
          codec_version != identity.row.codec_version ||
          !parse_u64("codec_generation", &codec_generation) ||
          codec_generation != identity.row.codec_generation ||
          !parse_u64("null_encoding", &null_encoding) ||
          null_encoding != identity.row.null_encoding_code) {
        return std::nullopt;
      }
    }
    return Rcp079ResolvedDatatypeAuthorityV1{
        descriptor_uuid, identity.row.type_uuid, false};
  }

  // Ordinary persisted TEXT columns carry the exact registry/type/codec
  // cohort but no contextual charset/collation authority.  Keep that carrier
  // distinct from the richer contextual TEXT descriptor below: absence of
  // those resources grants no contextual conversion or collation behavior.
  const bool contextual_text_carrier =
      fields.contains("character_length") || fields.contains("charset_uuid") ||
      fields.contains("collation_uuid") ||
      fields.contains("charset_generation") ||
      fields.contains("collation_generation") ||
      fields.contains("resource_epoch");
  if (!contextual_text_carrier) {
    constexpr std::array<std::string_view, 11> kRequiredPlainTextFields{
        "nullable", "datatype_descriptor_uuid",
        "datatype_descriptor_generation", "type_uuid", "type_generation",
        "codec_uuid", "codec_id", "codec_version", "codec_generation",
        "null_encoding", "column_uuid"};
    const bool type_spelling = fields.contains("type");
    const bool canonical_spelling = fields.contains("canonical");
    const auto spelling =
        type_spelling ? std::string_view("type")
                      : std::string_view("canonical");
    const auto exact = [&](const std::string_view key,
                           const std::string_view expected) {
      const auto found = fields.find(key);
      return found != fields.end() && found->second == expected;
    };
    const auto parse_u64 = [&](const std::string_view key,
                               std::uint64_t* value) {
      const auto found = fields.find(key);
      if (value == nullptr || found == fields.end() ||
          found->second.empty()) {
        return false;
      }
      const auto parsed = std::from_chars(
          found->second.data(), found->second.data() + found->second.size(),
          *value, 10);
      return parsed.ec == std::errc{} &&
             parsed.ptr == found->second.data() + found->second.size() &&
             *value != 0 && std::to_string(*value) == found->second;
    };
    std::uint64_t descriptor_generation = 0;
    std::uint64_t type_generation = 0;
    std::uint64_t codec_version = 0;
    std::uint64_t codec_generation = 0;
    std::uint64_t null_encoding = 0;
    const auto expected_nullable =
        persisted.nullable ? std::string_view("true")
                           : std::string_view("false");
    if (fields.size() != kRequiredPlainTextFields.size() + 1 ||
        type_spelling == canonical_spelling ||
        std::ranges::any_of(kRequiredPlainTextFields, [&](const auto key) {
          return !fields.contains(key);
        }) ||
        !exact(spelling, identity.row.canonical_name) ||
        !exact("nullable", expected_nullable) ||
        !exact("datatype_descriptor_uuid", identity.row.descriptor_uuid) ||
        !parse_u64("datatype_descriptor_generation",
                   &descriptor_generation) ||
        descriptor_generation != identity.row.descriptor_generation ||
        !exact("type_uuid", identity.row.type_uuid) ||
        !parse_u64("type_generation", &type_generation) ||
        type_generation != identity.row.type_generation ||
        !exact("codec_uuid", identity.row.codec_uuid) ||
        !exact("codec_id", identity.row.codec_id) ||
        !parse_u64("codec_version", &codec_version) ||
        codec_version != identity.row.codec_version ||
        !parse_u64("codec_generation", &codec_generation) ||
        codec_generation != identity.row.codec_generation ||
        !parse_u64("null_encoding", &null_encoding) ||
        null_encoding != identity.row.null_encoding_code ||
        !exact("column_uuid", persisted.column_uuid)) {
      return std::nullopt;
    }
    std::string canonical;
    const auto append = [&](const std::string_view key,
                            const std::string_view value) {
      if (!canonical.empty()) canonical.push_back(';');
      canonical.append(key);
      canonical.push_back('=');
      canonical.append(value);
    };
    append(spelling, identity.row.canonical_name);
    for (const auto key : kRequiredPlainTextFields) {
      append(key, fields.at(key));
    }
    if (canonical != persisted.value_descriptor.encoded_descriptor) {
      return std::nullopt;
    }
    return Rcp079ResolvedDatatypeAuthorityV1{
        descriptor_uuid, identity.row.type_uuid, true};
  }

  constexpr std::array<std::string_view, 17> kRequiredTextFields{
      "character_length", "charset_uuid", "collation_uuid", "nullable",
      "charset_generation", "collation_generation", "resource_epoch",
      "datatype_descriptor_uuid", "datatype_descriptor_generation",
      "type_uuid", "type_generation", "codec_uuid", "codec_id",
      "codec_version", "codec_generation", "null_encoding", "column_uuid"};
  const bool type_spelling = fields.contains("type");
  const bool canonical_spelling = fields.contains("canonical");
  if (fields.size() != kRequiredTextFields.size() + 1 ||
      type_spelling == canonical_spelling ||
      std::ranges::any_of(kRequiredTextFields, [&](const auto key) {
        return !fields.contains(key);
      })) {
    return std::nullopt;
  }
  const auto spelling =
      type_spelling ? std::string_view("type")
                    : std::string_view("canonical");
  for (const auto& [key, value] : fields) {
    if (value.empty() ||
        (key != spelling &&
         std::ranges::find(kRequiredTextFields, key) ==
             kRequiredTextFields.end())) {
      return std::nullopt;
    }
  }
  const auto exact = [&](const std::string_view key,
                         const std::string_view expected) {
    const auto found = fields.find(key);
    return found != fields.end() && found->second == expected;
  };
  const auto parse_u64 = [&](const std::string_view key,
                             std::uint64_t* value) {
    const auto found = fields.find(key);
    if (value == nullptr || found == fields.end() || found->second.empty()) {
      return false;
    }
    const auto parsed = std::from_chars(
        found->second.data(), found->second.data() + found->second.size(),
        *value, 10);
    return parsed.ec == std::errc{} &&
           parsed.ptr == found->second.data() + found->second.size() &&
           *value != 0 && std::to_string(*value) == found->second;
  };

  std::uint64_t character_length = 0;
  std::uint64_t charset_generation = 0;
  std::uint64_t collation_generation = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t descriptor_generation = 0;
  std::uint64_t type_generation = 0;
  std::uint64_t codec_version = 0;
  std::uint64_t codec_generation = 0;
  std::uint64_t null_encoding = 0;
  const auto expected_nullable =
      persisted.nullable ? std::string_view("true")
                         : std::string_view("false");
  if (!parse_u64("character_length", &character_length) ||
      !parse_u64("charset_generation", &charset_generation) ||
      !parse_u64("collation_generation", &collation_generation) ||
      !parse_u64("resource_epoch", &resource_epoch) ||
      !parse_u64("datatype_descriptor_generation",
                 &descriptor_generation) ||
      !parse_u64("type_generation", &type_generation) ||
      !parse_u64("codec_version", &codec_version) ||
      !parse_u64("codec_generation", &codec_generation) ||
      !parse_u64("null_encoding", &null_encoding) ||
      !exact(spelling, identity.row.canonical_name) ||
      character_length != persisted.character_length ||
      character_length > identity.row.canonical_value_maximum_bytes ||
      !exact("charset_uuid", persisted.charset_uuid) ||
      !exact("collation_uuid", persisted.collation_uuid) ||
      !exact("nullable", expected_nullable) ||
      !exact("column_uuid", persisted.column_uuid) ||
      resource_epoch != context.resource_epoch ||
      !exact("datatype_descriptor_uuid", identity.row.descriptor_uuid) ||
      descriptor_generation != identity.row.descriptor_generation ||
      !exact("type_uuid", identity.row.type_uuid) ||
      type_generation != identity.row.type_generation ||
      !exact("codec_uuid", identity.row.codec_uuid) ||
      !exact("codec_id", identity.row.codec_id) ||
      codec_version != identity.row.codec_version ||
      codec_generation != identity.row.codec_generation ||
      null_encoding != identity.row.null_encoding_code ||
      !CanonicalUuidText(persisted.charset_uuid) ||
      !CanonicalUuidText(persisted.collation_uuid)) {
    return std::nullopt;
  }

  api::EngineUuid charset_uuid;
  charset_uuid = persisted.charset_uuid;
  const auto charset =
      api::LookupEngineResourceDescriptorByUuid(context, charset_uuid,
                                                "charset");
  api::EngineUuid collation_uuid;
  collation_uuid = persisted.collation_uuid;
  const auto collation =
      api::LookupEngineResourceDescriptorByUuid(context, collation_uuid,
                                                "collation");
  if (!charset.ok || !collation.ok ||
      !charset.resource_descriptor.present ||
      !collation.resource_descriptor.present ||
      charset.resource_descriptor.resource_family != "charset" ||
      collation.resource_descriptor.resource_family != "collation" ||
      charset.resource_descriptor.resource_uuid !=
          persisted.charset_uuid ||
      collation.resource_descriptor.resource_uuid !=
          persisted.collation_uuid ||
      collation.resource_descriptor.parent_resource_uuid !=
          persisted.charset_uuid ||
      charset.resource_descriptor.family_epoch != charset_generation ||
      collation.resource_descriptor.family_epoch != collation_generation ||
      charset.resource_descriptor.resource_epoch != resource_epoch ||
      collation.resource_descriptor.resource_epoch != resource_epoch) {
    return std::nullopt;
  }

  std::string canonical;
  canonical.reserve(persisted.value_descriptor.encoded_descriptor.size());
  const auto append = [&](const std::string_view key,
                          const std::string_view value) {
    if (!canonical.empty()) canonical.push_back(';');
    canonical.append(key);
    canonical.push_back('=');
    canonical.append(value);
  };
  append(spelling, identity.row.canonical_name);
  append("character_length", fields.at("character_length"));
  append("charset_uuid", fields.at("charset_uuid"));
  append("collation_uuid", fields.at("collation_uuid"));
  append("nullable", expected_nullable);
  append("charset_generation", fields.at("charset_generation"));
  append("collation_generation", fields.at("collation_generation"));
  append("resource_epoch", fields.at("resource_epoch"));
  append("datatype_descriptor_uuid", identity.row.descriptor_uuid);
  append("datatype_descriptor_generation",
         fields.at("datatype_descriptor_generation"));
  append("type_uuid", identity.row.type_uuid);
  append("type_generation", fields.at("type_generation"));
  append("codec_uuid", identity.row.codec_uuid);
  append("codec_id", identity.row.codec_id);
  append("codec_version", fields.at("codec_version"));
  append("codec_generation", fields.at("codec_generation"));
  append("null_encoding", fields.at("null_encoding"));
  append("column_uuid", fields.at("column_uuid"));
  if (canonical != persisted.value_descriptor.encoded_descriptor) {
    return std::nullopt;
  }
  return Rcp079ResolvedDatatypeAuthorityV1{
      descriptor_uuid, identity.row.type_uuid, true};
}

bool Rcp079ExactColumnarJoinStorageSnapshotV1(
    const api::MgaRelationStorageDescriptor& actual,
    const api::MgaRelationStorageDescriptor& expected) {
  if (actual.descriptor_uuid !=
          expected.descriptor_uuid ||
      actual.database_uuid != expected.database_uuid ||
      actual.schema_uuid != expected.schema_uuid ||
      actual.relation_uuid != expected.relation_uuid ||
      actual.primary_filespace_uuid !=
          expected.primary_filespace_uuid ||
      actual.relation_kind != expected.relation_kind ||
      actual.storage_profile != expected.storage_profile ||
      actual.descriptor_generation != expected.descriptor_generation ||
      actual.page_size != expected.page_size ||
      actual.root_page_number != expected.root_page_number ||
      actual.allocation_root_page_number !=
          expected.allocation_root_page_number ||
      actual.row_identity_rule != expected.row_identity_rule ||
      actual.version_identity_rule != expected.version_identity_rule ||
      actual.mutation_rule != expected.mutation_rule ||
      actual.visibility_rule != expected.visibility_rule ||
      actual.cleanup_rule != expected.cleanup_rule ||
      actual.recovery_rule != expected.recovery_rule ||
      actual.descriptor_status != expected.descriptor_status ||
      actual.required_evidence_kinds != expected.required_evidence_kinds ||
      actual.columns.size() != expected.columns.size() ||
      actual.indexes.size() != expected.indexes.size()) {
    return false;
  }
  for (std::size_t ordinal = 0; ordinal < actual.columns.size(); ++ordinal) {
    const auto& left = actual.columns[ordinal];
    const auto& right = expected.columns[ordinal];
    if (left.column_uuid != right.column_uuid ||
        left.ordinal != right.ordinal ||
        left.canonical_name_key != right.canonical_name_key ||
        left.value_descriptor.descriptor_uuid !=
            right.value_descriptor.descriptor_uuid ||
        left.value_descriptor.descriptor_kind !=
            right.value_descriptor.descriptor_kind ||
        left.value_descriptor.canonical_type_name !=
            right.value_descriptor.canonical_type_name ||
        left.value_descriptor.encoded_descriptor !=
            right.value_descriptor.encoded_descriptor ||
        left.nullable != right.nullable || left.generated != right.generated ||
        left.identity_column != right.identity_column ||
        left.storage_class != right.storage_class ||
        left.charset_uuid != right.charset_uuid ||
        left.collation_uuid != right.collation_uuid ||
        left.character_length != right.character_length ||
        left.max_inline_bytes != right.max_inline_bytes ||
        left.overflow_policy != right.overflow_policy) {
      return false;
    }
  }
  for (std::size_t ordinal = 0; ordinal < actual.indexes.size(); ++ordinal) {
    const auto& left = actual.indexes[ordinal];
    const auto& right = expected.indexes[ordinal];
    if (left.index_uuid != right.index_uuid ||
        left.family != right.family || left.profile != right.profile ||
        left.unique != right.unique ||
        left.approximate != right.approximate ||
        left.key_envelopes != right.key_envelopes ||
        left.include_columns != right.include_columns ||
        left.predicate_kind != right.predicate_kind ||
        left.predicate_column != right.predicate_column ||
        left.predicate_value != right.predicate_value ||
        left.residency_policy != right.residency_policy) {
      return false;
    }
  }
  return true;
}

bool Rcp079ExactPersistedColumnDescriptorV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationColumnStorageDescriptor& persisted) {
  const auto fields =
      Rcp079ExactDescriptorFieldsV1(persisted.value_descriptor);
  if (!fields.has_value() || persisted.canonical_name_key.empty() ||
      !CanonicalUuidText(persisted.column_uuid) ||
      !api::QowCanonicalDescriptorIdentityV1(persisted.value_descriptor) ||
      persisted.value_descriptor.descriptor_kind !=
          "canonical_type_descriptor" ||
      !CanonicalUuidText(
          persisted.value_descriptor.descriptor_uuid) ||
      persisted.storage_class != "inline_row_value" ||
      persisted.max_inline_bytes != 4096 ||
      persisted.overflow_policy != "mga_large_value_locator") {
    return false;
  }
  const auto field = [&](const std::string_view name)
      -> std::optional<std::string_view> {
    const auto found = fields->find(name);
    return found == fields->end()
               ? std::optional<std::string_view>{}
               : std::optional<std::string_view>{found->second};
  };
  const auto type_uuid = field("type_uuid");
  std::optional<std::string_view> encoded_type_name;
  for (const auto alias :
       {std::string_view("type"), std::string_view("canonical"),
        std::string_view("canonical_type"),
        std::string_view("descriptor")}) {
    const auto candidate = field(alias);
    if (!candidate.has_value()) continue;
    if (encoded_type_name.has_value() &&
        *encoded_type_name != *candidate) {
      return false;
    }
    if (!encoded_type_name.has_value()) encoded_type_name = candidate;
  }
  const auto datatype_authority =
      Rcp079ResolvePersistedDatatypeAuthorityV1(context, persisted, *fields);
  if (!type_uuid.has_value() ||
      !CanonicalUuidText(std::string(*type_uuid)) ||
      !datatype_authority.has_value() ||
      datatype_authority->type_uuid != *type_uuid ||
      persisted.value_descriptor.descriptor_uuid == *type_uuid ||
      !encoded_type_name.has_value() ||
      *encoded_type_name !=
          persisted.value_descriptor.canonical_type_name) {
    return false;
  }

  const auto canonical_nullability = field("nullability");
  const auto storage_nullability = field("nullable");
  if (canonical_nullability.has_value() == storage_nullability.has_value()) {
    return false;
  }
  std::optional<bool> nullable;
  if (canonical_nullability == std::optional<std::string_view>{"nullable"}) {
    nullable = true;
  } else if (canonical_nullability ==
             std::optional<std::string_view>{"non_null"}) {
    nullable = false;
  } else if (storage_nullability ==
                 std::optional<std::string_view>{"true"} ||
             storage_nullability == std::optional<std::string_view>{"1"}) {
    nullable = true;
  } else if (storage_nullability ==
                 std::optional<std::string_view>{"false"} ||
             storage_nullability == std::optional<std::string_view>{"0"}) {
    nullable = false;
  } else {
    return false;
  }
  if (*nullable != persisted.nullable) return false;

  const auto exact_optional_bool = [&](const std::string_view name,
                                       const bool expected) {
    const auto encoded = field(name);
    if (!encoded.has_value()) return !expected;
    const auto actual =
        *encoded == "true" || *encoded == "1"
            ? std::optional<bool>{true}
            : (*encoded == "false" || *encoded == "0")
                  ? std::optional<bool>{false}
                  : std::optional<bool>{};
    return actual.has_value() && *actual == expected;
  };
  if (!exact_optional_bool("generated", persisted.generated) ||
      !exact_optional_bool("identity", persisted.identity_column)) {
    return false;
  }

  const auto encoded_collation = field("collation_uuid");
  const auto encoded_charset = field("charset_uuid");
  if (encoded_collation.has_value() !=
          !persisted.collation_uuid.empty() ||
      (encoded_collation.has_value() &&
       *encoded_collation != persisted.collation_uuid) ||
      (encoded_collation.has_value() &&
       !CanonicalUuidText(std::string(*encoded_collation))) ||
      encoded_charset.has_value() != !persisted.charset_uuid.empty() ||
      (encoded_charset.has_value() &&
       *encoded_charset != persisted.charset_uuid) ||
      (encoded_charset.has_value() &&
       !CanonicalUuidText(std::string(*encoded_charset)))) {
    return false;
  }

  const auto parse_u32 = [&](const std::string_view name,
                             std::optional<std::uint32_t>* value) {
    value->reset();
    const auto encoded = field(name);
    if (!encoded.has_value()) return true;
    if (encoded->empty() ||
        (encoded->size() > 1 && encoded->front() == '0')) {
      return false;
    }
    std::uint32_t parsed = 0;
    const auto converted = std::from_chars(
        encoded->data(), encoded->data() + encoded->size(), parsed);
    if (converted.ec != std::errc{} ||
        converted.ptr != encoded->data() + encoded->size()) {
      return false;
    }
    *value = parsed;
    return true;
  };
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> character_length;
  std::optional<std::uint32_t> precision;
  std::optional<std::uint32_t> scale;
  if (!parse_u32("width", &width) ||
      !parse_u32("character_length", &character_length) ||
      !parse_u32("precision", &precision) ||
      !parse_u32("scale", &scale) ||
      character_length.has_value() !=
          (persisted.character_length != 0) ||
      (character_length.has_value() &&
       *character_length != persisted.character_length) ||
      (character_length.has_value() && width.has_value() &&
       *character_length != *width)) {
    return false;
  }
  const auto timezone = field("timezone_profile_id");
  return !timezone.has_value() || !timezone->empty();
}

bool Rcp079ExactColumnarJoinColumnBindingV1(
    const api::EngineRequestContext& context,
    const api::RelationalTypeDescriptor& relational,
    const api::MgaRelationColumnStorageDescriptor& persisted) {
  const auto fields =
      Rcp079ExactDescriptorFieldsV1(persisted.value_descriptor);
  if (!fields.has_value() ||
      !Rcp079ExactPersistedColumnDescriptorV1(context, persisted)) {
    return false;
  }
  const auto field = [&](const std::string_view name)
      -> std::optional<std::string_view> {
    const auto found = fields->find(name);
    return found == fields->end()
               ? std::optional<std::string_view>{}
               : std::optional<std::string_view>{found->second};
  };
  const auto type_uuid = field("type_uuid");
  const auto datatype_authority =
      Rcp079ResolvePersistedDatatypeAuthorityV1(context, persisted, *fields);
  if (!type_uuid.has_value() ||
      !CanonicalUuidText(std::string(*type_uuid)) ||
      !datatype_authority.has_value() ||
      datatype_authority->type_uuid != *type_uuid ||
      (relational.descriptor_uuid !=
           persisted.value_descriptor.descriptor_uuid &&
       relational.descriptor_uuid != datatype_authority->descriptor_uuid) ||
      relational.type_uuid != *type_uuid ||
      relational.descriptor_uuid == relational.type_uuid) {
    return false;
  }
  const auto canonical_nullability = field("nullability");
  const auto storage_nullability = field("nullable");
  if (canonical_nullability.has_value() == storage_nullability.has_value()) {
    return false;
  }
  std::optional<bool> nullable;
  if (canonical_nullability == std::optional<std::string_view>{"nullable"}) {
    nullable = true;
  } else if (canonical_nullability ==
             std::optional<std::string_view>{"non_null"}) {
    nullable = false;
  } else if (storage_nullability ==
                 std::optional<std::string_view>{"true"} ||
             storage_nullability == std::optional<std::string_view>{"1"}) {
    nullable = true;
  } else if (storage_nullability ==
                 std::optional<std::string_view>{"false"} ||
             storage_nullability == std::optional<std::string_view>{"0"}) {
    nullable = false;
  } else {
    return false;
  }
  if (*nullable != persisted.nullable ||
      relational.nullability !=
          (persisted.nullable ? api::RelationalNullability::kNullable
                              : api::RelationalNullability::kNonNull)) {
    return false;
  }
  const auto encoded_collation = field("collation_uuid");
  const auto persisted_collation =
      persisted.collation_uuid.empty()
          ? encoded_collation
          : std::optional<std::string_view>{persisted.collation_uuid};
  if ((!persisted.collation_uuid.empty() &&
       encoded_collation != persisted_collation) ||
      (persisted_collation.has_value() &&
       !CanonicalUuidText(std::string(*persisted_collation))) ||
      relational.collation_uuid !=
          (persisted_collation.has_value()
               ? std::optional<std::string>{*persisted_collation}
               : std::optional<std::string>{})) {
    return false;
  }
  const auto encoded_charset = field("charset_uuid");
  if ((!persisted.charset_uuid.empty() &&
       encoded_charset !=
           std::optional<std::string_view>{persisted.charset_uuid}) ||
      (encoded_charset.has_value() &&
       !CanonicalUuidText(std::string(*encoded_charset)))) {
    return false;
  }
  const auto parse_u32 = [&](const std::string_view name,
                             std::optional<std::uint32_t>* value) {
    value->reset();
    const auto encoded = field(name);
    if (!encoded.has_value() || encoded->empty()) return !encoded.has_value();
    if (encoded->size() > 1 && encoded->front() == '0') return false;
    std::uint32_t parsed = 0;
    const auto converted = std::from_chars(
        encoded->data(), encoded->data() + encoded->size(), parsed);
    if (converted.ec != std::errc{} ||
        converted.ptr != encoded->data() + encoded->size()) {
      return false;
    }
    *value = parsed;
    return true;
  };
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> precision;
  std::optional<std::uint32_t> scale;
  if (!parse_u32("width", &width) ||
      !parse_u32("precision", &precision) ||
      !parse_u32("scale", &scale)) {
    return false;
  }
  if (persisted.character_length != 0) {
    if (width.has_value() && *width != persisted.character_length) {
      return false;
    }
    width = persisted.character_length;
  }
  const auto timezone = field("timezone_profile_id");
  return relational.timezone_profile_id ==
             (timezone.has_value()
                  ? std::optional<std::string>{*timezone}
                  : std::optional<std::string>{}) &&
         relational.width == width && relational.precision == precision &&
         relational.scale == scale;
}

bool Rcp079ExactColumnarIdentifierBindingsV1(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const api::RelationalDagNode& source,
    const api::MgaRelationStorageDescriptor& persisted,
    const bool has_filter,
    const bool has_project,
    std::string* detail) {
  if (detail == nullptr) return false;
  detail->clear();
  const auto refuse = [&](std::string value) {
    *detail = std::move(value);
    return false;
  };
  if (source.required_object_uuids.size() != 1 ||
      persisted.columns.empty() ||
      persisted.relation_uuid !=
          source.required_object_uuids.front()) {
    return refuse("columnar source object or persisted relation is not exact");
  }
  const auto& object_uuid = source.required_object_uuids.front();
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.expression_id == expression_id;
    });
  };
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
      return descriptor.descriptor_id == descriptor_id;
    });
  };
  const auto operation_expression = [&](const std::string_view operation) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.operator_name == operation;
    });
  };
  const auto operation_count = [&](const std::string_view operation) {
    return std::ranges::count_if(dag.expressions, [&](const auto& expression) {
      return expression.operator_name == operation;
    });
  };
  if (operation_count("COLUMNAR_SOURCE") != 1 ||
      operation_count("COLUMNAR_FILTER") !=
          static_cast<std::ptrdiff_t>(has_filter) ||
      operation_count("COLUMNAR_PROJECT") !=
          static_cast<std::ptrdiff_t>(has_project)) {
    return refuse("columnar operation root cardinality is not exact");
  }
  const auto columnar_source = operation_expression("COLUMNAR_SOURCE");
  if (columnar_source == dag.expressions.end() ||
      columnar_source->expression_kind !=
          api::RelationalExpressionKind::kFunctionCall ||
      columnar_source->function_uuid.has_value() ||
      !columnar_source->child_expression_ids.empty() ||
      columnar_source->bound_name_uuid !=
          std::optional<std::string>{object_uuid} ||
      columnar_source->literal_kind.has_value() ||
      columnar_source->literal_or_parameter_ref.has_value() ||
      columnar_source->operator_name != "COLUMNAR_SOURCE" ||
      columnar_source->result_descriptor_id == 0 ||
      std::ranges::count(source.bound_expression_ids,
                         columnar_source->expression_id) != 1) {
    return refuse("columnar source root or descriptor handle is not exact");
  }
  const auto source_alias_descriptor =
      descriptor_for(columnar_source->result_descriptor_id);
  if (source_alias_descriptor == dag.descriptors.end() ||
      !Rcp079ExactColumnarJoinColumnBindingV1(
          context, *source_alias_descriptor, persisted.columns.front())) {
    return refuse("columnar source descriptor is not first-column exact");
  }

  std::unordered_set<std::uint32_t> relation_alias_ids;
  const auto admit_operation_alias = [&](const std::string_view operation,
                                          const std::size_t minimum_arity,
                                          const std::size_t maximum_arity) {
    const auto root = operation_expression(operation);
    if (root == dag.expressions.end() ||
        root->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        root->function_uuid.has_value() || root->bound_name_uuid.has_value() ||
        root->literal_kind.has_value() ||
        root->literal_or_parameter_ref.has_value() ||
        root->operator_name != operation ||
        root->child_expression_ids.size() < minimum_arity ||
        root->child_expression_ids.size() > maximum_arity ||
        std::ranges::count(source.bound_expression_ids,
                           root->expression_id) != 1) {
      return refuse("columnar operation root or attachment is not exact");
    }
    const auto alias_id = root->child_expression_ids.front();
    const auto alias = expression_for(alias_id);
    if (alias_id == 0 || alias == dag.expressions.end() ||
        alias->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !alias->child_expression_ids.empty() ||
        alias->function_uuid.has_value() ||
        alias->bound_name_uuid != std::optional<std::string>{object_uuid} ||
        alias->literal_kind.has_value() ||
        alias->literal_or_parameter_ref.has_value() ||
        alias->operator_name.has_value() ||
        alias->result_descriptor_id !=
            columnar_source->result_descriptor_id ||
        !relation_alias_ids.insert(alias_id).second) {
      return refuse(
          "columnar operation source alias is not one exact distinct identifier");
    }
    const auto alias_descriptor = descriptor_for(alias->result_descriptor_id);
    if (alias_descriptor == dag.descriptors.end() ||
        !Rcp079ExactColumnarJoinColumnBindingV1(
            context, *alias_descriptor, persisted.columns.front())) {
      return refuse(
          "columnar operation source alias descriptor is not source-exact");
    }
    std::size_t reference_count = 0;
    for (const auto& expression : dag.expressions) {
      reference_count += static_cast<std::size_t>(std::ranges::count(
          expression.child_expression_ids, alias_id));
    }
    return reference_count == 1 ||
           refuse("columnar operation source alias occurrence is reused or orphaned");
  };
  if ((has_filter &&
       !admit_operation_alias("COLUMNAR_FILTER", 2, 2)) ||
      (has_project &&
       !admit_operation_alias("COLUMNAR_PROJECT", 2, 257)) ||
      relation_alias_ids.size() !=
          static_cast<std::size_t>(has_filter) +
              static_cast<std::size_t>(has_project)) {
    if (detail->empty()) {
      *detail = "columnar operation source alias cardinality is not exact";
    }
    return false;
  }
  for (const auto& expression : dag.expressions) {
    if (expression.expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !expression.bound_name_uuid.has_value()) {
      continue;
    }
    if (relation_alias_ids.contains(expression.expression_id)) continue;
    if (*expression.bound_name_uuid == object_uuid) {
      return refuse(
          "columnar relation alias identifier is outside an admitted operation root");
    }
    const auto persisted_column_count = std::ranges::count_if(
        persisted.columns, [&](const auto& column) {
          return column.column_uuid == *expression.bound_name_uuid;
        });
    if (persisted_column_count != 1) {
      return refuse("columnar identifier is not uniquely bound to a persisted column");
    }
    const auto persisted_column = std::ranges::find_if(
        persisted.columns, [&](const auto& column) {
          return column.column_uuid == *expression.bound_name_uuid;
        });
    const auto relational_descriptor =
        descriptor_for(expression.result_descriptor_id);
    if (relational_descriptor == dag.descriptors.end() ||
        !Rcp079ExactColumnarJoinColumnBindingV1(
            context, *relational_descriptor, *persisted_column)) {
      return refuse(
          "columnar referenced identifier differs from persisted type authority");
    }
  }
  return true;
}

bool Rcp079ExactExecutorColumnV1(
    const exec::ExecutorColumnDescriptor& actual,
    const exec::ExecutorColumnDescriptor& expected) {
  return actual.stable_name == expected.stable_name &&
         actual.nullable == expected.nullable &&
         actual.descriptor_id == expected.descriptor_id &&
         actual.descriptor.descriptor_uuid ==
             expected.descriptor.descriptor_uuid &&
         actual.descriptor.descriptor_kind ==
             expected.descriptor.descriptor_kind &&
         actual.descriptor.canonical_type_name ==
             expected.descriptor.canonical_type_name &&
         actual.descriptor.encoded_descriptor ==
             expected.descriptor.encoded_descriptor;
}

std::optional<std::uint64_t> Rcp079VisibleRowsMemoryBytesV1(
    const std::vector<api::CrudRowVersionRecord>& rows) {
  std::uint64_t bytes = 0;
  if (!CheckedMultiply(rows.capacity(), sizeof(api::CrudRowVersionRecord),
                       &bytes)) {
    return std::nullopt;
  }
  const auto account_string = [&](const std::string& value) {
    return CheckedAdd(bytes, value.capacity(), &bytes) &&
           CheckedAdd(bytes, 1, &bytes);
  };
  for (const auto& row : rows) {
    std::uint64_t values_bytes = 0;
    if (!account_string(row.table_uuid) || !account_string(row.row_uuid) ||
        !account_string(row.version_uuid) ||
        !account_string(row.temporary_session_uuid) ||
        !account_string(row.previous_version_uuid) ||
        !CheckedMultiply(
            row.values.capacity(),
            sizeof(std::pair<std::string, std::string>), &values_bytes) ||
        !CheckedAdd(bytes, values_bytes, &bytes)) {
      return std::nullopt;
    }
    for (const auto& [name, value] : row.values) {
      if (!account_string(name) || !account_string(value)) {
        return std::nullopt;
      }
    }
  }
  return bytes;
}

bool Rcp079AccountLogicalStringV1(const std::string_view value,
                                  std::uint64_t* bytes) {
  return bytes != nullptr && CheckedAdd(*bytes, value.size(), bytes) &&
         CheckedAdd(*bytes, 1, bytes);
}

bool Rcp079AccountLogicalArrayV1(const std::size_t count,
                                 const std::size_t element_bytes,
                                 std::uint64_t* bytes) {
  std::uint64_t allocation = 0;
  return bytes != nullptr &&
         CheckedMultiply(count, element_bytes, &allocation) &&
         CheckedAdd(*bytes, allocation, bytes);
}

bool Rcp079AccountLogicalMgaV1(
    const exec::PhysicalMgaStatementContext& context,
    std::uint64_t* bytes) {
  return Rcp079AccountLogicalStringV1(context.statement_uuid, bytes) &&
         Rcp079AccountLogicalStringV1(context.owning_transaction_uuid,
                                      bytes) &&
         Rcp079AccountLogicalStringV1(context.statement_snapshot_uuid,
                                      bytes) &&
         Rcp079AccountLogicalStringV1(
             context.statement_metadata_snapshot_uuid, bytes) &&
         Rcp079AccountLogicalArrayV1(
             context.active_excluded_local_transaction_ids.size(),
             sizeof(std::uint64_t), bytes) &&
         Rcp079AccountLogicalArrayV1(
             context.in_doubt_excluded_local_transaction_ids.size(),
             sizeof(std::uint64_t), bytes) &&
         Rcp079AccountLogicalStringV1(context.snapshot_kind, bytes) &&
         Rcp079AccountLogicalStringV1(context.statement_timestamp, bytes);
}

std::optional<std::uint64_t> Rcp079DescriptorBatchLogicalMemoryBytesV1(
    const exec::DescriptorBatch& batch,
    const bool include_inline_object = true) {
  std::uint64_t bytes = include_inline_object ? sizeof(batch) : 0;
  if (!Rcp079AccountLogicalArrayV1(
          batch.columns.size(), sizeof(exec::ExecutorColumnDescriptor),
          &bytes) ||
      !Rcp079AccountLogicalArrayV1(
          batch.rows.size(), sizeof(exec::DescriptorTuple), &bytes)) {
    return std::nullopt;
  }
  const auto account_descriptor = [&](const api::EngineDescriptor& descriptor) {
    return Rcp079AccountLogicalStringV1(
               descriptor.descriptor_uuid, &bytes) &&
           Rcp079AccountLogicalStringV1(descriptor.descriptor_kind, &bytes) &&
           Rcp079AccountLogicalStringV1(descriptor.canonical_type_name,
                                        &bytes) &&
           Rcp079AccountLogicalStringV1(descriptor.encoded_descriptor,
                                        &bytes);
  };
  for (const auto& column : batch.columns) {
    if (!Rcp079AccountLogicalStringV1(column.stable_name, &bytes) ||
        !account_descriptor(column.descriptor)) {
      return std::nullopt;
    }
  }
  for (const auto& row : batch.rows) {
    if (!Rcp079AccountLogicalArrayV1(
            row.values.size(), sizeof(api::EngineTypedValue), &bytes)) {
      return std::nullopt;
    }
    for (const auto& value : row.values) {
      if (!account_descriptor(value.descriptor) ||
          !Rcp079AccountLogicalStringV1(value.encoded_value, &bytes) ||
          !Rcp079AccountLogicalArrayV1(value.binary_value.size(),
                                       sizeof(std::uint8_t), &bytes)) {
        return std::nullopt;
      }
    }
  }
  return bytes;
}

bool Rcp079AccountModelRowIdentityLogicalMemoryV1(
    const exec::ModelProviderRowIdentityV1& identity,
    std::uint64_t* bytes) {
  if (!Rcp079AccountLogicalArrayV1(1, sizeof(identity), bytes)) return false;
  const std::array<std::string_view, 17> strings{
      identity.document_uuid,
      identity.row_uuid,
      identity.vertex_uuid,
      identity.edge_uuid,
      identity.path_uuid,
      identity.key,
      identity.series_uuid,
      identity.metric_uuid,
      identity.tags,
      identity.time_series_payload_kind,
      identity.time_series_raw_value,
      identity.time_series_sample_count,
      identity.time_series_aggregate_value,
      identity.vector_distance,
      identity.vector_score,
      identity.search_analyzer_uuid,
      identity.search_score};
  return std::ranges::all_of(strings, [&](const auto value) {
    return Rcp079AccountLogicalStringV1(value, bytes);
  });
}

bool Rcp079AccountModelPropertyLogicalMemoryV1(
    const exec::ModelPropertyDescriptorV1& property,
    std::uint64_t* bytes,
    const bool include_inline_object = true) {
  return (!include_inline_object ||
          Rcp079AccountLogicalArrayV1(1, sizeof(property), bytes)) &&
         Rcp079AccountLogicalStringV1(property.property_descriptor_id,
                                      bytes) &&
         Rcp079AccountLogicalStringV1(property.property_uuid, bytes) &&
         Rcp079AccountLogicalStringV1(property.ordering_id, bytes) &&
         Rcp079AccountLogicalStringV1(property.partitioning_id, bytes) &&
         Rcp079AccountLogicalStringV1(property.uniqueness_id, bytes);
}

std::optional<std::uint64_t> Rcp079ModelProviderBatchLogicalMemoryBytesV1(
    const exec::ModelProviderBatchV1& batch) {
  std::uint64_t bytes = sizeof(batch);
  const auto descriptor_batch =
      Rcp079DescriptorBatchLogicalMemoryBytesV1(batch.batch, false);
  if (!descriptor_batch.has_value() ||
      !CheckedAdd(bytes, *descriptor_batch, &bytes) ||
      !Rcp079AccountLogicalStringV1(batch.provider_uuid, &bytes) ||
      !Rcp079AccountLogicalStringV1(batch.selected_alternative_uuid, &bytes) ||
      !Rcp079AccountLogicalStringV1(batch.capability_uuid, &bytes) ||
      !Rcp079AccountLogicalStringV1(batch.result_handle_uuid, &bytes) ||
      !Rcp079AccountLogicalArrayV1(batch.output_descriptor_ids.size(),
                                   sizeof(std::uint32_t), &bytes) ||
      !Rcp079AccountLogicalStringV1(batch.security_receipt_uuid, &bytes) ||
      !Rcp079AccountLogicalStringV1(
          batch.multimodel_composition_receipt_uuid, &bytes) ||
      !Rcp079AccountModelPropertyLogicalMemoryV1(
          batch.properties, &bytes, false) ||
      !Rcp079AccountLogicalMgaV1(batch.mga_statement_context, &bytes)) {
    return std::nullopt;
  }
  for (const auto& identity : batch.ordered_row_identities) {
    if (!Rcp079AccountModelRowIdentityLogicalMemoryV1(identity, &bytes)) {
      return std::nullopt;
    }
  }
  return bytes;
}

std::optional<std::uint64_t>
Rcp079ProjectedModelSourceOutputLogicalMemoryBytesV1(
    const exec::ModelSourceInputDescriptorV1& input,
    const exec::ModelProviderBatchV1& provider) {
  std::uint64_t bytes = sizeof(exec::ModelSourceOutputDescriptorV1);
  const auto descriptor_batch =
      Rcp079DescriptorBatchLogicalMemoryBytesV1(provider.batch, false);
  if (!descriptor_batch.has_value() ||
      !CheckedAdd(bytes, *descriptor_batch, &bytes)) {
    return std::nullopt;
  }
  const std::array<std::string_view, 11> strings{
      "SB_MODEL_SOURCE_OUTPUT_DESCRIPTOR_V1",
      input.family_id,
      input.operation_id,
      input.object_uuid,
      input.spatial_geometry_descriptor_uuid,
      input.spatial_geometry_type_uuid,
      input.spatial_crs_uuid,
      input.selected_alternative_uuid,
      input.capability_uuid,
      input.provider_uuid,
      input.result_handle_uuid};
  if (!std::ranges::all_of(strings, [&](const auto value) {
        return Rcp079AccountLogicalStringV1(value, &bytes);
      }) ||
      !Rcp079AccountLogicalArrayV1(input.operation_ids.size(),
                                   sizeof(std::string), &bytes) ||
      !Rcp079AccountLogicalArrayV1(input.output_descriptor_ids.size(),
                                   sizeof(std::uint32_t), &bytes) ||
      !Rcp079AccountLogicalStringV1(provider.security_receipt_uuid, &bytes) ||
      !Rcp079AccountLogicalStringV1(
          provider.multimodel_composition_receipt_uuid, &bytes) ||
      !Rcp079AccountModelPropertyLogicalMemoryV1(
          provider.properties, &bytes, false) ||
      !Rcp079AccountLogicalMgaV1(input.mga_statement_context, &bytes)) {
    return std::nullopt;
  }
  for (const auto& operation : input.operation_ids) {
    if (!Rcp079AccountLogicalStringV1(operation, &bytes)) {
      return std::nullopt;
    }
  }
  for (const auto& identity : provider.ordered_row_identities) {
    if (!Rcp079AccountModelRowIdentityLogicalMemoryV1(identity, &bytes)) {
      return std::nullopt;
    }
  }
  return bytes;
}

std::optional<std::uint64_t> Rcp079ColumnarUniquenessLogicalMemoryBytesV1(
    const std::vector<exec::ModelProviderRowIdentityV1>& identities) {
  std::uint64_t bytes = 3 * sizeof(std::unordered_set<std::string>);
  constexpr std::uint64_t kSetNodeOverhead =
      sizeof(std::string) + 4 * sizeof(void*) + 64;
  for (const auto& identity : identities) {
    std::uint64_t node = kSetNodeOverhead;
    if (!CheckedAdd(node, identity.row_uuid.size(), &node) ||
        !CheckedAdd(bytes, node, &bytes)) {
      return std::nullopt;
    }
  }
  return bytes;
}

std::optional<std::uint64_t>
Rcp079ColumnarRequestNonBatchLogicalMemoryBytesV2(
    const api::nosql::ColumnarExecutionRequestV2& request) {
  std::uint64_t bytes = sizeof(request);
  if (!Rcp079AccountLogicalStringV1(request.profile_id, &bytes) ||
      !Rcp079AccountLogicalStringV1(request.operation_id, &bytes) ||
      !Rcp079AccountLogicalStringV1(request.relation_uuid, &bytes) ||
      !Rcp079AccountLogicalArrayV1(request.operation_ids.size(),
                                   sizeof(std::string), &bytes) ||
      !Rcp079AccountLogicalArrayV1(request.row_uuids.size(),
                                   sizeof(std::string), &bytes) ||
      !Rcp079AccountLogicalArrayV1(request.projected_columns.size(),
                                   sizeof(std::size_t), &bytes) ||
      !Rcp079AccountLogicalArrayV1(request.filter_truth_values.size(),
                                   sizeof(api::EngineSqlTruthValue), &bytes) ||
      !Rcp079AccountLogicalArrayV1(
          request.zone_proof.candidate_row_ordinals.size(),
          sizeof(std::size_t), &bytes) ||
      !Rcp079AccountLogicalMgaV1(request.statement_context, &bytes) ||
      !Rcp079AccountLogicalMgaV1(request.current_statement_context, &bytes)) {
    return std::nullopt;
  }
  for (const auto& operation : request.operation_ids) {
    if (!Rcp079AccountLogicalStringV1(operation, &bytes)) {
      return std::nullopt;
    }
  }
  for (const auto& row_uuid : request.row_uuids) {
    if (!Rcp079AccountLogicalStringV1(row_uuid, &bytes)) {
      return std::nullopt;
    }
  }
  return bytes;
}

struct Rcp079ContextualExecutionLogicalMemoryPlanV2 {
  std::uint64_t pre_barrier_peak_bytes{0};
  std::uint64_t post_api_retained_bytes{0};
};

bool Rcp079AdmitContextualMemoryPeakV2(
    const std::uint64_t base_bytes,
    const std::uint64_t contextual_bytes,
    const std::uint64_t grant_bytes,
    std::uint64_t* peak_bytes) noexcept {
  return peak_bytes != nullptr &&
         CheckedAdd(base_bytes, contextual_bytes, peak_bytes) &&
         *peak_bytes <= grant_bytes;
}

bool Rcp079ReserveContextualPostConsumeGrantV2(
    const std::uint64_t source_grant_bytes,
    const std::uint64_t retained_visible_row_bytes,
    const std::uint64_t contextual_post_consume_bytes,
    std::uint64_t* api_grant_bytes) noexcept {
  if (api_grant_bytes == nullptr ||
      retained_visible_row_bytes > source_grant_bytes) {
    return false;
  }
  const auto after_rows = source_grant_bytes - retained_visible_row_bytes;
  if (contextual_post_consume_bytes >= after_rows) return false;
  *api_grant_bytes = after_rows - contextual_post_consume_bytes;
  return *api_grant_bytes != 0;
}

std::optional<Rcp079ContextualExecutionLogicalMemoryPlanV2>
Rcp079ContextualExecutionLogicalMemoryPlan(
    const std::shared_ptr<ContextualTextDispatchActivationV2>& activation,
    const std::vector<ContextualTextDirectRouteTargetV2>& targets,
    const std::vector<ContextualTextDirectRouteEqualityV2>& equalities) {
  if (!activation || !activation->prepared.valid() ||
      activation->joint_consumed || activation->lease.valid() ||
      activation->bridge_retained_logical_bytes == 0) {
    return std::nullopt;
  }
  const auto provider =
      api::EstimatePreparedContextualTextLiteralResourcesV2(
          activation->prepared);
  if (!provider.ok ||
      provider.status !=
          api::EngineContextualTextResourceEstimateStatusV2::ok ||
      provider.prepared_retained_bytes == 0 ||
      provider.joint_incremental_peak_bytes == 0 ||
      provider.post_consume_lease_retained_bytes == 0) {
    return std::nullopt;
  }

  std::uint64_t activation_bytes = sizeof(ContextualTextDispatchActivationV2);
  if (!CheckedAdd(activation_bytes,
                  activation->bridge_retained_logical_bytes,
                  &activation_bytes)) {
    return std::nullopt;
  }
  std::uint64_t route_bytes = activation_bytes;
  if (!CheckedAdd(route_bytes, sizeof(targets), &route_bytes) ||
      !CheckedAdd(route_bytes, sizeof(equalities), &route_bytes)) {
    return std::nullopt;
  }
  const auto account_string = [&](const std::string& value) {
    return Rcp079AccountLogicalArrayV1(value.capacity(), sizeof(char),
                                       &route_bytes) &&
           CheckedAdd(route_bytes, 1, &route_bytes);
  };
  const auto account_engine_descriptor = [&](
                                             const api::EngineDescriptor& value) {
    return account_string(value.descriptor_uuid) &&
           account_string(value.descriptor_kind) &&
           account_string(value.canonical_type_name) &&
           account_string(value.encoded_descriptor);
  };
  const auto account_relational_descriptor = [
      &](const api::RelationalTypeDescriptor& value) {
    return account_string(value.descriptor_uuid) &&
           account_string(value.type_uuid) &&
           (!value.collation_uuid.has_value() ||
            account_string(*value.collation_uuid)) &&
           (!value.timezone_profile_id.has_value() ||
            account_string(*value.timezone_profile_id)) &&
           account_string(value.codec_id) &&
           account_string(value.statement_receipt_uuid) &&
           account_string(value.datatype_catalog_snapshot_uuid);
  };
  // Provider-owned Prepared, joint-transition, and lease storage is reported
  // only by the authority estimator. This route accounts its own activation,
  // bridge, prevalidated handles, descriptor copies, and erased callback
  // target payloads without inspecting provider internals. The expression
  // runtime is counted in the caller's ordinary row-binding cohort.
  constexpr std::uint64_t kContextualCallbackTargetPayload =
      sizeof(std::shared_ptr<ContextualTextDispatchActivationV2>) +
      2 * sizeof(void*);
  if (!Rcp079AccountLogicalArrayV1(
          targets.capacity(), sizeof(ContextualTextDirectRouteTargetV2),
          &route_bytes) ||
      !Rcp079AccountLogicalArrayV1(
          equalities.capacity(), sizeof(ContextualTextDirectRouteEqualityV2),
          &route_bytes) ||
      !CheckedAdd(route_bytes, kContextualCallbackTargetPayload,
                  &route_bytes)) {
    return std::nullopt;
  }
  for (const auto& target : targets) {
    if (!account_relational_descriptor(target.bound_descriptor) ||
        !account_engine_descriptor(target.persisted_descriptor)) {
      return std::nullopt;
    }
  }
  std::uint64_t prepared_retained = 0;
  std::uint64_t joint_peak = 0;
  std::uint64_t post_api_retained = 0;
  if (!CheckedAdd(route_bytes, provider.prepared_retained_bytes,
                  &prepared_retained) ||
      !CheckedAdd(prepared_retained,
                  provider.joint_incremental_peak_bytes, &joint_peak) ||
      !CheckedAdd(activation_bytes,
                  provider.post_consume_lease_retained_bytes,
                  &post_api_retained)) {
    return std::nullopt;
  }
  Rcp079ContextualExecutionLogicalMemoryPlanV2 plan;
  plan.pre_barrier_peak_bytes = joint_peak;
  plan.post_api_retained_bytes = post_api_retained;
  return plan;
}

std::optional<std::uint64_t>
Rcp079ColumnarLogicalMaterializationAdditionalBytesV1(
    const api::MgaRelationStorageDescriptor& persisted,
    const std::size_t row_count,
    const api::TypedRelationalDag& dag,
    const exec::ModelSourceInputDescriptorV1& source_input,
    const std::string& property_uuid,
    const std::string& security_receipt_uuid,
    const bool has_filter) {
  std::uint64_t bytes = sizeof(exec::DescriptorBatch);
  std::uint64_t allocation = 0;
  const auto add_array = [&](const std::uint64_t count,
                             const std::uint64_t element_bytes) {
    return CheckedMultiply(count, element_bytes, &allocation) &&
           CheckedAdd(bytes, allocation, &bytes);
  };
  const auto add_string = [&](const std::string& value) {
    return CheckedAdd(bytes, value.capacity(), &bytes) &&
           CheckedAdd(bytes, 1, &bytes);
  };
  if (!add_array(persisted.columns.size(),
                 sizeof(exec::ExecutorColumnDescriptor)) ||
      !add_array(persisted.columns.size(), sizeof(std::uint32_t)) ||
      !add_array(row_count, sizeof(exec::DescriptorTuple)) ||
      !add_array(row_count, sizeof(std::string)) ||
      !add_array(row_count, sizeof(api::EngineSqlTruthValue)) ||
      !add_array(persisted.columns.size(), sizeof(std::size_t)) ||
      !add_array(1, sizeof(api::nosql::ColumnarExecutionRequestV2)) ||
      !add_array(source_input.operation_ids.size(), sizeof(std::string)) ||
      !add_array(source_input.output_descriptor_ids.size(),
                 sizeof(std::uint32_t)) ||
      !add_array(row_count, sizeof(exec::ModelProviderRowIdentityV1))) {
    return std::nullopt;
  }
  if (!add_string(source_input.operation_id) ||
      !add_string(source_input.object_uuid) ||
      !add_string(source_input.provider_uuid) ||
      !add_string(source_input.selected_alternative_uuid) ||
      !add_string(source_input.capability_uuid) ||
      !add_string(source_input.result_handle_uuid) ||
      !add_string(source_input.security_context_uuid) ||
      !add_string(source_input.policy_snapshot_uuid) ||
      !add_string(source_input.resource_contract_uuid) ||
      !add_string(property_uuid) ||
      !add_string(security_receipt_uuid) ||
      !Rcp079AccountLogicalStringV1(
          "SB_MODEL_PROPERTY_DESCRIPTOR_V1", &bytes) ||
      !Rcp079AccountLogicalStringV1("fixture_order", &bytes) ||
      !Rcp079AccountLogicalStringV1("single_local_partition", &bytes) ||
      !Rcp079AccountLogicalStringV1("row_uuid", &bytes) ||
      !Rcp079AccountLogicalStringV1(
          api::nosql::kColumnarLogicalReconstructionV2, &bytes) ||
      !Rcp079AccountLogicalMgaV1(source_input.mga_statement_context, &bytes) ||
      !Rcp079AccountLogicalMgaV1(source_input.mga_statement_context, &bytes) ||
      !Rcp079AccountLogicalMgaV1(source_input.mga_statement_context, &bytes) ||
      !Rcp079AccountLogicalMgaV1(source_input.mga_statement_context, &bytes)) {
    return std::nullopt;
  }
  for (const auto& operation : source_input.operation_ids) {
    if (!add_string(operation)) return std::nullopt;
  }
  std::uint64_t per_cell_descriptor_bytes = 0;
  for (const auto& column : persisted.columns) {
    if (!add_string(column.canonical_name_key) ||
        !add_string(column.value_descriptor.descriptor_uuid) ||
        !add_string(column.value_descriptor.descriptor_kind) ||
        !add_string(column.value_descriptor.canonical_type_name) ||
        !add_string(column.value_descriptor.encoded_descriptor)) {
      return std::nullopt;
    }
    const auto account_per_cell = [&](const std::string& value) {
      return CheckedAdd(per_cell_descriptor_bytes, value.capacity(),
                        &per_cell_descriptor_bytes) &&
             CheckedAdd(per_cell_descriptor_bytes, 1,
                        &per_cell_descriptor_bytes);
    };
    if (!account_per_cell(
            column.value_descriptor.descriptor_uuid) ||
        !account_per_cell(column.value_descriptor.descriptor_kind) ||
        !account_per_cell(column.value_descriptor.canonical_type_name) ||
        !account_per_cell(column.value_descriptor.encoded_descriptor)) {
      return std::nullopt;
    }
  }
  std::uint64_t cells = 0;
  std::uint64_t cell_objects = 0;
  std::uint64_t cell_descriptors = 0;
  if (!CheckedMultiply(row_count, persisted.columns.size(), &cells) ||
      !CheckedMultiply(cells, sizeof(api::EngineTypedValue),
                       &cell_objects) ||
      !CheckedMultiply(row_count, per_cell_descriptor_bytes,
                       &cell_descriptors) ||
      !CheckedAdd(bytes, cell_objects, &bytes) ||
      !CheckedAdd(bytes, cell_descriptors, &bytes)) {
    return std::nullopt;
  }
  if (has_filter) {
    std::uint64_t edge_count = 0;
    for (const auto& expression : dag.expressions) {
      if (!CheckedAdd(edge_count, expression.child_expression_ids.size(),
                      &edge_count)) {
        return std::nullopt;
      }
    }
    constexpr std::uint64_t kNodeLinks = 6 * sizeof(void*);
    const auto expression_count = dag.expressions.size();
    std::uint64_t bucket_pointer_count = 0;
    std::uint64_t expression_bucket_pointer_count = 0;
    std::uint64_t pending_count = 0;
    if (!CheckedMultiply(persisted.columns.size(), 2,
                         &bucket_pointer_count) ||
        !CheckedMultiply(expression_count, 4,
                         &expression_bucket_pointer_count) ||
        !CheckedAdd(bucket_pointer_count,
                    expression_bucket_pointer_count,
                    &bucket_pointer_count) ||
        !CheckedAdd(edge_count, 1, &pending_count)) {
      return std::nullopt;
    }
    if (!add_array(persisted.columns.size(),
                   sizeof(std::pair<const std::uint32_t, std::size_t>) +
                       kNodeLinks) ||
        !add_array(expression_count,
                   sizeof(std::pair<
                       const std::uint32_t,
                       const api::RelationalExpressionRecord*>) + kNodeLinks) ||
        !add_array(expression_count, sizeof(std::uint32_t) + kNodeLinks) ||
        !add_array(bucket_pointer_count, sizeof(void*)) ||
        !add_array(pending_count, sizeof(std::uint32_t)) ||
        !add_array(persisted.columns.size(), sizeof(std::uint32_t)) ||
        !add_array(expression_count,
                   sizeof(CanonicalRelationalExpressionRowSlotBinding)) ||
        !add_array(1, sizeof(CanonicalRelationalExpressionRowBinding)) ||
        !add_array(1, sizeof(CanonicalRelationalExpressionRuntime))) {
      return std::nullopt;
    }
  }
  return bytes;
}

std::optional<std::uint64_t>
Rcp079SpatialSourceMaterializationAdditionalBytesV1(
    const std::vector<api::CrudRowVersionRecord>& rows,
    const api::TypedRelationalDag& dag,
    const exec::ModelSourceInputDescriptorV1& source_input,
    const bool has_match,
    const bool has_nearest,
    const std::string_view match_predicate) {
  std::uint64_t bytes = sizeof(api::nosql::SpatialExecutionRequestV2);
  const auto add_array = [&](const std::uint64_t count,
                             const std::uint64_t element_bytes) {
    std::uint64_t allocation = 0;
    return CheckedMultiply(count, element_bytes, &allocation) &&
           CheckedAdd(bytes, allocation, &bytes);
  };
  const auto add_string = [&](const std::string_view value) {
    return CheckedAdd(bytes, value.size(), &bytes) &&
           CheckedAdd(bytes, 1, &bytes);
  };
  const auto operation_phase_bytes =
      [&](const std::string_view operation,
          const std::string_view predicate,
          const bool query_point) -> std::optional<std::uint64_t> {
    std::uint64_t phase = 0;
    const auto add_phase_string = [&](const std::string_view value) {
      return CheckedAdd(phase, value.size(), &phase) &&
             CheckedAdd(phase, 1, &phase);
    };
    if (!add_phase_string(operation) ||
        (!predicate.empty() && !add_phase_string(predicate)) ||
        (query_point &&
         (!add_phase_string(source_input.spatial_crs_uuid) ||
          !CheckedAdd(phase, 24, &phase)))) {
      return std::nullopt;
    }
    return phase;
  };
  std::optional<std::uint64_t> maximum_operation_phase;
  const auto include_operation_phase =
      [&](const std::optional<std::uint64_t> candidate) {
    if (!candidate.has_value()) return false;
    maximum_operation_phase =
        maximum_operation_phase.has_value()
            ? std::max(*maximum_operation_phase, *candidate)
            : *candidate;
    return true;
  };
  if ((!has_match && !has_nearest &&
       !include_operation_phase(operation_phase_bytes(
           "SPATIAL_SOURCE", {}, false))) ||
      (has_match &&
       !include_operation_phase(operation_phase_bytes(
           "SPATIAL_MATCH", match_predicate, true))) ||
      (has_nearest &&
       !include_operation_phase(operation_phase_bytes(
           "SPATIAL_NEAREST", {}, true))) ||
      !maximum_operation_phase.has_value()) {
    return std::nullopt;
  }
  if (!add_array(rows.size(), sizeof(api::nosql::SpatialSourceRowV1)) ||
      !add_string(api::nosql::kSpatialNativeCartesianPoint2dV1) ||
      !add_string(source_input.object_uuid) ||
      !add_string(source_input.spatial_geometry_descriptor_uuid) ||
      !add_string(source_input.spatial_geometry_type_uuid) ||
      !add_string(source_input.spatial_crs_uuid) ||
      !CheckedAdd(bytes, *maximum_operation_phase, &bytes) ||
      !Rcp079AccountLogicalMgaV1(source_input.mga_statement_context, &bytes) ||
      !Rcp079AccountLogicalMgaV1(source_input.mga_statement_context, &bytes)) {
    return std::nullopt;
  }
  if (has_match || has_nearest) {
    constexpr std::uint64_t kNodeLinks = 6 * sizeof(void*);
    std::uint64_t bucket_pointer_count = 0;
    std::uint64_t expression_bucket_pointer_count = 0;
    if (!CheckedMultiply(dag.descriptors.size(), 4,
                         &bucket_pointer_count) ||
        !CheckedMultiply(dag.expressions.size(), 4,
                         &expression_bucket_pointer_count) ||
        !CheckedAdd(bucket_pointer_count,
                    expression_bucket_pointer_count,
                    &bucket_pointer_count) ||
        !add_array(
            dag.descriptors.size(),
            sizeof(std::pair<
                const std::uint32_t,
                const api::RelationalTypeDescriptor*>) + kNodeLinks) ||
        !add_array(
            dag.expressions.size(),
            sizeof(std::pair<
                const std::uint32_t,
                const api::RelationalExpressionRecord*>) + kNodeLinks) ||
        !add_array(dag.descriptors.size(),
                   sizeof(std::pair<const std::uint32_t, std::string>) +
                       kNodeLinks) ||
        !add_array(dag.expressions.size(),
                   sizeof(std::uint32_t) + kNodeLinks) ||
        !add_array(bucket_pointer_count, sizeof(void*)) ||
        !add_array(1, sizeof(CanonicalRelationalExpressionRuntime)) ||
        !add_array(1, sizeof(api::EngineTypedValue)) ||
        !add_array(1, sizeof(std::string))) {
      return std::nullopt;
    }
    for (const auto& descriptor : dag.descriptors) {
      if (!add_string("geometry")) return std::nullopt;
      std::uint64_t encoded_descriptor_bytes =
          std::string_view("type_uuid=").size() + descriptor.type_uuid.size() +
          std::string_view(";nullability=").size();
      const std::string_view nullability =
          descriptor.nullability == api::RelationalNullability::kNonNull
              ? std::string_view("non_null")
              : (descriptor.nullability ==
                         api::RelationalNullability::kNullable
                     ? std::string_view("nullable")
                     : std::string_view("unknown"));
      if (!CheckedAdd(encoded_descriptor_bytes, nullability.size(),
                      &encoded_descriptor_bytes)) {
        return std::nullopt;
      }
      const auto add_optional_descriptor_string =
          [&](const std::string_view label,
              const std::optional<std::string>& value) {
        return !value.has_value() ||
               (CheckedAdd(encoded_descriptor_bytes, label.size(),
                           &encoded_descriptor_bytes) &&
                CheckedAdd(encoded_descriptor_bytes, value->size(),
                           &encoded_descriptor_bytes));
      };
      const auto decimal_digits = [](std::uint32_t value) {
        std::uint64_t digits = 1;
        while (value >= 10) {
          value /= 10;
          ++digits;
        }
        return digits;
      };
      const auto add_optional_descriptor_number =
          [&](const std::string_view label,
              const std::optional<std::uint32_t> value) {
        return !value.has_value() ||
               (CheckedAdd(encoded_descriptor_bytes, label.size(),
                           &encoded_descriptor_bytes) &&
                CheckedAdd(encoded_descriptor_bytes,
                           decimal_digits(*value),
                           &encoded_descriptor_bytes));
      };
      if (!add_optional_descriptor_string(
              ";collation_uuid=", descriptor.collation_uuid) ||
          !add_optional_descriptor_string(
              ";timezone_profile_id=", descriptor.timezone_profile_id) ||
          !add_optional_descriptor_number(";width=", descriptor.width) ||
          !add_optional_descriptor_number(";precision=",
                                          descriptor.precision) ||
          !add_optional_descriptor_number(";scale=", descriptor.scale) ||
          !add_string(descriptor.descriptor_uuid) ||
          !add_string("scalar") || !add_string("geometry") ||
          !CheckedAdd(bytes, encoded_descriptor_bytes, &bytes) ||
          !CheckedAdd(bytes, 1, &bytes)) {
        return std::nullopt;
      }
    }
  }
  for (const auto& row : rows) {
    const std::string* point = nullptr;
    const std::string* crs = nullptr;
    for (const auto& [name, value] : row.values) {
      if (name == "spatial_value") point = &value;
      if (name == "crs_uuid") crs = &value;
    }
    if (point == nullptr || crs == nullptr ||
        !add_string(row.row_uuid) ||
        !CheckedAdd(bytes, point->size(), &bytes) || !add_string(*crs) ||
        !add_string(api::nosql::kSpatialNativeCartesianPoint2dV1)) {
      return std::nullopt;
    }
  }
  return bytes;
}

std::optional<std::uint64_t> Rcp079SpatialResultRowsLogicalMemoryBytesV1(
    const std::vector<api::nosql::SpatialResultRowV1>& rows) {
  std::uint64_t bytes = 0;
  if (!CheckedMultiply(rows.capacity(),
                       sizeof(api::nosql::SpatialResultRowV1), &bytes)) {
    return std::nullopt;
  }
  const auto add_string = [&](const std::string& value) {
    return CheckedAdd(bytes, value.capacity(), &bytes) &&
           CheckedAdd(bytes, 1, &bytes);
  };
  for (const auto& row : rows) {
    if (!add_string(row.row_uuid) ||
        !CheckedAdd(bytes, row.encoded_point.capacity(), &bytes) ||
        !add_string(row.crs_uuid)) {
      return std::nullopt;
    }
  }
  return bytes;
}

std::optional<std::uint64_t> Rcp079SpatialProviderBuildAdditionalBytesV1(
    const std::vector<api::nosql::SpatialResultRowV1>& rows,
    const std::vector<exec::ExecutorColumnDescriptor>& public_columns,
    const exec::ModelSourceInputDescriptorV1& source_input,
    const std::string& property_uuid,
    const std::string& security_receipt_uuid,
    const bool has_match,
    const bool has_nearest) {
  std::uint64_t bytes = sizeof(exec::ModelProviderBatchV1);
  const auto add_array = [&](const std::uint64_t count,
                             const std::uint64_t element_bytes) {
    std::uint64_t allocation = 0;
    return CheckedMultiply(count, element_bytes, &allocation) &&
           CheckedAdd(bytes, allocation, &bytes);
  };
  const auto add_string = [&](const std::string_view value) {
    return CheckedAdd(bytes, value.size(), &bytes) &&
           CheckedAdd(bytes, 1, &bytes);
  };
  std::uint64_t cells = 0;
  if (!CheckedMultiply(rows.size(), public_columns.size(), &cells) ||
      !add_array(public_columns.size(),
                 sizeof(exec::ExecutorColumnDescriptor)) ||
      !add_array(rows.size(), sizeof(exec::DescriptorTuple)) ||
      !add_array(cells, sizeof(api::EngineTypedValue)) ||
      !add_array(rows.size(), sizeof(exec::ModelProviderRowIdentityV1)) ||
      !add_array(source_input.output_descriptor_ids.size(),
                 sizeof(std::uint32_t)) ||
      !add_string(source_input.provider_uuid) ||
      !add_string(source_input.selected_alternative_uuid) ||
      !add_string(source_input.capability_uuid) ||
      !add_string(source_input.result_handle_uuid) ||
      !add_string(property_uuid) || !add_string(security_receipt_uuid) ||
      !add_string("SB_MODEL_PROPERTY_DESCRIPTOR_V1") ||
      !add_string(has_nearest
                      ? "spatial_distance_row_uuid_ascending_v1"
                      : "fixture_order") ||
      !add_string("single_local_partition") || !add_string("row_uuid") ||
      !Rcp079AccountLogicalMgaV1(source_input.mga_statement_context, &bytes)) {
    return std::nullopt;
  }
  std::uint64_t per_cell_descriptor_bytes = 0;
  for (const auto& column : public_columns) {
    if (!add_string(column.stable_name) ||
        !add_string(column.descriptor.descriptor_uuid) ||
        !add_string(column.descriptor.descriptor_kind) ||
        !add_string(column.descriptor.canonical_type_name) ||
        !add_string(column.descriptor.encoded_descriptor)) {
      return std::nullopt;
    }
    const auto add_cell_descriptor_string = [&](const std::string& value) {
      return CheckedAdd(per_cell_descriptor_bytes, value.capacity(),
                        &per_cell_descriptor_bytes) &&
             CheckedAdd(per_cell_descriptor_bytes, 1,
                        &per_cell_descriptor_bytes);
    };
    if (!add_cell_descriptor_string(
            column.descriptor.descriptor_uuid) ||
        !add_cell_descriptor_string(column.descriptor.descriptor_kind) ||
        !add_cell_descriptor_string(column.descriptor.canonical_type_name) ||
        !add_cell_descriptor_string(column.descriptor.encoded_descriptor)) {
      return std::nullopt;
    }
  }
  std::uint64_t cell_descriptor_bytes = 0;
  if (!CheckedMultiply(rows.size(), per_cell_descriptor_bytes,
                       &cell_descriptor_bytes) ||
      !CheckedAdd(bytes, cell_descriptor_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& row : rows) {
    if (!add_string(row.row_uuid)) {
      return std::nullopt;
    }
    if (has_match && !add_string("true")) {
      return std::nullopt;
    }
    if (has_nearest) {
      std::array<char, 64> distance_buffer{};
      std::size_t distance_size = 1;
      if (row.distance == 0.0) {
        distance_buffer[0] = '0';
      } else {
        const auto distance = std::to_chars(
            distance_buffer.data(),
            distance_buffer.data() + distance_buffer.size(), row.distance,
            std::chars_format::general,
            std::numeric_limits<double>::max_digits10);
        if (distance.ec != std::errc{}) return std::nullopt;
        distance_size = static_cast<std::size_t>(
            distance.ptr - distance_buffer.data());
      }
      if (!add_string(std::string_view(distance_buffer.data(),
                                       distance_size))) {
        return std::nullopt;
      }
    }
  }
  return bytes;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalColumnarFamilyJoinQuery(
    const CanonicalCurrentHeapExecutionRequest& input) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  std::vector<const api::RelationalDagNode*> sources;
  const api::RelationalDagNode* join = nullptr;
  for (const auto& node : dag.nodes) {
    if (node.node_kind == api::RelationalDagNodeKind::kScan &&
        node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1") {
      sources.push_back(&node);
    } else if (node.node_kind == api::RelationalDagNodeKind::kJoin &&
               join == nullptr) {
      join = &node;
    }
  }
  if (sources.size() != 2 || join == nullptr || dag.nodes.size() != 3) {
    return result;
  }
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.expression_id == expression_id;
    });
  };
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
      return descriptor.descriptor_id == descriptor_id;
    });
  };
  const auto attached_columnar_source = [&](const auto* source) {
    return std::ranges::count_if(
        source->bound_expression_ids, [&](const auto expression_id) {
          const auto expression = expression_for(expression_id);
          return expression != dag.expressions.end() &&
                 expression->operator_name == "COLUMNAR_SOURCE";
        }) == 1;
  };
  if (!attached_columnar_source(sources[0]) ||
      !attached_columnar_source(sources[1])) {
    return result;
  }
  result.profile_matched = true;
  CanonicalObjectFreeValuesExecutionRequest response_context;
  response_context.context = input.context;
  response_context.relational_dag = dag;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(response_context, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  exec::CanonicalAcceptedJoinKind join_kind;
  std::string join_form;
  std::string join_component;
  if (join->semantic_variant_id.starts_with("join.inner")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kInner;
    join_form = "INNER";
    join_component = "inner";
  } else if (join->semantic_variant_id.starts_with("join.left-outer")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kLeftOuter;
    join_form = "LEFT";
    join_component = "left-outer";
  } else if (join->semantic_variant_id.starts_with("join.right-outer")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kRightOuter;
    join_form = "RIGHT";
    join_component = "right-outer";
  } else if (join->semantic_variant_id.starts_with("join.full-outer")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
    join_form = "FULL";
    join_component = "full-outer";
  } else if (join->semantic_variant_id.starts_with("join.left-semi")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kLeftSemi;
    join_form = "SEMI";
    join_component = "left-semi";
  } else if (join->semantic_variant_id.starts_with("join.left-anti")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kLeftAnti;
    join_form = "ANTI";
    join_component = "left-anti";
  } else if (join->semantic_variant_id == "join.cross.v1") {
    join_kind = exec::CanonicalAcceptedJoinKind::kCross;
    join_form = "CROSS";
    join_component = "cross";
  } else {
    return refuse("SB_MODEL_JOIN_FORM_REFUSED_V1",
                  "columnar composition join form is not executable");
  }
  std::string condition_form = join_form == "CROSS" ? "NONE" : "ON";
  if (join->semantic_variant_id.find(".using.") != std::string::npos) {
    condition_form = "USING";
  } else if (join->semantic_variant_id.find(".natural.") !=
             std::string::npos) {
    condition_form = "NATURAL";
  }
  if (condition_form == "USING" || condition_form == "NATURAL") {
    return refuse(
        condition_form == "USING"
            ? "SB_MODEL_JOIN_USING_BINDING_REFUSED_V1"
            : "SB_MODEL_JOIN_NATURAL_BINDING_REFUSED_V1",
        "columnar USING/NATURAL join lacks binder-owned named bindings and "
        "a coalescing projection");
  }
  const bool predicate_join = join_form != "CROSS";
  const bool left_only = join_form == "SEMI" || join_form == "ANTI";
  std::vector<std::uint32_t> expected_join_descriptors =
      sources[0]->output_descriptor_ids;
  if (!left_only) {
    expected_join_descriptors.insert(expected_join_descriptors.end(),
                                     sources[1]->output_descriptor_ids.begin(),
                                     sources[1]->output_descriptor_ids.end());
  }
  if (dag.wire_version != 2 || dag.root_node_id != join->node_id ||
      join->input_node_ids !=
          std::vector<std::uint32_t>{sources[0]->node_id,
                                     sources[1]->node_id} ||
      join->output_descriptor_ids != expected_join_descriptors ||
      join->bound_expression_ids.size() !=
          static_cast<std::size_t>(predicate_join) ||
      dag.statement_timestamp.empty() ||
      dag.statement_timestamp != input.context.statement_timestamp) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "columnar composition DAG shape is incomplete");
  }
  opt::ModelFamilyJoinAdmissionRequestV1 pair_request;
  pair_request.left_family_id = "columnar";
  pair_request.right_family_id = "columnar";
  pair_request.join_form_id = join_form;
  pair_request.condition_form_id = condition_form;
  const auto pair_admission =
      opt::CoordinateModelFamilyJoinAdmissionV1(pair_request);
  if (!pair_admission.accepted ||
      !pair_admission.root_publication_allowed) {
    return refuse(pair_admission.diagnostic_id, pair_admission.detail);
  }

  api::EngineResolveStatementSnapshotRequest snapshot_request;
  snapshot_request.context = input.context;
  const auto snapshot = api::EngineResolveStatementSnapshot(snapshot_request);
  if (!snapshot.ok) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "columnar composition statement snapshot is unavailable");
  }
  auto mga = PhysicalMgaContextFromResolvedSnapshot(
      input.context, snapshot.snapshot_vector);
  mga.statement_timestamp = input.context.statement_timestamp;
  if (!exec::PhysicalMgaStatementContextValid(mga)) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "columnar composition statement context is invalid");
  }

  const auto identity_scope =
      dag.bound_sblr_tree_uuid + ":" + input.context.statement_uuid;
  const auto shared_source_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "columnar-join.source.capability");
  std::vector<Rcp079ColumnarJoinSourceV1> prepared_sources;
  prepared_sources.reserve(2);
  std::vector<std::string> object_uuids;
  std::unordered_set<std::string> column_uuids;
  std::unordered_set<std::string> descriptor_uuids;
  std::unordered_set<std::string> type_uuids;
  for (const auto* source : sources) {
    if (!source->input_node_ids.empty() ||
        source->required_object_uuids.size() != 1 ||
        source->output_descriptor_ids.empty() ||
        source->output_descriptor_ids.size() > 256) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "columnar source leg is incomplete");
    }
    Rcp079ColumnarJoinSourceV1 prepared;
    prepared.logical_node_id = source->node_id;
    prepared.object_uuid = source->required_object_uuids.front();
    const auto authorization = api::EvaluateMaterializedAuthorization(
        input.context, input.context.authorization_context, "SELECT",
        prepared.object_uuid);
    if (!authorization.authorized || authorization.denied ||
        authorization.policy_recheck_required ||
        !authorization.diagnostics.empty()) {
      return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                    "columnar composition SELECT authorization was refused");
    }
    const auto loaded =
        api::LoadMgaRelationStorageDescriptor(input.context,
                                              prepared.object_uuid);
    if (!loaded.ok) {
      return refuse("SB_MODEL_COLUMNAR_EXACT_FALLBACK_UNAVAILABLE_V1",
                    loaded.diagnostic.detail);
    }
    prepared.persisted = loaded.descriptor;
    if (prepared.persisted.relation_uuid != prepared.object_uuid ||
        prepared.persisted.database_uuid !=
            input.context.database_uuid ||
        !CanonicalUuidText(prepared.persisted.schema_uuid) ||
        !CanonicalUuidText(
            prepared.persisted.descriptor_uuid) ||
        prepared.persisted.relation_kind != "table" ||
        prepared.persisted.storage_profile != "local_mga_rowstore_v1" ||
        prepared.persisted.descriptor_generation == 0 ||
        (prepared.persisted.descriptor_status != "production_descriptor" &&
         prepared.persisted.descriptor_status !=
             "metadata_bridge_vetted_descriptor") ||
        prepared.persisted.columns.size() !=
            source->output_descriptor_ids.size()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "columnar composition descriptor is invalid");
    }
    std::vector<const api::RelationalOutputRecord*> outputs;
    for (const auto& output : dag.outputs) {
      if (output.relation_node_id == source->node_id) {
        outputs.push_back(&output);
      }
    }
    std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
    if (outputs.size() != source->output_descriptor_ids.size()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "columnar source outputs are incomplete");
    }
    for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
      const auto descriptor = descriptor_for(outputs[ordinal]->descriptor_id);
      const auto expression = expression_for(outputs[ordinal]->expression_id);
      const auto& column = prepared.persisted.columns[ordinal];
      if (descriptor == dag.descriptors.end() ||
          expression == dag.expressions.end() ||
          outputs[ordinal]->ordinal != ordinal || !outputs[ordinal]->visible ||
          outputs[ordinal]->descriptor_id !=
              source->output_descriptor_ids[ordinal] ||
          expression->expression_kind !=
              api::RelationalExpressionKind::kIdentifier ||
          expression->result_descriptor_id != descriptor->descriptor_id ||
          expression->bound_name_uuid !=
              std::optional<std::string>(column.column_uuid) ||
          outputs[ordinal]->output_name_utf8 != column.canonical_name_key ||
          column.ordinal != ordinal ||
          !column_uuids.insert(column.column_uuid).second ||
          !descriptor_uuids.insert(descriptor->descriptor_uuid).second ||
          !Rcp079ExactColumnarJoinColumnBindingV1(input.context, *descriptor,
                                                  column)) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "columnar source binding was substituted");
      }
      type_uuids.insert(descriptor->type_uuid);
      auto engine_descriptor = column.value_descriptor;
      engine_descriptor.descriptor_kind = "scalar";
      prepared.columns.push_back(
          {column.canonical_name_key, std::move(engine_descriptor),
           column.nullable, descriptor->descriptor_id});
      prepared.output_expression_ids.push_back(
          outputs[ordinal]->expression_id);
    }
    if (source->bound_expression_ids.size() != outputs.size() + 1 ||
        std::ranges::any_of(prepared.output_expression_ids,
                            [&](const auto expression_id) {
          return std::ranges::count(source->bound_expression_ids,
                                    expression_id) != 1;
        })) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "columnar source expression binding is incomplete");
    }
    prepared.output_descriptor_ids = source->output_descriptor_ids;
    const auto suffix = std::to_string(source->node_id);
    prepared.capability_uuid = shared_source_capability_uuid;
    prepared.provider_uuid = DerivedCanonicalUuid(
        identity_scope, "columnar-join.provider." + suffix);
    prepared.result_handle_uuid = DerivedCanonicalUuid(
        identity_scope, "columnar-join.result-handle." + suffix);
    prepared.property_uuid = DerivedCanonicalUuid(
        identity_scope, "columnar-join.property." + suffix);
    prepared.security_receipt_uuid = DerivedCanonicalUuid(
        identity_scope, "columnar-join.security-receipt." + suffix);
    object_uuids.push_back(prepared.object_uuid);
    prepared_sources.push_back(std::move(prepared));
  }
  if (object_uuids[0] == object_uuids[1]) {
    return refuse("SB_MODEL_JOIN_SEMANTIC_PRECONDITION_REFUSED_V1",
                  "columnar composition requires distinct bound legs");
  }
  if (std::ranges::any_of(descriptor_uuids, [&](const auto& descriptor_uuid) {
        return type_uuids.contains(descriptor_uuid);
      })) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "columnar descriptor and type identity domains overlap");
  }

  api::CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid = input.context.catalog_epoch_uuid;
  planning_scope.security_context_uuid =
      input.context.authorization_context.authority_uuid;
  planning_scope.statement_uuid = input.context.statement_uuid;
  planning_scope.statement_timestamp = input.context.statement_timestamp;
  planning_scope.owning_transaction_uuid = input.context.transaction_uuid;
  planning_scope.statement_snapshot_uuid =
      input.context.statement_snapshot_uuid;
  planning_scope.statement_metadata_snapshot_uuid =
      input.context.statement_metadata_snapshot_uuid;
  planning_scope.local_transaction_id = input.context.local_transaction_id;
  planning_scope.snapshot_visible_through_local_transaction_id =
      input.context.snapshot_visible_through_local_transaction_id;
  planning_scope.metadata_snapshot_engine_owned =
      input.context.statement_metadata_snapshot_engine_owned;
  planning_scope.authorization_context_engine_owned =
      input.context.authorization_context.present;
  auto logical = api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
      dag, planning_scope);
  if (!logical.accepted || logical.logical_graph.nodes.size() != 3 ||
      logical.logical_graph.nodes[0].model_family_identity !=
          plan::CanonicalLogicalModelFamilyIdentity::kColumnar ||
      logical.logical_graph.nodes[1].model_family_identity !=
          plan::CanonicalLogicalModelFamilyIdentity::kColumnar ||
      logical.logical_graph.nodes[2].model_family_identity !=
          plan::CanonicalLogicalModelFamilyIdentity::kUnspecified) {
    return refuse(logical.issues.empty()
                      ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
                      : logical.issues.front().diagnostic_id,
                  logical.issues.empty()
                      ? "columnar composition logical bridge was refused"
                      : logical.issues.front().field_id);
  }
  const auto registered_logical_mga = Rcp079LogicalMga(mga, false);
  const auto current_logical_mga = Rcp079LogicalMga(mga, true);
  if (!plan::CanonicalMgaStatementContextEqual(
          logical.logical_graph.mga_statement_context,
          registered_logical_mga) ||
      !plan::CanonicalMgaStatementContextEqual(
          logical.property_catalog.mga_statement_context,
          registered_logical_mga)) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "columnar composition logical MGA cohort changed");
  }
  logical.logical_graph.mga_statement_context = current_logical_mga;
  logical.property_catalog.mga_statement_context = current_logical_mga;

  opt::CanonicalNativeObjectAdmissionContext admission_context;
  admission_context.statement_uuid = input.context.statement_uuid;
  admission_context.catalog_snapshot_uuid =
      input.context.statement_metadata_snapshot_uuid;
  admission_context.security_context_uuid =
      input.context.authorization_context.authority_uuid;
  admission_context.catalog_generation = input.context.catalog_generation_id;
  admission_context.authorization_catalog_generation =
      input.context.authorization_context.catalog_generation_id;
  admission_context.security_epoch =
      input.context.authorization_context.security_epoch;
  admission_context.policy_epoch =
      input.context.authorization_context.policy_epoch;
  admission_context.resource_epoch = input.context.resource_epoch;
  admission_context.capability_snapshot_uuid =
      input.context.optimizer_capability_snapshot_uuid;
  admission_context.resource_snapshot_uuid =
      input.context.optimizer_resource_snapshot_uuid;
  admission_context.route_snapshot_uuid =
      input.context.optimizer_route_snapshot_uuid;
  admission_context.route_epoch = input.context.optimizer_route_epoch;
  admission_context.route_generation = input.context.optimizer_route_generation;
  admission_context.memory_budget_bytes =
      input.context.optimizer_memory_budget_bytes;
  admission_context.maximum_candidate_count =
      input.context.optimizer_maximum_candidate_count;
  admission_context.maximum_memo_groups =
      input.context.optimizer_maximum_memo_groups;
  admission_context.maximum_search_steps =
      input.context.optimizer_maximum_search_steps;
  admission_context.maximum_planning_time_ns =
      input.context.optimizer_maximum_planning_time_ns;
  admission_context.spill_allowed = input.context.optimizer_spill_allowed;
  admission_context.local_transaction_id = input.context.local_transaction_id;
  admission_context.statement_snapshot_id =
      input.context.snapshot_visible_through_local_transaction_id;
  admission_context.mga_statement_context = current_logical_mga;
  std::uint64_t admitted_at_monotonic_ns = 0;
  const auto monotonic = std::from_chars(
      input.context.current_monotonic_ns.data(),
      input.context.current_monotonic_ns.data() +
          input.context.current_monotonic_ns.size(),
      admitted_at_monotonic_ns);
  if (monotonic.ec != std::errc{} ||
      monotonic.ptr != input.context.current_monotonic_ns.data() +
                           input.context.current_monotonic_ns.size() ||
      admitted_at_monotonic_ns == 0) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "columnar composition monotonic context is invalid");
  }
  admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
  admission_context.metadata_snapshot_engine_owned = true;
  admission_context.authorization_context_engine_owned = true;
  admission_context.catalog_object_uuids = object_uuids;
  admission_context.authorized_object_uuids = object_uuids;
  admission_context.catalog_object_evidence_engine_owned = true;
  admission_context.authorization_object_evidence_engine_owned = true;
  auto canonical_admission =
      opt::BuildCanonicalObjectAwareNativeOptimizerAdmissionRequest(
          logical.logical_graph, logical.property_catalog, admission_context);
  if (!canonical_admission.built ||
      !canonical_admission.admission.admitted ||
      !canonical_admission.admission.planning_allowed ||
      canonical_admission.admission.data_access_allowed) {
    return refuse(canonical_admission.diagnostic_id.empty()
                      ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
                      : canonical_admission.diagnostic_id,
                  canonical_admission.field_id.empty()
                      ? "columnar composition optimizer admission failed"
                      : canonical_admission.field_id);
  }
  result.optimizer_admitted = true;
  result.optimizer_admission_stage_count =
      canonical_admission.admission.evidence.size();
  CanonicalObjectFreeValuesExecutionRequest planning_request{
      input.context, dag, canonical_admission.request,
      canonical_admission.admission};

  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& source : prepared_sources) {
    LivePhysicalNodeProfile profile;
    profile.logical_node_id = source.logical_node_id;
    profile.implementation_id = source.implementation_id;
    profile.capability_uuid = source.capability_uuid;
    profile.logical_node_kind =
        plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
    profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
    profile.transformation_rule_id =
        "canonical.columnar.reconstruction.v1";
    profile.estimated_rows = 1;
    profile.memory_bytes_required = std::max<std::uint64_t>(
        4096, input.context.optimizer_memory_budget_bytes / 8);
    profile.page_read_sequential_units = 1;
    profile.mga_visibility_checks_expected = 1;
    profile.storage_read_capable = true;
    profile.mga_visibility_capable = true;
    profile.storage_recheck_required = true;
    profile.compatibility_profile_id = "columnar.local.v1";
    profiles.push_back(std::move(profile));
  }
  const auto join_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "columnar-join." + join_component + ".capability");
  LivePhysicalNodeProfile join_profile;
  join_profile.logical_node_id = join->node_id;
  join_profile.implementation_id =
      "join." + join_component + ".3vl.nested.v1";
  join_profile.capability_uuid = join_capability_uuid;
  join_profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kJoin;
  join_profile.physical_node_kind = exec::PhysicalNodeKind::kJoin;
  join_profile.transformation_rule_id =
      "canonical.model-family.join." + join_component + ".v1";
  join_profile.estimated_rows = 1;
  join_profile.memory_bytes_required = std::max<std::uint64_t>(
      4096, input.context.optimizer_memory_budget_bytes / 4);
  join_profile.minimum_input_count = 2;
  join_profile.maximum_input_count = 2;
  join_profile.runtime_peak_from_callback_batches = true;
  profiles.push_back(std::move(join_profile));
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "columnar composition memory receipts are incomplete");
  }
  const auto physical = PlanAndPublishLivePhysicalDag(
      planning_request, profiles, "columnar-join.selected-plan",
      "columnar model-family join", "columnar.local.v1");
  if (!physical.ok || physical.physical_dag.nodes.size() != 3) {
    return refuse(physical.diagnostic_id.empty()
                      ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
                      : physical.diagnostic_id,
                  physical.detail.empty()
                      ? "columnar composition physical DAG was not published"
                      : physical.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = physical.physical_dag.nodes.size();
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

  const auto maximum_rows = static_cast<std::size_t>(std::min<std::uint64_t>(
      65'536, input.context.optimizer_maximum_candidate_count));
  const auto maximum_columns = join->output_descriptor_ids.size();
  std::uint64_t maximum_pairs_u64 = 0;
  std::uint64_t maximum_cells = 0;
  std::uint64_t maximum_total_rows = 0;
  std::uint64_t maximum_total_cells = 0;
  if (maximum_rows == 0 || maximum_columns == 0 ||
      !CheckedMultiply(maximum_rows, maximum_rows, &maximum_pairs_u64) ||
      maximum_pairs_u64 > std::numeric_limits<std::size_t>::max() ||
      !CheckedMultiply(maximum_rows, maximum_columns, &maximum_cells) ||
      !CheckedMultiply(maximum_rows, 3, &maximum_total_rows) ||
      !CheckedMultiply(maximum_cells, 3, &maximum_total_cells) ||
      maximum_cells > std::numeric_limits<std::size_t>::max() ||
      maximum_total_rows > std::numeric_limits<std::size_t>::max() ||
      maximum_total_cells > std::numeric_limits<std::size_t>::max()) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "columnar composition execution bounds overflowed");
  }
  CanonicalRelationalExpressionRowBinding predicate_binding;
  std::string predicate_detail;
  if (predicate_join) {
    std::vector<std::uint32_t> input_descriptors =
        sources[0]->output_descriptor_ids;
    input_descriptors.insert(input_descriptors.end(),
                             sources[1]->output_descriptor_ids.begin(),
                             sources[1]->output_descriptor_ids.end());
    if (!PrepareInputRowBindingForComposition(
            dag, join->bound_expression_ids.front(), input_descriptors,
            &predicate_binding, &predicate_detail)) {
      return refuse("SB_MODEL_JOIN_SEMANTIC_PRECONDITION_REFUSED_V1",
                    predicate_detail);
    }
  }

  exec::CanonicalPhysicalExecutorRegistration source_registration;
  source_registration.node_kind = exec::PhysicalNodeKind::kScan;
  source_registration.implementation_id =
      "physical_columnar_zone_scan_v1";
  source_registration.executor_capability_uuid.clear();
  source_registration.executor_capability_abi_version = 1;
  source_registration.engine_owned = true;
  source_registration.accepts_optimizer_publication_v2 = true;
  source_registration.publishes_runtime_observation_v1 = true;
  const auto context = input.context;
  const auto cancellation_requested =
      input.context.query_cancellation_requested
          ? input.context.query_cancellation_requested
          : std::function<bool()>([] { return false; });
  std::vector<exec::ExecutorColumnDescriptor> expected_join_columns;
  expected_join_columns.reserve(join->output_descriptor_ids.size());
  for (const auto& source : prepared_sources) {
    expected_join_columns.insert(expected_join_columns.end(),
                                 source.columns.begin(), source.columns.end());
  }
  const auto left_width = prepared_sources[0].columns.size();
  source_registration.execute =
      [prepared_sources, context, cancellation_requested, mga,
       maximum_rows](const exec::TypedPhysicalNodeDag& selected_dag,
                     const exec::PhysicalNodeRecord& selected_node,
                     const std::vector<exec::CanonicalPhysicalDispatchInput>&
                         inputs) mutable {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = selected_dag.selected_plan_uuid;
        step.executed_physical_node_id = selected_node.physical_node_id;
        step.causal_counter_id = selected_node.causal_counter_id;
        step.output_descriptor_ids = selected_node.output_descriptor_ids;
        step.mga_statement_context = selected_dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        const auto source = std::ranges::find_if(
            prepared_sources, [&](const auto& candidate) {
              return candidate.logical_node_id == selected_node.relational_node_id;
            });
        if (!inputs.empty() || source == prepared_sources.end() ||
            selected_node.memory_bytes_required == 0 ||
            selected_node.memory_bytes_required >
                selected_dag.memory_budget_bytes ||
            selected_node.implementation_id != source->implementation_id ||
            selected_node.executor_capability_uuid != source->capability_uuid ||
            selected_node.output_descriptor_ids !=
                source->output_descriptor_ids ||
            !exec::PhysicalMgaStatementContextEqual(
                selected_dag.mga_statement_context, mga)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
          step.diagnostic.detail =
              "selected columnar composition source was substituted";
          return step;
        }
        exec::ModelFamilyExecutionRequestV1 request;
        auto& source_input = request.input;
        source_input.family_id = "columnar";
        source_input.operation_ids = {"COLUMNAR_SOURCE"};
        source_input.operation_id = "COLUMNAR_SOURCE";
        source_input.object_uuid = source->object_uuid;
        source_input.physical_node_id = selected_node.physical_node_id;
        source_input.selected_alternative_uuid =
            selected_node.selected_alternative_uuid;
        source_input.capability_uuid = source->capability_uuid;
        source_input.provider_uuid = source->provider_uuid;
        source_input.provider_generation =
            source->persisted.descriptor_generation;
        source_input.result_handle_uuid = source->result_handle_uuid;
        source_input.causal_counter_id = selected_node.causal_counter_id;
        source_input.output_descriptor_ids = source->output_descriptor_ids;
        source_input.mga_statement_context = mga;
        source_input.catalog_epoch_uuid = context.catalog_epoch_uuid;
        source_input.security_context_uuid =
            context.authorization_context.authority_uuid;
        source_input.policy_snapshot_uuid = source->property_uuid;
        source_input.resource_contract_uuid = source->security_receipt_uuid;
        source_input.catalog_generation = context.catalog_generation_id;
        source_input.descriptor_generation =
            source->persisted.descriptor_generation;
        source_input.security_generation = context.security_epoch;
        source_input.policy_generation =
            context.authorization_context.policy_epoch;
        source_input.resource_generation = context.resource_epoch;
        source_input.maximum_rows = maximum_rows;
        std::uint64_t source_maximum_cells = 0;
        if (!CheckedMultiply(maximum_rows, source->columns.size(),
                             &source_maximum_cells) ||
            source_maximum_cells >
                std::numeric_limits<std::size_t>::max()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
          step.diagnostic.detail =
              "columnar composition source cell bound overflowed";
          return step;
        }
        source_input.maximum_cells =
            static_cast<std::size_t>(source_maximum_cells);
        source_input.maximum_memory_bytes =
            selected_node.memory_bytes_required;
        source_input.exact_fallback_selected = true;
        request.capability.capability_uuid = source->capability_uuid;
        request.capability.family_id = "columnar";
        request.capability.provider_uuid = source->provider_uuid;
        request.capability.provider_generation =
            source->persisted.descriptor_generation;
        request.capability.available = true;
        request.capability.exact = true;
        request.capability.exact_collection_fallback_available = true;
        request.capability.cancellation_supported = true;
        request.capability.cleanup_supported = true;
        request.capability.residual_recheck_supported = true;
        request.capability.base_row_mga_recheck_supported = true;
        request.capability.security_recheck_supported = true;
        request.cancellation_requested = cancellation_requested;
        request.cleanup_provider = [] {};
        request.exact_fallback_selected = true;
        request.security_admitted = true;
        request.current_catalog_generation = context.catalog_generation_id;
        request.current_descriptor_generation =
            source->persisted.descriptor_generation;
        request.current_security_generation = context.security_epoch;
        request.current_policy_generation =
            context.authorization_context.policy_epoch;
        request.current_resource_generation = context.resource_epoch;
        request.current_provider_generation =
            source->persisted.descriptor_generation;
        request.current_mga_statement_context = mga;
        const auto source_copy = *source;
        Rcp079ColumnarSourceRuntimeMemoryReceiptV1
            source_runtime_memory_receipt;
        request.execute_provider =
            [context, source_copy, cancellation_requested,
             &source_runtime_memory_receipt](
                const exec::ModelSourceInputDescriptorV1& source_input) {
              exec::ModelProviderExecutionResultV1 provider;
              const auto fail = [&](std::string diagnostic,
                                    std::string detail) {
                provider.diagnostic_id = std::move(diagnostic);
                provider.detail = std::move(detail);
                return provider;
              };
              if (cancellation_requested()) {
                return fail("SB_MODEL_EXECUTION_CANCELLED_V1",
                            "columnar composition was cancelled before source revalidation");
              }
              const auto authorization = api::EvaluateMaterializedAuthorization(
                  context, context.authorization_context, "SELECT",
                  source_input.object_uuid);
              if (!authorization.authorized || authorization.denied ||
                  authorization.policy_recheck_required ||
                  !authorization.diagnostics.empty()) {
                return fail("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                            "columnar composition authorization changed");
              }
              const auto preflight = api::LoadMgaRelationStorageDescriptor(
                  context, source_input.object_uuid);
              if (!preflight.ok ||
                  !Rcp079ExactColumnarJoinStorageSnapshotV1(
                      preflight.descriptor, source_copy.persisted)) {
                return fail("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                            "columnar composition descriptor changed before MGA access");
              }
              if (cancellation_requested()) {
                return fail("SB_MODEL_EXECUTION_CANCELLED_V1",
                            "columnar composition was cancelled before MGA access");
              }
              api::MgaVisibleHeapRelationReadRequest read_request;
              read_request.relation_uuid = source_input.object_uuid;
              read_request.maximum_scanned_row_versions =
                  context.optimizer_maximum_search_steps;
              read_request.maximum_decoded_bytes =
                  source_input.maximum_memory_bytes / 2;
              read_request.maximum_output_rows = source_input.maximum_rows;
              read_request.cancellation_requested = cancellation_requested;
              if (read_request.maximum_scanned_row_versions == 0 ||
                  read_request.maximum_decoded_bytes == 0 ||
                  read_request.maximum_output_rows == 0) {
                return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                            "columnar composition MGA read bound is zero");
              }
              const auto read =
                  api::ReadVisibleMgaHeapRelation(context, read_request);
              provider.data_access_observed =
                  read.ok || read.scanned_row_version_count != 0 ||
                  read.decoded_byte_count != 0;
              provider.rows_examined = read.scanned_row_version_count;
              if (!read.ok) {
                return fail(read.diagnostic.code, read.diagnostic.detail);
              }
              if (!Rcp079ExactColumnarJoinStorageSnapshotV1(
                      read.descriptor, preflight.descriptor) ||
                  !Rcp079ExactColumnarJoinStorageSnapshotV1(
                      read.descriptor, source_copy.persisted) ||
                  read.current_relation_base_generation == 0) {
                return fail("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                            "columnar composition descriptor changed");
              }
              if (cancellation_requested()) {
                return fail("SB_MODEL_EXECUTION_CANCELLED_V1",
                            "columnar composition was cancelled after MGA access");
              }
              const auto retained_visible_row_memory =
                  Rcp079VisibleRowsMemoryBytesV1(read.visible_rows);
              const api::TypedRelationalDag source_only_dag;
              const auto materialization_memory =
                  Rcp079ColumnarLogicalMaterializationAdditionalBytesV1(
                      source_copy.persisted, read.visible_rows.size(),
                      source_only_dag, source_input,
                      source_copy.property_uuid,
                      source_copy.security_receipt_uuid, false);
              std::uint64_t retained_and_materialized = 0;
              std::uint64_t projected_live_memory = 0;
              if (!retained_visible_row_memory.has_value() ||
                  *retained_visible_row_memory >=
                      source_input.maximum_memory_bytes ||
                  !materialization_memory.has_value() ||
                  !CheckedAdd(*retained_visible_row_memory,
                              *materialization_memory,
                              &retained_and_materialized) ||
                  !CheckedMultiply(retained_and_materialized, 2,
                                   &projected_live_memory) ||
                  projected_live_memory >
                      source_input.maximum_memory_bytes) {
                return fail(
                    "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "columnar source materialization exceeds the selected-node grant");
              }
              const auto post_authorization =
                  api::EvaluateMaterializedAuthorization(
                      context, context.authorization_context, "SELECT",
                      source_input.object_uuid);
              if (!post_authorization.authorized ||
                  post_authorization.denied ||
                  post_authorization.policy_recheck_required ||
                  !post_authorization.diagnostics.empty()) {
                return fail("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                            "columnar composition authorization changed after MGA access");
              }
              exec::DescriptorBatch logical_rows;
              logical_rows.columns = source_copy.columns;
              std::vector<std::string> row_uuids;
              logical_rows.rows.reserve(read.visible_rows.size());
              row_uuids.reserve(read.visible_rows.size());
              for (const auto& row : read.visible_rows) {
                if (cancellation_requested()) {
                  return fail("SB_MODEL_EXECUTION_CANCELLED_V1",
                              "columnar composition row reconstruction was cancelled");
                }
                if (row.table_uuid != source_input.object_uuid ||
                    !CanonicalUuidText(row.row_uuid) ||
                    !CanonicalUuidText(row.version_uuid) ||
                    row.values.size() != source_copy.persisted.columns.size()) {
                  return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                              "columnar row identity or width is invalid");
                }
                exec::DescriptorTuple tuple;
                tuple.values.reserve(source_copy.persisted.columns.size());
                for (std::size_t ordinal = 0;
                     ordinal < source_copy.persisted.columns.size();
                     ++ordinal) {
                  const auto& column =
                      source_copy.persisted.columns[ordinal];
                  const std::string* encoded = nullptr;
                  for (const auto& [name, value] : row.values) {
                    if (name == column.canonical_name_key) {
                      if (encoded != nullptr) {
                        return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                                    "columnar row repeats a field");
                      }
                      encoded = &value;
                    }
                  }
                  if (encoded == nullptr) {
                    return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                                "columnar row omits a field");
                  }
                  if (*encoded == "<NULL>" && !column.nullable) {
                    return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                                "columnar row nulls a non-null field");
                  }
                  api::EngineTypedValue value;
                  value.descriptor = source_copy.columns[ordinal].descriptor;
                  if (*encoded == "<NULL>") {
                    value.setState(api::EngineValueState::sql_null);
                  } else {
                    value.setState(api::EngineValueState::value);
                    value.encoded_value = *encoded;
                  }
                  tuple.values.push_back(std::move(value));
                }
                logical_rows.rows.push_back(std::move(tuple));
                row_uuids.push_back(row.row_uuid);
              }
              if (cancellation_requested()) {
                return fail("SB_MODEL_EXECUTION_CANCELLED_V1",
                            "columnar composition was cancelled before reconstruction");
              }
              api::nosql::ColumnarExecutionRequestV2 columnar_request;
              columnar_request.operation_ids = {"COLUMNAR_SOURCE"};
              columnar_request.operation_id = "COLUMNAR_SOURCE";
              columnar_request.relation_uuid = source_input.object_uuid;
              columnar_request.row_uuids = std::move(row_uuids);
              columnar_request.logical_rows = std::move(logical_rows);
              columnar_request.statement_context =
                  source_input.mga_statement_context;
              columnar_request.current_statement_context =
                  source_input.mga_statement_context;
              columnar_request.source_generation =
                  source_input.descriptor_generation;
              columnar_request.catalog_generation =
                  source_input.catalog_generation;
              columnar_request.summary_generation =
                  source_input.provider_generation;
              columnar_request.maximum_rows = source_input.maximum_rows;
              columnar_request.maximum_input_cells =
                  source_input.maximum_cells;
              columnar_request.maximum_cells = source_input.maximum_cells;
              columnar_request.maximum_memory_bytes =
                  source_input.maximum_memory_bytes -
                  *retained_visible_row_memory;
              columnar_request.cancellation_requested = [](const void* context) {
                return (*static_cast<const std::function<bool()>*>(context))();
              };
              columnar_request.cancellation_context = &cancellation_requested;
              columnar_request.security_admitted = true;
              columnar_request.exact_reconstruction_fallback_available = true;
              const auto pre_api_batch_memory =
                  Rcp079DescriptorBatchLogicalMemoryBytesV1(
                      columnar_request.logical_rows);
              const auto pre_api_request_memory =
                  Rcp079ColumnarRequestNonBatchLogicalMemoryBytesV2(
                      columnar_request);
              std::uint64_t pre_api_peak_memory = 0;
              if (!pre_api_batch_memory.has_value() ||
                  !pre_api_request_memory.has_value() ||
                  !CheckedAdd(*retained_visible_row_memory,
                              *pre_api_batch_memory,
                              &pre_api_peak_memory) ||
                  !CheckedAdd(pre_api_peak_memory,
                              *pre_api_request_memory,
                              &pre_api_peak_memory) ||
                  pre_api_peak_memory >
                      source_input.maximum_memory_bytes) {
                return fail(
                    "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "columnar source request exceeded the selected-node grant");
              }
              auto reconstructed =
                  api::nosql::ExecuteColumnarLogicalV2(
                      std::move(columnar_request));
              if (!reconstructed.accepted ||
                  !reconstructed.root_publishable) {
                return fail(reconstructed.diagnostic_id.empty()
                                ? "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"
                                : reconstructed.diagnostic_id,
                            reconstructed.detail.empty()
                                ? "columnar source reconstruction did not publish"
                                : reconstructed.detail);
              }
              if (!reconstructed.exact_fallback_selected ||
                  !reconstructed.exact_reconstruction_complete ||
                  !reconstructed.predicate_recheck_complete ||
                  !reconstructed.mga_recheck_complete ||
                  reconstructed.cancellation_observed ||
                  reconstructed.cancellation_probe_failed ||
                  !reconstructed.memory_receipt_complete ||
                  reconstructed.current_live_memory_bytes >
                      reconstructed.peak_live_memory_bytes ||
                  reconstructed.peak_live_memory_bytes >
                      reconstructed.memory_grant_bytes ||
                  reconstructed.memory_grant_bytes !=
                      source_input.maximum_memory_bytes -
                          *retained_visible_row_memory ||
                  reconstructed.physical_operator_id !=
                      "COLUMNAR_ROW_RECONSTRUCTION_SCAN_V1" ||
                  !reconstructed.fallback_reason_id.empty() ||
                  reconstructed.diagnostic_id != "SB_EXECUTOR_OK" ||
                  !reconstructed.detail.empty() ||
                  reconstructed.row_uuids.size() !=
                      read.visible_rows.size() ||
                  reconstructed.batch.rows.size() !=
                      read.visible_rows.size() ||
                  reconstructed.batch.rows.size() >
                      source_input.maximum_rows ||
                  reconstructed.batch.columns.size() !=
                      source_copy.columns.size() ||
                  !std::ranges::equal(
                      reconstructed.batch.columns, source_copy.columns,
                      [](const auto& actual, const auto& expected) {
                        return Rcp079ExactExecutorColumnV1(actual,
                                                          expected);
                      })) {
                return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "columnar source reconstruction receipt changed");
              }
              std::uint64_t combined_peak_memory = 0;
              if (!CheckedAdd(*retained_visible_row_memory,
                              reconstructed.peak_live_memory_bytes,
                              &combined_peak_memory) ||
                  combined_peak_memory >
                      source_input.maximum_memory_bytes) {
                return fail(
                    "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "columnar source retained rows and reconstruction peak exceed the selected-node grant");
              }
              if (reconstructed.batch.rows.size() != 0 &&
                  reconstructed.batch.columns.size() >
                      source_input.maximum_cells /
                          reconstructed.batch.rows.size()) {
                return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                            "columnar source reconstruction cell receipt exceeded its bound");
              }
              for (std::size_t row = 0;
                   row < read.visible_rows.size(); ++row) {
                const auto& source_row = read.visible_rows[row];
                const auto& actual_row = reconstructed.batch.rows[row];
                if (reconstructed.row_uuids[row] != source_row.row_uuid ||
                    actual_row.values.size() !=
                        source_copy.persisted.columns.size()) {
                  return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                              "columnar source row identity or width changed");
                }
                for (std::size_t ordinal = 0;
                     ordinal < source_copy.persisted.columns.size();
                     ++ordinal) {
                  const auto& persisted_column =
                      source_copy.persisted.columns[ordinal];
                  const std::string* expected_encoded = nullptr;
                  for (const auto& [name, value] : source_row.values) {
                    if (name == persisted_column.canonical_name_key) {
                      if (expected_encoded != nullptr) {
                        return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                                    "columnar source row repeats a field during receipt replay");
                      }
                      expected_encoded = &value;
                    }
                  }
                  if (expected_encoded == nullptr) {
                    return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                                "columnar source row omitted a field during receipt replay");
                  }
                  const auto& actual = actual_row.values[ordinal];
                  const bool expected_null = *expected_encoded == "<NULL>";
                  if (!CanonicalQueryEngineDescriptorExactlyEqual(
                          actual.descriptor,
                          source_copy.columns[ordinal].descriptor) ||
                      (expected_null
                           ? (actual.state !=
                                  api::EngineValueState::sql_null ||
                              !actual.is_null ||
                              !actual.encoded_value.empty() ||
                              !actual.binary_value.empty())
                           : (actual.state != api::EngineValueState::value ||
                              actual.is_null ||
                              actual.encoded_value != *expected_encoded ||
                              !actual.binary_value.empty()))) {
                    return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                                "columnar source reconstructed value changed");
                  }
                }
              }
              auto& batch = provider.provider_batch;
              batch.provider_uuid = source_input.provider_uuid;
              batch.provider_generation = source_input.provider_generation;
              batch.selected_alternative_uuid =
                  source_input.selected_alternative_uuid;
              batch.capability_uuid = source_input.capability_uuid;
              batch.exact_fallback_selected = true;
              batch.result_handle_uuid = source_input.result_handle_uuid;
              batch.causal_counter_id = source_input.causal_counter_id;
              batch.output_descriptor_ids =
                  source_input.output_descriptor_ids;
              batch.batch = std::move(reconstructed.batch);
              batch.properties.property_uuid = source_copy.property_uuid;
              batch.properties.uniqueness_id = "row_uuid";
              batch.properties.exact = true;
              batch.properties.residual_recheck_complete = true;
              batch.properties.base_row_mga_recheck_complete = true;
              batch.properties.security_recheck_complete = true;
              batch.mga_statement_context = source_input.mga_statement_context;
              batch.security_receipt_uuid =
                  source_copy.security_receipt_uuid;
              batch.residual_recheck_complete = true;
              batch.base_row_mga_recheck_complete = true;
              batch.security_recheck_complete = true;
              for (auto& row_uuid : reconstructed.row_uuids) {
                exec::ModelProviderRowIdentityV1 identity;
                identity.row_uuid = std::move(row_uuid);
                batch.ordered_row_identities.push_back(std::move(identity));
              }
              const auto provider_memory =
                  Rcp079ModelProviderBatchLogicalMemoryBytesV1(batch);
              const auto output_memory =
                  Rcp079ProjectedModelSourceOutputLogicalMemoryBytesV1(
                      source_input, batch);
              const auto uniqueness_memory =
                  Rcp079ColumnarUniquenessLogicalMemoryBytesV1(
                      batch.ordered_row_identities);
              std::uint64_t output_copy_peak = 0;
              std::uint64_t uniqueness_peak = 0;
              if (!provider_memory.has_value() ||
                  !output_memory.has_value() ||
                  !uniqueness_memory.has_value() ||
                  !CheckedAdd(*provider_memory, *output_memory,
                              &output_copy_peak) ||
                  !CheckedAdd(*provider_memory, *uniqueness_memory,
                              &uniqueness_peak)) {
                return fail(
                    "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "columnar source provider/exchange memory receipt overflowed");
              }
              source_runtime_memory_receipt.provider_logical_memory_bytes =
                  *provider_memory;
              source_runtime_memory_receipt.peak_live_memory_bytes =
                  std::max({pre_api_peak_memory, combined_peak_memory,
                            output_copy_peak, uniqueness_peak});
              source_runtime_memory_receipt.memory_grant_bytes =
                  source_input.maximum_memory_bytes;
              source_runtime_memory_receipt.complete =
                  source_runtime_memory_receipt.peak_live_memory_bytes <=
                  source_runtime_memory_receipt.memory_grant_bytes;
              if (!source_runtime_memory_receipt.complete) {
                return fail(
                    "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "columnar source provider/exchange peak exceeded the selected-node grant");
              }
              provider.ok = true;
              return provider;
            };
        const auto executed = exec::ExecuteModelFamilySourceV1(request);
        step.data_access_observed = executed.data_access_observed;
        if (!executed.accepted || !executed.root_published ||
            !executed.cleanup_complete || executed.cleanup_count != 1 ||
            !executed.output.exact_exchange_validated) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              executed.diagnostic_id.empty()
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : executed.diagnostic_id;
          step.diagnostic.detail = executed.detail;
          return step;
        }
        std::uint64_t current_memory_bytes = 0;
        if (!source_runtime_memory_receipt.complete ||
            source_runtime_memory_receipt.memory_grant_bytes !=
                selected_node.memory_bytes_required ||
            source_runtime_memory_receipt.peak_live_memory_bytes >
                source_runtime_memory_receipt.memory_grant_bytes ||
            !RuntimeMaterializedBatchMemoryBytes(
                executed.output.batch, &current_memory_bytes) ||
            current_memory_bytes >
                source_runtime_memory_receipt.peak_live_memory_bytes) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
          step.diagnostic.detail =
              "columnar source runtime memory receipt changed";
          return step;
        }
        PublishRuntimeMemoryObservation(
            &step, current_memory_bytes,
            source_runtime_memory_receipt.peak_live_memory_bytes);
        step.result_handle_id = selected_node.physical_node_id;
        step.output_row_count = executed.output.batch.rows.size();
        step.rows_examined = executed.rows_examined;
        step.current_relation_descriptor_uuid =
            source->persisted.descriptor_uuid;
        step.current_relation_descriptor_generation =
            source->persisted.descriptor_generation;
        step.materialized_output_batch = std::move(executed.output.batch);
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest selected;
  selected.selected_physical_dag = physical.physical_dag;
  selected.pre_access_statistics_snapshot_uuid =
      physical.physical_dag.statistics_snapshot_uuid;
  selected.mga_authority =
      BuildCanonicalExecutionMgaAuthority(input.context,
                                          physical.physical_dag);
  selected.runtime_limits.maximum_rows_per_batch = maximum_rows;
  selected.runtime_limits.maximum_columns_per_batch = maximum_columns;
  selected.runtime_limits.maximum_cells_per_batch = maximum_cells;
  selected.runtime_limits.maximum_total_materialized_rows =
      maximum_total_rows;
  selected.runtime_limits.maximum_total_materialized_cells =
      maximum_total_cells;
  selected.cancellation_requested = cancellation_requested;
  source_registration.executor_capability_uuid =
      shared_source_capability_uuid;
  selected.available_executors.push_back(std::move(source_registration));
  CanonicalRelationalExpressionRuntimeServices predicate_services;
  predicate_services.comparison_evaluator =
      [context = input.context](const api::EngineTypedValue& left,
                                const api::EngineTypedValue& right,
                                int* comparison,
                                std::string* diagnostic_id,
                                std::string* refusal_detail) {
        return CompareCanonicalRelationalScalarsV1(
            context, left, right, comparison, diagnostic_id, refusal_detail);
      };
  BindCanonicalPersistedRowDescriptorAuthorityForSpatialColumnarV1(
      input.context, &predicate_services);
  auto join_registration = MakeLiveJoinRegistration(
      "join." + join_component + ".3vl.nested.v1", join_capability_uuid, {},
      static_cast<std::size_t>(maximum_pairs_u64), maximum_rows, join_kind,
      "columnar model-family join", input.context, true,
      predicate_join ? join->bound_expression_ids.front() : 0,
      std::move(predicate_binding), dag, std::move(predicate_services));
  auto execute_join = std::move(join_registration.execute);
  join_registration.execute =
      [execute_join = std::move(execute_join), expected_join_columns](
          const exec::TypedPhysicalNodeDag& selected_dag,
          const exec::PhysicalNodeRecord& selected_node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs)
          mutable {
        auto step = execute_join(selected_dag, selected_node, inputs);
        if (!step.diagnostic.ok) return step;
        if (!step.materialized_output_batch.has_value() ||
            step.materialized_output_batch->columns.size() !=
                expected_join_columns.size() ||
            !std::ranges::equal(
                step.materialized_output_batch->columns,
                expected_join_columns,
                [](const auto& actual, const auto& expected) {
                  return Rcp079ExactExecutorColumnV1(actual, expected);
                })) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
          step.diagnostic.detail =
              "columnar join output descriptor differs from its exact source binding";
          step.materialized_output_batch.reset();
        }
        return step;
      };
  selected.available_executors.push_back(std::move(join_registration));
  selected.engine_execution_authorized = true;
  selected.result_publication_request.statement_uuid =
      input.context.statement_uuid;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  selected.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(identity_scope + ":" +
                               input.context.current_monotonic_ns,
                           "columnar-join.execution-attempt");
  selected.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id) + ":" +
              std::to_string(
                  input.context.snapshot_visible_through_local_transaction_id),
          "columnar-join.transaction-effect-unchanged");
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.maximum_row_count = maximum_rows;
  std::vector<const api::RelationalOutputRecord*> root_outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == join->node_id) root_outputs.push_back(&output);
  }
  std::ranges::sort(root_outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (root_outputs.size() != join->output_descriptor_ids.size() ||
      root_outputs.size() != expected_join_columns.size()) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "columnar join root bindings are incomplete");
  }
  for (std::size_t ordinal = 0; ordinal < root_outputs.size(); ++ordinal) {
    const auto descriptor = descriptor_for(root_outputs[ordinal]->descriptor_id);
    const auto source_index = ordinal < left_width ? 0U : 1U;
    const auto source_ordinal =
        source_index == 0 ? ordinal : ordinal - left_width;
    const auto& exact_source = prepared_sources[source_index];
    const auto& base_column = exact_source.columns[source_ordinal];
    const auto& persisted_column =
        exact_source.persisted.columns[source_ordinal];
    const auto& expected_column = expected_join_columns[ordinal];
    if (descriptor == dag.descriptors.end() ||
        root_outputs[ordinal]->ordinal != ordinal ||
        !root_outputs[ordinal]->visible ||
        root_outputs[ordinal]->descriptor_id !=
            join->output_descriptor_ids[ordinal] ||
        root_outputs[ordinal]->expression_id !=
            exact_source.output_expression_ids[source_ordinal] ||
        root_outputs[ordinal]->output_name_utf8 != base_column.stable_name ||
        descriptor->descriptor_id != base_column.descriptor_id ||
        descriptor->descriptor_uuid !=
            base_column.descriptor.descriptor_uuid ||
        !Rcp079ExactColumnarJoinColumnBindingV1(
            input.context, *descriptor, persisted_column) ||
        expected_column.descriptor_id != descriptor->descriptor_id ||
        expected_column.descriptor.descriptor_uuid !=
            descriptor->descriptor_uuid) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "columnar join root descriptor is not source-exact");
    }
    exec::CanonicalResultColumnDescriptor published;
    published.ordinal = static_cast<std::uint32_t>(ordinal);
    published.name_utf8 = root_outputs[ordinal]->output_name_utf8;
    published.descriptor_uuid = descriptor->descriptor_uuid;
    published.type_uuid = descriptor->type_uuid;
    published.nullability =
        expected_column.nullable
            ? exec::CanonicalResultNullability::kNullable
            : exec::CanonicalResultNullability::kNonNull;
    published.collation_uuid = descriptor->collation_uuid;
    published.timezone_profile_id = descriptor->timezone_profile_id;
    selected.result_publication_request.column_bindings.push_back(
        {ordinal, true, std::move(published)});
  }
  const auto execution = ExecuteSelectedCanonicalObjectFreeDag(
      input.context, selected, physical.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.data_access_observed ||
      !execution.runtime_actuals.accepted ||
      execution.dispatch.executed_steps.size() != 3 ||
      !execution.issues.empty()) {
    return refuse(execution.issues.empty()
                      ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                      : execution.issues.front().diagnostic_id,
                  execution.issues.empty()
                      ? "columnar composition execution did not complete"
                      : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = true;
  result.canonical_result_published = true;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(planning_request, execution);
  result.api_result.evidence.push_back(
      {"canonical.model_composition",
       "COLUMNAR_SOURCE_JOIN_COLUMNAR_SOURCE_ONE_ROOT_V1"});
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalSpatialColumnarFamilyQuery(
    const CanonicalCurrentHeapExecutionRequest& input,
    Rcp079CapturedModelLegV1* leg_capture) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  if (input.contextual_text_activation &&
      !Rcp079ExactContextualTextDirectRouteCandidateV2(dag)) {
    return result;
  }
  const auto source = std::ranges::find_if(dag.nodes, [](const auto& node) {
    return node.node_kind == api::RelationalDagNodeKind::kScan &&
           node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1";
  });
  if (source == dag.nodes.end()) return result;

  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.expression_id == expression_id;
    });
  };
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
      return descriptor.descriptor_id == descriptor_id;
    });
  };
  const auto operation_expression = [&](const std::string_view operation) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.operator_name == operation;
    });
  };
  const bool spatial =
      operation_expression("SPATIAL_SOURCE") != dag.expressions.end();
  const bool columnar =
      operation_expression("COLUMNAR_SOURCE") != dag.expressions.end();
  if (spatial == columnar) return result;
  result.profile_matched = true;

  CanonicalObjectFreeValuesExecutionRequest response_context;
  response_context.context = input.context;
  response_context.relational_dag = dag;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(response_context, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  std::vector<std::string> operations;
  for (const auto expression_id : source->bound_expression_ids) {
    const auto expression = expression_for(expression_id);
    if (expression == dag.expressions.end() ||
        !expression->operator_name.has_value()) {
      continue;
    }
    const auto& name = *expression->operator_name;
    if ((spatial && (name == "SPATIAL_SOURCE" || name == "SPATIAL_MATCH" ||
                     name == "SPATIAL_NEAREST")) ||
        (columnar && (name == "COLUMNAR_SOURCE" ||
                      name == "COLUMNAR_FILTER" ||
                      name == "COLUMNAR_PROJECT"))) {
      operations.push_back(name);
    }
  }
  const bool has_match = spatial &&
      std::ranges::find(operations, "SPATIAL_MATCH") != operations.end();
  const bool has_nearest = spatial &&
      std::ranges::find(operations, "SPATIAL_NEAREST") != operations.end();
  const bool has_filter = columnar &&
      std::ranges::find(operations, "COLUMNAR_FILTER") != operations.end();
  const bool has_project = columnar &&
      std::ranges::find(operations, "COLUMNAR_PROJECT") != operations.end();
  if (input.contextual_text_activation && (!columnar || !has_filter)) {
    return refuse(
        "SBLR.CONTEXTUAL_TEXT_LITERAL.ROUTE_MISMATCH",
        "contextual TEXT execution requires the exact COLUMNAR_FILTER route");
  }
  const std::vector<std::string> expected_operations = spatial
      ? (has_match
             ? (has_nearest
                    ? std::vector<std::string>{"SPATIAL_SOURCE",
                                               "SPATIAL_MATCH",
                                               "SPATIAL_NEAREST"}
                    : std::vector<std::string>{"SPATIAL_SOURCE",
                                               "SPATIAL_MATCH"})
             : (has_nearest
                    ? std::vector<std::string>{"SPATIAL_SOURCE",
                                               "SPATIAL_NEAREST"}
                    : std::vector<std::string>{"SPATIAL_SOURCE"}))
      : (has_filter
             ? (has_project
                    ? std::vector<std::string>{"COLUMNAR_SOURCE",
                                               "COLUMNAR_FILTER",
                                               "COLUMNAR_PROJECT"}
                    : std::vector<std::string>{"COLUMNAR_SOURCE",
                                               "COLUMNAR_FILTER"})
             : (has_project
                    ? std::vector<std::string>{"COLUMNAR_SOURCE",
                                               "COLUMNAR_PROJECT"}
                    : std::vector<std::string>{"COLUMNAR_SOURCE"}));
  const auto expected_width = spatial
      ? std::size_t{3} + static_cast<std::size_t>(has_match) +
            static_cast<std::size_t>(has_nearest)
      : source->output_descriptor_ids.size();
  if (dag.wire_version != 2 || dag.nodes.size() != 1 ||
      dag.root_node_id != source->node_id || !source->input_node_ids.empty() ||
      source->required_object_uuids.size() != 1 ||
      source->output_descriptor_ids.size() != expected_width ||
      expected_width == 0 || expected_width > 256 ||
      operations != expected_operations || dag.statement_timestamp.empty() ||
      dag.statement_timestamp != input.context.statement_timestamp) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "spatial/columnar canonical source shape is incomplete");
  }
  for (const auto& name : expected_operations) {
    const auto count = std::ranges::count_if(
        dag.expressions, [&](const auto& expression) {
          return expression.operator_name == name;
        });
    if (count != 1) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "spatial/columnar operation identity is duplicated");
    }
  }

  const auto object_uuid = source->required_object_uuids.front();
  if (columnar) {
    const auto columnar_source = operation_expression("COLUMNAR_SOURCE");
    if (columnar_source == dag.expressions.end() ||
        columnar_source->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        columnar_source->function_uuid.has_value() ||
        !columnar_source->child_expression_ids.empty() ||
        columnar_source->bound_name_uuid !=
            std::optional<std::string>(object_uuid) ||
        columnar_source->literal_kind.has_value() ||
        columnar_source->literal_or_parameter_ref.has_value()) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "COLUMNAR_SOURCE object binding is not exact");
    }
  }
  const auto authorization = api::EvaluateMaterializedAuthorization(
      input.context, input.context.authorization_context, "SELECT", object_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "spatial/columnar SELECT authorization was refused");
  }
  const auto loaded_relation =
      api::LoadMgaRelationStorageDescriptor(input.context, object_uuid);
  if (!loaded_relation.ok) {
    return refuse(spatial
                      ? "SB_MODEL_SPATIAL_EXACT_FALLBACK_UNAVAILABLE_V1"
                      : "SB_MODEL_COLUMNAR_EXACT_FALLBACK_UNAVAILABLE_V1",
                  loaded_relation.diagnostic.detail.empty()
                      ? "persistent model-family relation is unavailable"
                      : loaded_relation.diagnostic.detail);
  }
  const auto& persisted = loaded_relation.descriptor;
  if (persisted.relation_uuid != object_uuid ||
      persisted.database_uuid !=
          input.context.database_uuid ||
      !CanonicalUuidText(persisted.schema_uuid) ||
      persisted.relation_kind != "table" ||
      persisted.storage_profile != "local_mga_rowstore_v1" ||
      persisted.row_identity_rule != "engine_uuid_v7_only" ||
      persisted.version_identity_rule != "engine_uuid_v7_only" ||
      persisted.mutation_rule != "copy_on_write" ||
      persisted.visibility_rule !=
          "local_inventory_snapshot_visibility" ||
      persisted.cleanup_rule != "authoritative_local_horizon" ||
      persisted.recovery_rule !=
          "dirty_manifest_classification_no_wal" ||
      persisted.required_evidence_kinds !=
          std::vector<std::string>{"relation_descriptor", "row_version",
                                   "transaction_inventory",
                                   "dirty_manifest"} ||
      !CanonicalUuidText(persisted.descriptor_uuid) ||
      persisted.descriptor_generation == 0 || persisted.columns.empty() ||
      (persisted.descriptor_status != "production_descriptor" &&
       persisted.descriptor_status !=
           "metadata_bridge_vetted_descriptor") ||
      persisted.columns.size() > 256) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "persistent spatial/columnar relation descriptor is invalid");
  }
  static constexpr std::array<std::string_view, 5> kSpatialNames{
      "row_uuid", "spatial_value", "crs_uuid", "predicate_truth", "distance"};
  static constexpr std::array<std::string_view, 5> kSpatialTypes{
      "uuid", "geometry", "uuid", "boolean", "real64"};
  std::array<std::string, 5> spatial_type_uuids;
  if (spatial) {
    for (std::size_t ordinal = 0; ordinal < kSpatialTypes.size(); ++ordinal) {
      spatial_type_uuids[ordinal] =
          ExactCanonicalCoreDatatypeUuidV1(kSpatialTypes[ordinal]);
    }
    if (std::ranges::any_of(spatial_type_uuids,
                            [](const auto& uuid) { return uuid.empty(); })) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                    "spatial core datatype registry is unavailable");
    }
  }
  if (columnar) {
    std::unordered_set<std::string> column_uuids;
    std::unordered_set<std::string> column_names;
    std::unordered_set<std::string> descriptor_carrier_identities;
    for (std::size_t ordinal = 0; ordinal < persisted.columns.size();
         ++ordinal) {
      const auto& column = persisted.columns[ordinal];
      const bool ordinal_exact = column.ordinal == ordinal;
      const bool name_present = !column.canonical_name_key.empty();
      const bool column_uuid_exact =
          CanonicalUuidText(column.column_uuid);
      const bool column_uuid_unique =
          column_uuid_exact &&
          column_uuids.insert(column.column_uuid).second;
      const bool column_name_unique =
          name_present && column_names.insert(column.canonical_name_key).second;
      const bool descriptor_identity_exact =
          api::QowCanonicalDescriptorIdentityV1(column.value_descriptor);
      const bool descriptor_kind_exact =
          column.value_descriptor.descriptor_kind ==
          "canonical_type_descriptor";
      const bool persisted_shape_exact =
          Rcp079ExactPersistedColumnDescriptorV1(input.context, column);
      const bool descriptor_carrier_unique =
          descriptor_carrier_identities
              .insert(column.value_descriptor.descriptor_uuid +
                      ":" + column.column_uuid)
              .second;
      const auto descriptor_fields =
          Rcp079ExactDescriptorFieldsV1(column.value_descriptor);
      const bool descriptor_fields_exact = descriptor_fields.has_value();
      const auto diagnostic_field = [&](const std::string_view name) {
        if (!descriptor_fields.has_value()) return std::string_view{};
        const auto found = descriptor_fields->find(name);
        return found == descriptor_fields->end() ? std::string_view{}
                                                  : found->second;
      };
      const auto expected_core_type_uuid = ExactCanonicalCoreDatatypeTypeUuidV1(
          column.value_descriptor.canonical_type_name);
      if (!ordinal_exact || !name_present || !column_uuid_exact ||
          !column_uuid_unique || !column_name_unique ||
          !descriptor_identity_exact || !descriptor_kind_exact ||
          !persisted_shape_exact || !descriptor_carrier_unique ||
          !descriptor_fields_exact) {
        std::ostringstream detail;
        detail << "columnar persisted descriptor identity is not exact"
               << ";ordinal=" << ordinal
               << ";stored_ordinal=" << column.ordinal
               << ";name=" << column.canonical_name_key
               << ";ordinal_exact=" << ordinal_exact
               << ";name_present=" << name_present
               << ";column_uuid_exact=" << column_uuid_exact
               << ";column_uuid_unique=" << column_uuid_unique
               << ";column_name_unique=" << column_name_unique
               << ";descriptor_identity_exact="
               << descriptor_identity_exact
               << ";descriptor_kind_exact=" << descriptor_kind_exact
               << ";persisted_shape_exact=" << persisted_shape_exact
               << ";descriptor_carrier_unique="
               << descriptor_carrier_unique
               << ";descriptor_fields_exact=" << descriptor_fields_exact
               << ";column_uuid=" << column.column_uuid
               << ";descriptor_uuid="
               << column.value_descriptor.descriptor_uuid
               << ";descriptor_kind="
               << column.value_descriptor.descriptor_kind
               << ";canonical_type_name="
               << column.value_descriptor.canonical_type_name
               << ";encoded_type_uuid=" << diagnostic_field("type_uuid")
               << ";expected_core_type_uuid=" << expected_core_type_uuid
               << ";encoded_type=" << diagnostic_field("type")
               << ";encoded_canonical=" << diagnostic_field("canonical")
               << ";nullable=" << column.nullable
               << ";storage_class=" << column.storage_class
               << ";max_inline_bytes=" << column.max_inline_bytes
               << ";overflow_policy=" << column.overflow_policy
               << ";charset_uuid=" << column.charset_uuid
               << ";collation_uuid=" << column.collation_uuid
               << ";character_length=" << column.character_length
               << ";encoded_descriptor="
               << column.value_descriptor.encoded_descriptor;
        return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                      detail.str());
      }
    }
    std::string columnar_identifier_detail;
    if (!Rcp079ExactColumnarIdentifierBindingsV1(
            input.context, dag, *source, persisted, has_filter, has_project,
            &columnar_identifier_detail)) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                    std::move(columnar_identifier_detail));
    }
  }
  if (spatial &&
      (persisted.columns.size() != 3 ||
       persisted.columns[0].canonical_name_key != "row_uuid" ||
       persisted.columns[0].value_descriptor.canonical_type_name != "uuid" ||
       persisted.columns[0].nullable ||
       persisted.columns[1].canonical_name_key != "spatial_value" ||
       persisted.columns[1].value_descriptor.canonical_type_name != "geometry" ||
       persisted.columns[1].nullable ||
       persisted.columns[2].canonical_name_key != "crs_uuid" ||
       persisted.columns[2].value_descriptor.canonical_type_name != "uuid" ||
       persisted.columns[2].nullable)) {
    return refuse("SB_MODEL_SPATIAL_PROFILE_UNSUPPORTED_V1",
                  "persistent spatial relation is not the exact point cohort");
  }
  if (spatial) {
    for (std::size_t ordinal = 0; ordinal < persisted.columns.size(); ++ordinal) {
      const auto& column = persisted.columns[ordinal];
      if (column.ordinal != ordinal ||
          !CanonicalUuidText(column.column_uuid) ||
          column.value_descriptor.descriptor_kind !=
              "canonical_type_descriptor" ||
          !api::QowCanonicalDescriptorIdentityV1(column.value_descriptor) ||
          !CanonicalDescriptorFieldEqualsForComposition(
              column.value_descriptor, "type_uuid",
              std::string_view(spatial_type_uuids[ordinal])) ||
          !column.collation_uuid.empty() ||
          !CanonicalDescriptorFieldEqualsForComposition(column.value_descriptor,
                                            "collation_uuid", std::nullopt) ||
          !CanonicalDescriptorFieldEqualsForComposition(
              column.value_descriptor, "timezone_profile_id", std::nullopt)) {
        return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                      "spatial persisted type identity is not exact");
      }
    }
  }

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == source->node_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (outputs.size() != expected_width) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "spatial/columnar public outputs are incomplete");
  }
  std::vector<exec::ExecutorColumnDescriptor> public_columns;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  public_columns.reserve(outputs.size());
  result_bindings.reserve(outputs.size());
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    const auto descriptor = descriptor_for(outputs[ordinal]->descriptor_id);
    const auto output_expression = expression_for(outputs[ordinal]->expression_id);
    if (descriptor == dag.descriptors.end() ||
        output_expression == dag.expressions.end() ||
        !outputs[ordinal]->visible || outputs[ordinal]->ordinal != ordinal ||
        outputs[ordinal]->descriptor_id != source->output_descriptor_ids[ordinal] ||
        output_expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        output_expression->result_descriptor_id != descriptor->descriptor_id ||
        descriptor->nullability == api::RelationalNullability::kUnknown) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "spatial/columnar output binding was substituted");
    }
    const api::MgaRelationColumnStorageDescriptor* persisted_column = nullptr;
    if (output_expression->bound_name_uuid.has_value()) {
      const auto found = std::ranges::find_if(
          persisted.columns, [&](const auto& column) {
            return column.column_uuid ==
                   *output_expression->bound_name_uuid;
          });
      if (found != persisted.columns.end()) persisted_column = &*found;
    }
    api::EngineDescriptor engine_descriptor;
    if (spatial && ordinal < persisted.columns.size()) {
      const auto& column = persisted.columns[ordinal];
      // A standalone spatial source publishes the persisted column descriptor
      // identity.  A captured multileg source instead publishes the exact
      // statement-scoped V10 allocation that the enclosing coordinator has
      // already preflighted.  Both retain the persisted column/type/name
      // authority; only the result-carrier descriptor UUID differs.
      const bool exact_descriptor_identity =
          leg_capture == nullptr
              ? descriptor->descriptor_uuid ==
                    column.value_descriptor.descriptor_uuid
              : CanonicalUuidText(descriptor->descriptor_uuid) &&
                    descriptor->descriptor_uuid != descriptor->type_uuid;
      if (output_expression->bound_name_uuid !=
              std::optional<std::string>(column.column_uuid) ||
          outputs[ordinal]->output_name_utf8 != column.canonical_name_key ||
          !exact_descriptor_identity ||
          descriptor->type_uuid != spatial_type_uuids[ordinal] ||
          descriptor->nullability != api::RelationalNullability::kNonNull ||
          descriptor->collation_uuid.has_value() ||
          descriptor->timezone_profile_id.has_value()) {
        return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                      "spatial base output differs from persisted type authority");
      }
      engine_descriptor = column.value_descriptor;
      if (leg_capture != nullptr) {
        engine_descriptor.descriptor_uuid =
            descriptor->descriptor_uuid;
      }
      engine_descriptor.descriptor_kind = "scalar";
      engine_descriptor.encoded_descriptor =
          "type_uuid=" + descriptor->type_uuid + ";nullability=non_null";
    } else if (persisted_column != nullptr && !spatial) {
      if (outputs[ordinal]->output_name_utf8 !=
              persisted_column->canonical_name_key ||
          std::ranges::count_if(
              persisted.columns, [&](const auto& candidate) {
                return candidate.column_uuid ==
                       persisted_column->column_uuid;
              }) != 1 ||
          !Rcp079ExactColumnarJoinColumnBindingV1(
              input.context, *descriptor, *persisted_column)) {
        return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                      "columnar output differs from persisted type authority");
      }
      engine_descriptor = persisted_column->value_descriptor;
      // The persisted relation column keeps its public descriptor handle, but
      // the statement DAG carries the exact live canonical datatype descriptor
      // selected from that handle.  Execution and result publication must use
      // the latter after the persisted-column authority check above succeeds.
      engine_descriptor.descriptor_uuid = descriptor->descriptor_uuid;
      engine_descriptor.descriptor_kind = "scalar";
    } else {
      const auto expected_name = spatial && ordinal < kSpatialNames.size()
                                     ? kSpatialNames[ordinal]
                                     : std::string_view{};
      const auto expected_type = spatial && ordinal < kSpatialTypes.size()
                                     ? kSpatialTypes[ordinal]
                                     : std::string_view{};
      if (!spatial || expected_name.empty() ||
          outputs[ordinal]->output_name_utf8 != expected_name ||
          descriptor->type_uuid != spatial_type_uuids[ordinal] ||
          descriptor->nullability != api::RelationalNullability::kNonNull ||
          descriptor->collation_uuid.has_value() ||
          descriptor->timezone_profile_id.has_value()) {
        return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                      "derived spatial output type identity is invalid");
      }
      engine_descriptor.descriptor_uuid = descriptor->descriptor_uuid;
      engine_descriptor.descriptor_kind = "scalar";
      engine_descriptor.canonical_type_name = std::string(expected_type);
      engine_descriptor.encoded_descriptor =
          "type_uuid=" + descriptor->type_uuid + ";nullability=non_null";
    }
    public_columns.push_back(
        {outputs[ordinal]->output_name_utf8, engine_descriptor,
         descriptor->nullability == api::RelationalNullability::kNullable,
         descriptor->descriptor_id});
    exec::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = ordinal;
    binding.visible = true;
    binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
        static_cast<std::uint32_t>(ordinal),
        outputs[ordinal]->output_name_utf8, descriptor->descriptor_uuid,
        descriptor->type_uuid,
        descriptor->nullability == api::RelationalNullability::kNullable
            ? exec::CanonicalResultNullability::kNullable
            : exec::CanonicalResultNullability::kNonNull,
        descriptor->collation_uuid, descriptor->timezone_profile_id};
    result_bindings.push_back(std::move(binding));
  }
  exec::DescriptorBatch public_descriptor_batch;
  public_descriptor_batch.columns = public_columns;
  const auto public_validation = exec::ValidateCanonicalDescriptorBatch(
      public_descriptor_batch, source->output_descriptor_ids);
  if (!public_validation.ok) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  public_validation.detail);
  }

  api::EngineResolveStatementSnapshotRequest snapshot_request;
  snapshot_request.context = input.context;
  const auto snapshot = api::EngineResolveStatementSnapshot(snapshot_request);
  if (!snapshot.ok) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "current engine MGA statement snapshot is unavailable");
  }
  auto mga = PhysicalMgaContextFromResolvedSnapshot(
      input.context, snapshot.snapshot_vector);
  mga.statement_timestamp = input.context.statement_timestamp;
  if (!exec::PhysicalMgaStatementContextValid(mga)) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "spatial/columnar statement context is invalid");
  }

  std::string spatial_crs_uuid;
  std::uint64_t spatial_crs_generation = 0;
  if (spatial) {
    const auto operation = has_match
                               ? operation_expression("SPATIAL_MATCH")
                               : has_nearest
                                     ? operation_expression("SPATIAL_NEAREST")
                                     : dag.expressions.end();
    if (operation != dag.expressions.end()) {
      const auto crs_ordinal = has_match ? std::size_t{3} : std::size_t{2};
      if (operation->child_expression_ids.size() != 4) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "spatial operation arity changed");
      }
      const auto crs =
          expression_for(operation->child_expression_ids[crs_ordinal]);
      if (crs == dag.expressions.end() ||
          crs->expression_kind != api::RelationalExpressionKind::kIdentifier ||
          !crs->bound_name_uuid.has_value()) {
        return refuse("SB_MODEL_SPATIAL_CRS_BINDING_REQUIRED_V1",
                      "spatial query CRS is not bound");
      }
      spatial_crs_uuid = *crs->bound_name_uuid;
      spatial_crs_generation = input.context.catalog_generation_id;
    } else {
      const auto crs = ExactEncodedDescriptorField(
          persisted.columns[1].value_descriptor.encoded_descriptor,
          "crs_uuid");
      const auto generation = ExactEncodedDescriptorField(
          persisted.columns[1].value_descriptor.encoded_descriptor,
          "crs_generation");
      if (!crs.has_value() || !generation.has_value()) {
        return refuse("SB_MODEL_SPATIAL_CRS_BINDING_REQUIRED_V1",
                      "spatial source descriptor lacks CRS identity");
      }
      spatial_crs_uuid = *crs;
      const auto parsed = std::from_chars(
          generation->data(), generation->data() + generation->size(),
          spatial_crs_generation);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != generation->data() + generation->size()) {
        spatial_crs_generation = 0;
      }
    }
    if (!CanonicalUuidText(spatial_crs_uuid) || spatial_crs_generation == 0) {
      return refuse("SB_MODEL_SPATIAL_CRS_BINDING_REQUIRED_V1",
                    "spatial CRS identity or generation is invalid");
    }
    const auto source_root = operation_expression("SPATIAL_SOURCE");
    if (source_root == dag.expressions.end() ||
        source_root->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        source_root->function_uuid.has_value() ||
        !source_root->child_expression_ids.empty()) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "SPATIAL_SOURCE root is not exact");
    }
    const auto validate_spatial_operation = [&](const auto& operation,
                                                 const bool match) {
      if (operation == dag.expressions.end() ||
          operation->expression_kind !=
              api::RelationalExpressionKind::kFunctionCall ||
          operation->function_uuid.has_value() ||
          operation->child_expression_ids.size() != 4) {
        return false;
      }
      const auto alias = expression_for(operation->child_expression_ids[0]);
      const auto point = expression_for(
          operation->child_expression_ids[match ? 2 : 1]);
      const auto crs = expression_for(
          operation->child_expression_ids[match ? 3 : 2]);
      return alias != dag.expressions.end() && point != dag.expressions.end() &&
             crs != dag.expressions.end() &&
             alias->expression_kind ==
                 api::RelationalExpressionKind::kIdentifier &&
             alias->bound_name_uuid == std::optional<std::string>(object_uuid) &&
             point->expression_kind ==
                 api::RelationalExpressionKind::kFunctionCall &&
             point->operator_name == "POINT" &&
             !point->function_uuid.has_value() &&
             point->child_expression_ids.size() == 2 &&
             crs->expression_kind ==
                 api::RelationalExpressionKind::kIdentifier &&
             crs->bound_name_uuid ==
                 std::optional<std::string>(spatial_crs_uuid);
    };
    if ((has_match &&
         !validate_spatial_operation(operation_expression("SPATIAL_MATCH"),
                                     true)) ||
        (has_nearest &&
         !validate_spatial_operation(operation_expression("SPATIAL_NEAREST"),
                                     false))) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "spatial alias, POINT, or CRS binding changed");
    }
  }

  const std::string family = spatial ? "spatial" : "columnar";
  const std::string operation_id =
      operations.size() == 1
          ? operations.front()
          : operations.size() == 2 ? operations.back() : std::string{};
  const std::string implementation_id =
      spatial ? "physical_spatial_index_scan_v1"
              : "physical_columnar_zone_scan_v1";
  const std::string logical_operator_id =
      spatial ? "LOGICAL_SPATIAL_SOURCE_V1"
              : "LOGICAL_COLUMNAR_SOURCE_V1";
  const auto identity_scope =
      dag.bound_sblr_tree_uuid + ":" + input.context.statement_uuid;
  const auto source_identity_scope =
      identity_scope + ":" + std::to_string(source->node_id) + ":" +
      object_uuid;
  const auto provider_uuid =
      DerivedCanonicalUuid(source_identity_scope, family + ".provider");
  const auto capability_uuid =
      DerivedCanonicalUuid(identity_scope, family + ".capability");
  const auto result_handle_uuid =
      DerivedCanonicalUuid(source_identity_scope, family + ".result-handle");
  const auto property_uuid =
      DerivedCanonicalUuid(source_identity_scope, family + ".property");
  const auto security_receipt_uuid =
      DerivedCanonicalUuid(source_identity_scope, family + ".security-receipt");
  const auto policy_snapshot_uuid =
      DerivedCanonicalUuid(source_identity_scope, family + ".policy-snapshot");
  const auto statistics_snapshot_uuid =
      DerivedCanonicalUuid(source_identity_scope,
                           family + ".statistics-snapshot");
  const auto resource_contract_uuid =
      DerivedCanonicalUuid(source_identity_scope,
                           family + ".resource-contract");
  const auto suffix = std::to_string(source->node_id) + "." + implementation_id;
  const auto alternative_uuid =
      DerivedCanonicalUuid(identity_scope, "alternative." + suffix);
  const auto cost_uuid =
      DerivedCanonicalUuid(identity_scope, "cost-vector." + suffix);
  const auto generation =
      std::max<std::uint64_t>(1, input.context.catalog_generation_id);

  opt::ModelFamilyCoordinatorRequestV1 planning;
  planning.family_id = family;
  planning.operation_ids = operations;
  planning.operation_id = operation_id;
  planning.logical_operator_id = logical_operator_id;
  planning.logical_node_id = source->node_id;
  planning.object_uuid = object_uuid;
  planning.output_descriptor_ids = source->output_descriptor_ids;
  planning.mga_statement_context = mga;
  planning.bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
  planning.catalog_epoch_uuid = input.context.catalog_epoch_uuid;
  planning.security_context_uuid =
      input.context.authorization_context.authority_uuid;
  planning.capability_snapshot_uuid =
      input.context.optimizer_capability_snapshot_uuid;
  planning.resource_snapshot_uuid =
      input.context.optimizer_resource_snapshot_uuid;
  planning.statistics_snapshot_uuid = statistics_snapshot_uuid;
  planning.route_snapshot_uuid =
      input.context.optimizer_route_snapshot_uuid;
  planning.catalog_generation = generation;
  planning.current_catalog_generation = generation;
  planning.security_epoch = std::max<std::uint64_t>(1, input.context.security_epoch);
  planning.policy_epoch = std::max<std::uint64_t>(
      1, input.context.authorization_context.policy_epoch);
  planning.resource_epoch = std::max<std::uint64_t>(1, input.context.resource_epoch);
  planning.statistics_generation = generation;
  planning.route_epoch = input.context.optimizer_route_epoch;
  planning.route_generation = input.context.optimizer_route_generation;
  planning.memory_budget_bytes = input.context.optimizer_memory_budget_bytes;
  planning.security_admitted = input.context.security_context_present &&
                               input.context.authorization_context.present;
  std::vector<opt::ModelFamilyCapabilitySnapshotV1> alternatives;
  alternatives.push_back(MakeModelFamilyCapabilitySnapshotForCompositionV1(
      planning, identity_scope + "." + family + ".fallback",
      opt::ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback,
      provider_uuid, capability_uuid, persisted.descriptor_generation, true, 1,
      1, std::max<std::uint64_t>(1, planning.memory_budget_bytes / 2)));
  const auto planned = PlanCanonicalModelFamilySourceForCompositionV1(
      planning, identity_scope + "." + family + ".inventory",
      std::move(alternatives));
  if (!planned.accepted || !planned.selected ||
      !planned.data_access_allowed || !planned.optimizer_owned_enumeration ||
      !planned.exact_fallback_selected ||
      planned.selected_candidate.provider_uuid != provider_uuid ||
      planned.selected_candidate.capability_uuid != capability_uuid ||
      planned.selected_candidate.provider_generation !=
          persisted.descriptor_generation) {
    return refuse(planned.diagnostic_id.empty()
                      ? (spatial
                             ? "SB_MODEL_SPATIAL_EXACT_FALLBACK_UNAVAILABLE_V1"
                             : "SB_MODEL_COLUMNAR_EXACT_FALLBACK_UNAVAILABLE_V1")
                      : planned.diagnostic_id,
                  planned.detail.empty()
                      ? "model-family coordinator did not select exact fallback"
                      : planned.detail);
  }

  api::CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid = input.context.catalog_epoch_uuid;
  planning_scope.security_context_uuid =
      input.context.authorization_context.authority_uuid;
  planning_scope.statement_uuid = input.context.statement_uuid;
  planning_scope.statement_timestamp = input.context.statement_timestamp;
  planning_scope.owning_transaction_uuid = input.context.transaction_uuid;
  planning_scope.statement_snapshot_uuid =
      input.context.statement_snapshot_uuid;
  planning_scope.statement_metadata_snapshot_uuid =
      input.context.statement_metadata_snapshot_uuid;
  planning_scope.local_transaction_id = input.context.local_transaction_id;
  planning_scope.snapshot_visible_through_local_transaction_id =
      input.context.snapshot_visible_through_local_transaction_id;
  planning_scope.metadata_snapshot_engine_owned =
      input.context.statement_metadata_snapshot_engine_owned;
  planning_scope.authorization_context_engine_owned =
      input.context.authorization_context.present;
  auto logical = api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
      dag, planning_scope);
  const auto expected_family =
      spatial ? plan::CanonicalLogicalModelFamilyIdentity::kSpatial
              : plan::CanonicalLogicalModelFamilyIdentity::kColumnar;
  if (!logical.accepted || logical.logical_graph.nodes.size() != 1 ||
      logical.logical_graph.nodes.front().logical_node_id != source->node_id ||
      logical.logical_graph.nodes.front().model_family_identity != expected_family) {
    return refuse(logical.issues.empty()
                      ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
                      : logical.issues.front().diagnostic_id,
                  logical.issues.empty()
                      ? "spatial/columnar logical bridge was refused"
                      : logical.issues.front().field_id);
  }
  const auto registered_logical_mga = Rcp079LogicalMga(mga, false);
  const auto current_logical_mga = Rcp079LogicalMga(mga, true);
  if (!plan::CanonicalMgaStatementContextEqual(
          logical.logical_graph.mga_statement_context,
          registered_logical_mga) ||
      !plan::CanonicalMgaStatementContextEqual(
          logical.property_catalog.mga_statement_context,
          registered_logical_mga)) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "spatial/columnar logical bridge MGA cohort changed");
  }
  logical.logical_graph.mga_statement_context = current_logical_mga;
  logical.property_catalog.mga_statement_context = current_logical_mga;

  opt::CanonicalNativeObjectAdmissionContext admission_context;
  admission_context.statement_uuid = input.context.statement_uuid;
  admission_context.catalog_snapshot_uuid =
      input.context.statement_metadata_snapshot_uuid;
  admission_context.security_context_uuid =
      input.context.authorization_context.authority_uuid;
  admission_context.catalog_generation = input.context.catalog_generation_id;
  admission_context.authorization_catalog_generation =
      input.context.authorization_context.catalog_generation_id;
  admission_context.security_epoch =
      input.context.authorization_context.security_epoch;
  admission_context.policy_epoch =
      input.context.authorization_context.policy_epoch;
  admission_context.resource_epoch = input.context.resource_epoch;
  admission_context.capability_snapshot_uuid =
      input.context.optimizer_capability_snapshot_uuid;
  admission_context.resource_snapshot_uuid =
      input.context.optimizer_resource_snapshot_uuid;
  admission_context.route_snapshot_uuid =
      input.context.optimizer_route_snapshot_uuid;
  admission_context.route_epoch = input.context.optimizer_route_epoch;
  admission_context.route_generation = input.context.optimizer_route_generation;
  admission_context.memory_budget_bytes = input.context.optimizer_memory_budget_bytes;
  admission_context.maximum_candidate_count =
      input.context.optimizer_maximum_candidate_count;
  admission_context.maximum_memo_groups =
      input.context.optimizer_maximum_memo_groups;
  admission_context.maximum_search_steps =
      input.context.optimizer_maximum_search_steps;
  admission_context.maximum_planning_time_ns =
      input.context.optimizer_maximum_planning_time_ns;
  admission_context.spill_allowed = input.context.optimizer_spill_allowed;
  admission_context.local_transaction_id = input.context.local_transaction_id;
  admission_context.statement_snapshot_id =
      input.context.snapshot_visible_through_local_transaction_id;
  admission_context.mga_statement_context = current_logical_mga;
  std::uint64_t admitted_at_monotonic_ns = 0;
  const auto monotonic = std::from_chars(
      input.context.current_monotonic_ns.data(),
      input.context.current_monotonic_ns.data() +
          input.context.current_monotonic_ns.size(),
      admitted_at_monotonic_ns);
  if (monotonic.ec != std::errc{} ||
      monotonic.ptr != input.context.current_monotonic_ns.data() +
                           input.context.current_monotonic_ns.size() ||
      admitted_at_monotonic_ns == 0) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "spatial/columnar optimizer monotonic context is invalid");
  }
  admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
  admission_context.metadata_snapshot_engine_owned = true;
  admission_context.authorization_context_engine_owned = true;
  admission_context.catalog_object_uuids = {object_uuid};
  admission_context.authorized_object_uuids = {object_uuid};
  admission_context.catalog_object_evidence_engine_owned = true;
  admission_context.authorization_object_evidence_engine_owned = true;
  auto canonical_admission =
      opt::BuildCanonicalObjectAwareNativeOptimizerAdmissionRequest(
          logical.logical_graph, logical.property_catalog, admission_context);
  if (!canonical_admission.built ||
      !canonical_admission.admission.admitted ||
      !canonical_admission.admission.planning_allowed ||
      canonical_admission.admission.data_access_allowed ||
      canonical_admission.admission.evidence.size() != 8) {
    return refuse(canonical_admission.diagnostic_id.empty()
                      ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
                      : canonical_admission.diagnostic_id,
                  canonical_admission.field_id.empty()
                      ? "spatial/columnar canonical optimizer admission failed"
                      : canonical_admission.field_id);
  }
  result.optimizer_admitted = true;
  CanonicalObjectFreeValuesExecutionRequest canonical_planning_request{
      input.context, dag, canonical_admission.request,
      canonical_admission.admission};
  LivePhysicalNodeProfile profile;
  profile.logical_node_id = source->node_id;
  profile.implementation_id = implementation_id;
  profile.capability_uuid = capability_uuid;
  profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
  profile.transformation_rule_id =
      spatial ? "canonical.spatial.exact-scan.v1"
              : "canonical.columnar.reconstruction.v1";
  profile.estimated_rows = 1;
  profile.memory_bytes_required =
      planned.selected_candidate.cost.memory_bytes_required;
  profile.page_read_sequential_units = 1;
  profile.mga_visibility_checks_expected = 1;
  profile.storage_read_capable = true;
  profile.mga_visibility_capable = true;
  profile.residual_predicate_required = has_match || has_filter || has_nearest;
  profile.storage_recheck_required = true;
  profile.compatibility_profile_id = family + ".local.v1";
  std::vector<LivePhysicalNodeProfile> profiles{std::move(profile)};
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "spatial/columnar producer memory receipt is incomplete");
  }
  const auto physical = PlanAndPublishLivePhysicalDag(
      canonical_planning_request, profiles, family + ".selected-plan",
      family + " model source", family + ".local.v1");
  if (!physical.ok || physical.physical_dag.nodes.size() != 1 ||
      physical.physical_dag.nodes.front().implementation_id != implementation_id ||
      physical.physical_dag.nodes.front().selected_alternative_uuid !=
          alternative_uuid ||
      physical.physical_dag.nodes.front().executor_capability_uuid !=
          capability_uuid) {
    return refuse(physical.diagnostic_id.empty()
                      ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
                      : physical.diagnostic_id,
                  physical.detail.empty()
                      ? "spatial/columnar physical DAG was not published"
                      : physical.detail);
  }
  const auto& physical_source = physical.physical_dag.nodes.front();
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.optimizer_admission_stage_count =
      canonical_admission.admission.evidence.size();
  result.physical_node_count = 1;
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

  if (physical_source.memory_bytes_required == 0 ||
      physical_source.memory_bytes_required >
          physical.physical_dag.memory_budget_bytes ||
      physical_source.memory_bytes_required > planning.memory_budget_bytes) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "spatial/columnar selected source grant is invalid");
  }
  const auto exchange_memory = physical_source.memory_bytes_required;
  const auto provider_memory = planning.memory_budget_bytes - exchange_memory;
  const auto maximum_rows = std::min<std::uint64_t>(
      {65'536, input.context.optimizer_maximum_candidate_count,
       std::max<std::uint64_t>(1, provider_memory / 256)});
  std::uint64_t maximum_cells = 0;
  std::uint64_t maximum_reconstruction_cells = 0;
  if (provider_memory < 4096 || exchange_memory < 4096 || maximum_rows == 0 ||
      !CheckedMultiply(maximum_rows, expected_width, &maximum_cells) ||
      !CheckedMultiply(maximum_rows, persisted.columns.size(),
                       &maximum_reconstruction_cells) ||
      maximum_cells > std::numeric_limits<std::size_t>::max() ||
      maximum_reconstruction_cells >
          std::numeric_limits<std::size_t>::max()) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "spatial/columnar route resource bounds are incomplete");
  }

  exec::ModelSourceInputDescriptorV1 source_input;
  source_input.family_id = family;
  source_input.operation_ids = operations;
  source_input.operation_id = operation_id;
  source_input.object_uuid = object_uuid;
  if (spatial) {
    source_input.spatial_geometry_descriptor_uuid =
        persisted.columns[1].value_descriptor.descriptor_uuid;
    source_input.spatial_geometry_type_uuid =
        spatial_type_uuids[1];
    source_input.spatial_crs_uuid = spatial_crs_uuid;
    source_input.spatial_crs_generation = spatial_crs_generation;
  }
  source_input.physical_node_id = physical_source.physical_node_id;
  source_input.selected_alternative_uuid = alternative_uuid;
  source_input.capability_uuid = capability_uuid;
  source_input.provider_uuid = provider_uuid;
  source_input.provider_generation = persisted.descriptor_generation;
  source_input.result_handle_uuid = result_handle_uuid;
  source_input.causal_counter_id = physical_source.causal_counter_id;
  source_input.output_descriptor_ids = source->output_descriptor_ids;
  source_input.mga_statement_context = mga;
  source_input.catalog_epoch_uuid = input.context.catalog_epoch_uuid;
  source_input.security_context_uuid =
      input.context.authorization_context.authority_uuid;
  source_input.policy_snapshot_uuid = policy_snapshot_uuid;
  source_input.resource_contract_uuid = resource_contract_uuid;
  source_input.catalog_generation = generation;
  source_input.descriptor_generation = persisted.descriptor_generation;
  source_input.security_generation = planning.security_epoch;
  source_input.policy_generation = planning.policy_epoch;
  source_input.resource_generation = planning.resource_epoch;
  source_input.maximum_rows = static_cast<std::size_t>(maximum_rows);
  source_input.maximum_cells = static_cast<std::size_t>(maximum_cells);
  source_input.maximum_memory_bytes = exchange_memory;
  source_input.exact_fallback_selected = true;

  const auto model_cancellation_probe_failed =
      std::make_shared<std::atomic_bool>(false);
  const auto raw_model_cancellation =
      input.context.query_cancellation_requested;
  const std::function<bool()> cancellation_requested =
      [raw_model_cancellation, model_cancellation_probe_failed]() noexcept {
        if (!raw_model_cancellation) return false;
        try {
          return raw_model_cancellation();
        } catch (...) {
          model_cancellation_probe_failed->store(
              true, std::memory_order_relaxed);
          return true;
        }
      };
  const auto columnar_runtime_memory_receipt =
      std::make_shared<Rcp079ColumnarSourceRuntimeMemoryReceiptV1>();
  exec::ModelFamilyExecutionRequestV1 execution_request;
  execution_request.input = source_input;
  execution_request.capability.capability_uuid = capability_uuid;
  execution_request.capability.family_id = family;
  execution_request.capability.provider_uuid = provider_uuid;
  execution_request.capability.provider_generation =
      source_input.provider_generation;
  execution_request.capability.available = true;
  execution_request.capability.exact = true;
  execution_request.capability.exact_collection_fallback_available = true;
  execution_request.capability.cancellation_supported = true;
  execution_request.capability.cleanup_supported = true;
  execution_request.capability.residual_recheck_supported = true;
  execution_request.capability.base_row_mga_recheck_supported = true;
  execution_request.capability.security_recheck_supported = true;
  // The no-throw sticky wrapper prevents the generic exchange helper from
  // laundering a thrown engine probe into an ordinary cancellation.  The
  // registration checks the sticky failure before emitting cancellation
  // evidence.
  execution_request.cancellation_requested = input.contextual_text_activation
      ? std::function<bool()>([] { return false; })
      : cancellation_requested;
  execution_request.cleanup_provider = [] {};
  execution_request.exact_fallback_selected = true;
  execution_request.security_admitted = planning.security_admitted;
  execution_request.current_catalog_generation = generation;
  execution_request.current_descriptor_generation =
      source_input.descriptor_generation;
  execution_request.current_security_generation = planning.security_epoch;
  execution_request.current_policy_generation = planning.policy_epoch;
  execution_request.current_resource_generation = planning.resource_epoch;
  execution_request.current_provider_generation = source_input.provider_generation;
  execution_request.current_mga_statement_context = mga;

  const auto context = input.context;
  execution_request.execute_provider =
      [context, dag, persisted, source_input, public_columns, property_uuid,
       security_receipt_uuid, has_match, has_nearest, has_filter, has_project,
       cancellation_requested, maximum_reconstruction_cells,
       model_cancellation_probe_failed, columnar_runtime_memory_receipt,
       source_node_id = source->node_id,
       contextual_text_activation = input.contextual_text_activation](
          const exec::ModelSourceInputDescriptorV1&) mutable {
        exec::ModelProviderExecutionResultV1 provider;
        const auto fail = [&](std::string diagnostic, std::string detail) {
          provider.diagnostic_id = std::move(diagnostic);
          provider.detail = std::move(detail);
          return provider;
        };
        const auto cancellation_diagnostic = [&] {
          return Rcp079ContextualCancellationDiagnosticV2(
              static_cast<bool>(contextual_text_activation),
              model_cancellation_probe_failed->load(
                  std::memory_order_relaxed));
        };
        if (columnar_runtime_memory_receipt != nullptr) {
          *columnar_runtime_memory_receipt = {};
        }
        if (cancellation_requested()) {
          return fail(cancellation_diagnostic(),
                      "model-family execution was cancelled before revalidation");
        }
        const auto authorization = api::EvaluateMaterializedAuthorization(
            context, context.authorization_context, "SELECT",
            source_input.object_uuid);
        if (!authorization.authorized || authorization.denied ||
            authorization.policy_recheck_required ||
            !authorization.diagnostics.empty()) {
          return fail("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                      "model-family execution authorization changed");
        }
        const auto preflight = api::LoadMgaRelationStorageDescriptor(
            context, source_input.object_uuid);
        if (!preflight.ok ||
            !Rcp079ExactColumnarJoinStorageSnapshotV1(
                preflight.descriptor, persisted)) {
          return fail("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                      "model-family descriptor changed before MGA access");
        }
        if (cancellation_requested()) {
          return fail(cancellation_diagnostic(),
                      "model-family execution was cancelled before MGA access");
        }
        api::MgaVisibleHeapRelationReadRequest read_request;
        read_request.relation_uuid = source_input.object_uuid;
        read_request.maximum_scanned_row_versions =
            context.optimizer_maximum_search_steps;
        read_request.maximum_decoded_bytes =
            source_input.maximum_memory_bytes / 2;
        read_request.maximum_output_rows = source_input.maximum_rows;
        read_request.cancellation_requested = cancellation_requested;
        if (read_request.maximum_scanned_row_versions == 0 ||
            read_request.maximum_decoded_bytes == 0 ||
            read_request.maximum_output_rows == 0) {
          return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "model-family MGA read bound is zero");
        }
        auto read = api::ReadVisibleMgaHeapRelation(context, read_request);
        provider.data_access_observed =
            read.ok || read.scanned_row_version_count != 0 ||
            read.decoded_byte_count != 0;
        provider.rows_examined = read.scanned_row_version_count;
        if (!read.ok) {
          if (read.cancellation_observed ||
              model_cancellation_probe_failed->load(
                  std::memory_order_relaxed)) {
            return fail(cancellation_diagnostic(),
                        model_cancellation_probe_failed->load(
                                std::memory_order_relaxed)
                            ? "model-family MGA cancellation probe failed"
                            : "model-family MGA read was cancelled");
          }
          return fail(model_cancellation_probe_failed->load(
                              std::memory_order_relaxed)
                          ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                          : (read.diagnostic.code.empty()
                                 ? "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"
                                 : read.diagnostic.code),
                      read.diagnostic.detail.empty()
                          ? "current MGA-visible relation read failed"
                          : read.diagnostic.detail);
        }
        const auto post_access = api::LoadMgaRelationStorageDescriptor(
            context, source_input.object_uuid);
        if (!Rcp079ExactColumnarJoinStorageSnapshotV1(
                read.descriptor, preflight.descriptor) ||
            !Rcp079ExactColumnarJoinStorageSnapshotV1(
                read.descriptor, persisted) ||
            !post_access.ok ||
            !Rcp079ExactColumnarJoinStorageSnapshotV1(
                post_access.descriptor, read.descriptor) ||
            read.current_relation_base_generation == 0) {
          return fail("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                      "model-family descriptor changed during execution");
        }
        if (cancellation_requested()) {
          return fail(cancellation_diagnostic(),
                      "model-family execution was cancelled after MGA access");
        }
        const auto post_authorization = api::EvaluateMaterializedAuthorization(
            context, context.authorization_context, "SELECT",
            source_input.object_uuid);
        if (!post_authorization.authorized || post_authorization.denied ||
            post_authorization.policy_recheck_required ||
            !post_authorization.diagnostics.empty()) {
          return fail("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                      "model-family security recheck failed");
        }
        for (const auto& row : read.visible_rows) {
          if (cancellation_requested()) {
            return fail(cancellation_diagnostic(),
                        "model-family row validation was cancelled");
          }
          if (row.table_uuid != source_input.object_uuid ||
              !CanonicalUuidText(row.row_uuid) ||
              !CanonicalUuidText(row.version_uuid) ||
              row.values.size() != persisted.columns.size()) {
            return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                        "model-family row identity or width is invalid");
          }
          for (const auto& column : persisted.columns) {
            const std::string* encoded = nullptr;
            for (const auto& [name, value] : row.values) {
              if (name != column.canonical_name_key) continue;
              if (encoded != nullptr) {
                return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "model-family row repeats a field");
              }
              encoded = &value;
            }
            if (encoded == nullptr ||
                (*encoded == "<NULL>" && !column.nullable)) {
              return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                          "model-family row omits or nulls a required field");
            }
          }
        }
        const auto visible_row_memory =
            Rcp079VisibleRowsMemoryBytesV1(read.visible_rows);
        std::uint64_t spatial_preflight_peak_memory = 0;
        if (source_input.family_id == "spatial" &&
            columnar_runtime_memory_receipt != nullptr) {
          std::string_view match_predicate;
          if (has_match) {
            const auto match = std::ranges::find_if(
                dag.expressions, [](const auto& expression) {
                  return expression.operator_name == "SPATIAL_MATCH";
                });
            if (match != dag.expressions.end() &&
                match->child_expression_ids.size() > 1) {
              const auto predicate_id = match->child_expression_ids[1];
              const auto predicate = std::ranges::find_if(
                  dag.expressions, [&](const auto& expression) {
                    return expression.expression_id == predicate_id;
                  });
              if (predicate != dag.expressions.end() &&
                  predicate->literal_or_parameter_ref.has_value()) {
                match_predicate =
                    *predicate->literal_or_parameter_ref;
              }
            }
          }
          const auto materialization_memory =
              Rcp079SpatialSourceMaterializationAdditionalBytesV1(
                  read.visible_rows, dag, source_input, has_match,
                  has_nearest, match_predicate);
          std::uint64_t retained_and_materialized = 0;
          if (!visible_row_memory.has_value() ||
              *visible_row_memory >= source_input.maximum_memory_bytes ||
              !materialization_memory.has_value() ||
              !CheckedAdd(*visible_row_memory, *materialization_memory,
                          &retained_and_materialized) ||
              retained_and_materialized >
                  source_input.maximum_memory_bytes) {
            return fail(
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                "spatial source materialization exceeds the source memory grant");
          }
          spatial_preflight_peak_memory = retained_and_materialized;
        }
        if (source_input.family_id == "columnar" &&
            (!visible_row_memory.has_value() ||
             *visible_row_memory >= source_input.maximum_memory_bytes)) {
          return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "columnar MGA rows exhaust the source memory grant");
        }
        if (source_input.family_id == "columnar") {
          const auto materialization_memory =
              Rcp079ColumnarLogicalMaterializationAdditionalBytesV1(
                  persisted, read.visible_rows.size(), dag, source_input,
                  property_uuid, security_receipt_uuid, has_filter);
          std::uint64_t projected_live_memory = 0;
          std::uint64_t retained_and_materialized = 0;
          // Admission precedes every logical-row allocation.  Two complete
          // cohorts cover the retained provider image and the simultaneous
          // engine-owned exchange copy; the helper also includes request,
          // row-binding, result-identity, property, and four MGA-context
          // carriers.  The later measured receipt may be smaller, but may
          // never exceed this grant.
          if (!materialization_memory.has_value() ||
              !CheckedAdd(*visible_row_memory, *materialization_memory,
                          &retained_and_materialized) ||
              !CheckedMultiply(retained_and_materialized, 2,
                               &projected_live_memory) ||
              projected_live_memory > source_input.maximum_memory_bytes) {
            return fail(
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                "columnar logical materialization exceeds the source memory grant");
          }
        }

        auto& batch = provider.provider_batch;
        std::uint64_t contextual_post_consume_retained_memory = 0;
        const auto value_for = [](const api::CrudRowVersionRecord& row,
                                  const std::string_view name)
            -> const std::string* {
          const std::string* value = nullptr;
          for (const auto& [field, candidate] : row.values) {
            if (field != name) continue;
            if (value != nullptr) return nullptr;
            value = &candidate;
          }
          return value;
        };
        if (source_input.family_id == "spatial") {
          std::vector<api::nosql::SpatialSourceRowV1> source_rows;
          try {
            source_rows.reserve(read.visible_rows.size());
            for (const auto& row : read.visible_rows) {
              if (cancellation_requested()) {
                return fail(cancellation_diagnostic(),
                            "spatial source-row materialization was cancelled");
              }
              const auto row_uuid = value_for(row, "row_uuid");
              const auto point = value_for(row, "spatial_value");
              const auto crs = value_for(row, "crs_uuid");
              if (row_uuid == nullptr || point == nullptr || crs == nullptr ||
                  *row_uuid != row.row_uuid ||
                  *crs != source_input.spatial_crs_uuid) {
                return fail("SB_MODEL_SPATIAL_CRS_MISMATCH_V1",
                            "persistent spatial row identity or CRS changed");
              }
              source_rows.push_back(
                  {row.row_uuid,
                   std::vector<std::uint8_t>(point->begin(), point->end()),
                   *crs});
            }
          } catch (const std::bad_alloc&) {
            return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                        "spatial source-row allocation was refused");
          } catch (const std::length_error&) {
            return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                        "spatial source-row length was refused");
          }
          const auto expression_for = [&](const std::uint32_t expression_id) {
            return std::ranges::find_if(
                dag.expressions, [&](const auto& expression) {
                  return expression.expression_id == expression_id;
                });
          };
          const auto operation_for = [&](const std::string_view name) {
            return std::ranges::find_if(
                dag.expressions, [&](const auto& expression) {
                  return expression.operator_name == name;
                });
          };
          bool spatial_evaluation_resource_refused = false;
          const auto evaluate_point = [&](const auto& operation,
                                          const api::EngineCanonicalExpressionConsumer
                                              consumer,
                                          std::vector<std::uint8_t>* bytes) {
            try {
              if (operation == dag.expressions.end() || bytes == nullptr) {
                return false;
              }
              const auto point_ordinal =
                  operation->operator_name == "SPATIAL_MATCH"
                      ? std::size_t{2}
                      : std::size_t{1};
              const auto point = expression_for(
                  operation->child_expression_ids[point_ordinal]);
              if (point == dag.expressions.end()) return false;
              CanonicalRelationalExpressionRuntime runtime(dag);
              api::EngineTypedValue value;
              std::string detail;
              if (!runtime.EvaluateForConsumer(
                      point->expression_id, "geometry", consumer, &value,
                      &detail) || value.isSqlNull() ||
                  value.binary_value.empty()) {
                return false;
              }
              *bytes = std::move(value.binary_value);
              return true;
            } catch (const std::bad_alloc&) {
              spatial_evaluation_resource_refused = true;
              return false;
            } catch (const std::length_error&) {
              spatial_evaluation_resource_refused = true;
              return false;
            }
          };
          const auto make_spatial_request =
              [&](std::vector<api::nosql::SpatialSourceRowV1>&& request_rows,
                  const std::uint64_t retained_outside_api_bytes)
              -> std::optional<api::nosql::SpatialExecutionRequestV2> {
            std::uint64_t retained_bytes = 0;
            if (!visible_row_memory.has_value() ||
                !CheckedAdd(*visible_row_memory,
                            retained_outside_api_bytes, &retained_bytes) ||
                retained_bytes >= source_input.maximum_memory_bytes) {
              return std::nullopt;
            }
            try {
              api::nosql::SpatialExecutionRequestV2 request;
              request.object_uuid = source_input.object_uuid;
              request.geometry_descriptor_uuid =
                  source_input.spatial_geometry_descriptor_uuid;
              request.geometry_type_uuid =
                  source_input.spatial_geometry_type_uuid;
              request.crs_uuid = source_input.spatial_crs_uuid;
              request.crs_generation = source_input.spatial_crs_generation;
              request.source_generation = source_input.descriptor_generation;
              request.catalog_generation = source_input.catalog_generation;
              request.policy_generation = source_input.policy_generation;
              request.security_generation = source_input.security_generation;
              request.resource_generation = source_input.resource_generation;
              request.route_generation = context.optimizer_route_generation;
              request.statement_context = source_input.mga_statement_context;
              request.current_statement_context =
                  source_input.mga_statement_context;
              request.source_rows = std::move(request_rows);
              request.maximum_rows = source_input.maximum_rows;
              request.maximum_memory_bytes =
                  source_input.maximum_memory_bytes - retained_bytes;
              request.cancellation_requested = [](const void* context) {
                return (*static_cast<const std::function<bool()>*>(context))();
              };
              request.cancellation_context = &cancellation_requested;
              request.security_admitted = true;
              request.exact_scan_fallback_available = true;
              return request;
            } catch (const std::bad_alloc&) {
              return std::nullopt;
            } catch (const std::length_error&) {
              return std::nullopt;
            }
          };
          auto current_source_rows = std::move(source_rows);
          std::vector<api::nosql::SpatialResultRowV1> rows;
          std::optional<api::nosql::SpatialPoint2dV1> match_query_point;
          if (has_match) {
            const auto match = operation_for("SPATIAL_MATCH");
            const auto predicate = expression_for(match->child_expression_ids[1]);
            auto match_request = make_spatial_request(
                std::move(current_source_rows), 0);
            if (!match_request.has_value()) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial match byte grant is unavailable");
            }
            try {
              match_request->operation_id = "SPATIAL_MATCH";
              match_request->predicate_id =
                  predicate == dag.expressions.end() ||
                          !predicate->literal_or_parameter_ref.has_value()
                      ? std::string{}
                      : *predicate->literal_or_parameter_ref;
              match_request->query_crs_uuid = source_input.spatial_crs_uuid;
            } catch (const std::bad_alloc&) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial match binding allocation was refused");
            } catch (const std::length_error&) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial match binding length was refused");
            }
            if (!evaluate_point(
                    match, api::EngineCanonicalExpressionConsumer::filter,
                    &match_request->encoded_query_point)) {
              return fail(spatial_evaluation_resource_refused
                              ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                              : "SB_MODEL_SPATIAL_COORDINATE_INVALID_V1",
                          "SPATIAL_MATCH POINT evaluation failed");
            }
            if (match_request->predicate_id != "INTERSECTS" &&
                match_request->predicate_id != "CONTAINS") {
              return fail("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                          "SPATIAL_MATCH predicate is not admitted");
            }
            std::array<char, 24> retained_match_query_bytes{};
            const bool match_query_bytes_retained =
                match_request->encoded_query_point.size() ==
                retained_match_query_bytes.size();
            if (match_query_bytes_retained) {
              std::ranges::transform(
                  match_request->encoded_query_point,
                  retained_match_query_bytes.begin(),
                  [](const std::uint8_t byte) {
                    return static_cast<char>(byte);
                  });
            }
            const auto match_source_count =
                match_request->source_rows.size();
            const auto match_memory_grant =
                match_request->maximum_memory_bytes;
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "spatial match execution was cancelled before dispatch");
            }
            auto matched =
                api::nosql::ExecuteSpatialNativeV2(
                    std::move(*match_request));
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "spatial match execution was cancelled after dispatch");
            }
            if (!matched.accepted || !matched.root_publishable) {
              return fail(matched.diagnostic_id.empty()
                              ? "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"
                              : matched.diagnostic_id,
                          matched.detail.empty()
                              ? "spatial match execution did not publish"
                              : matched.detail);
            }
            if (!match_query_bytes_retained ||
                !api::nosql::DecodeSpatialPoint2dV1(
                    std::string_view(retained_match_query_bytes.data(),
                                     retained_match_query_bytes.size()),
                    &match_query_point.emplace())) {
              return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                          "accepted spatial match query point changed");
            }
            if (!matched.exact_fallback_selected ||
                !matched.candidate_recheck_complete ||
                !matched.mga_recheck_complete ||
                matched.cancellation_observed ||
                matched.cancellation_probe_failed ||
                !matched.memory_receipt_complete ||
                matched.current_live_memory_bytes >
                    matched.peak_live_memory_bytes ||
                matched.peak_live_memory_bytes >
                    matched.memory_grant_bytes ||
                matched.memory_grant_bytes != match_memory_grant ||
                matched.physical_operator_id !=
                    "SPATIAL_EXACT_GEOMETRY_SCAN_V1" ||
                matched.diagnostic_id != "SB_EXECUTOR_OK" ||
                !matched.detail.empty() ||
                match_source_count != read.visible_rows.size() ||
                match_source_count > source_input.maximum_rows ||
                matched.rows.size() > match_source_count ||
                matched.rows.size() > source_input.maximum_rows ||
                (matched.rows.size() != 0 &&
                 public_columns.size() >
                     source_input.maximum_cells / matched.rows.size())) {
              return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                          "spatial match execution receipt changed");
            }
            std::uint64_t match_api_peak = 0;
            if (!CheckedAdd(*visible_row_memory,
                            matched.peak_live_memory_bytes,
                            &match_api_peak) ||
                match_api_peak > source_input.maximum_memory_bytes) {
              return fail(
                  "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "spatial match retained rows and API peak exceed the source memory grant");
            }
            columnar_runtime_memory_receipt->peak_live_memory_bytes =
                std::max(columnar_runtime_memory_receipt
                             ->peak_live_memory_bytes,
                         match_api_peak);
            std::size_t matched_ordinal = 0;
            for (std::size_t source_ordinal = 0;
                 source_ordinal < read.visible_rows.size();
                 ++source_ordinal) {
              if (cancellation_requested()) {
                return fail(cancellation_diagnostic(),
                            "spatial match receipt replay was cancelled");
              }
              const auto& retained_row = read.visible_rows[source_ordinal];
              const auto* retained_row_uuid =
                  value_for(retained_row, "row_uuid");
              const auto* retained_point =
                  value_for(retained_row, "spatial_value");
              const auto* retained_crs = value_for(retained_row, "crs_uuid");
              api::nosql::SpatialPoint2dV1 retained_decoded_point;
              if (retained_row_uuid == nullptr || retained_point == nullptr ||
                  retained_crs == nullptr ||
                  retained_row.row_uuid != *retained_row_uuid ||
                  *retained_crs != source_input.spatial_crs_uuid ||
                  !api::nosql::DecodeSpatialPoint2dV1(
                      std::string_view(*retained_point),
                      &retained_decoded_point)) {
                return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "spatial match source cohort changed");
              }
              if (retained_decoded_point.x != match_query_point->x ||
                  retained_decoded_point.y != match_query_point->y) {
                continue;
              }
              if (matched_ordinal >= matched.rows.size()) {
                return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "spatial match result omitted an exact match");
              }
              const auto& actual = matched.rows[matched_ordinal++];
              if (actual.row_uuid != retained_row.row_uuid ||
                  actual.encoded_point.size() != retained_point->size() ||
                  !std::ranges::equal(
                      actual.encoded_point, *retained_point,
                      [](const std::uint8_t actual_byte,
                         const char retained_byte) {
                        return actual_byte ==
                               static_cast<std::uint8_t>(retained_byte);
                      }) ||
                  actual.crs_uuid != *retained_crs ||
                  actual.crs_uuid != source_input.spatial_crs_uuid ||
                  !actual.predicate_truth || actual.distance != 0.0 ||
                  std::signbit(actual.distance)) {
                return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "spatial match result changed before publication");
              }
            }
            if (matched_ordinal != matched.rows.size()) {
              return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                          "spatial match result added or reordered a row");
            }
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "spatial match execution was cancelled before handoff");
            }
            if (has_nearest) {
              std::uint64_t handoff_row_bytes = 0;
              std::uint64_t handoff_profile_bytes = 0;
              std::uint64_t handoff_peak = 0;
              if (!CheckedMultiply(matched.rows.size(),
                                   sizeof(api::nosql::SpatialSourceRowV1),
                                   &handoff_row_bytes) ||
                  !CheckedMultiply(
                      matched.rows.size(),
                      api::nosql::kSpatialNativeCartesianPoint2dV1.size() + 1,
                      &handoff_profile_bytes) ||
                  !CheckedAdd(*visible_row_memory,
                              matched.current_live_memory_bytes,
                              &handoff_peak) ||
                  !CheckedAdd(handoff_peak, handoff_row_bytes,
                              &handoff_peak) ||
                  !CheckedAdd(handoff_peak, handoff_profile_bytes,
                              &handoff_peak) ||
                  handoff_peak > source_input.maximum_memory_bytes) {
                return fail(
                    "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "spatial match-to-nearest handoff exceeds the source memory grant");
              }
              try {
                std::vector<api::nosql::SpatialSourceRowV1>{}.swap(
                    current_source_rows);
                current_source_rows.reserve(matched.rows.size());
                for (auto& row : matched.rows) {
                  if (cancellation_requested()) {
                    return fail(
                        cancellation_diagnostic(),
                        "spatial match-to-nearest handoff was cancelled");
                  }
                  current_source_rows.push_back(
                      {std::move(row.row_uuid),
                       std::move(row.encoded_point),
                       std::move(row.crs_uuid)});
                }
              } catch (const std::bad_alloc&) {
                return fail(
                    "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "spatial match-to-nearest handoff allocation was refused");
              } catch (const std::length_error&) {
                return fail(
                    "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "spatial match-to-nearest handoff length was refused");
              }
              columnar_runtime_memory_receipt->peak_live_memory_bytes =
                  std::max(columnar_runtime_memory_receipt
                               ->peak_live_memory_bytes,
                           handoff_peak);
            } else {
              rows = std::move(matched.rows);
            }
          }
          if (has_nearest) {
            const auto nearest = operation_for("SPATIAL_NEAREST");
            const auto top_k = expression_for(nearest->child_expression_ids[3]);
            auto nearest_request = make_spatial_request(
                std::move(current_source_rows), 0);
            if (!nearest_request.has_value()) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial nearest byte grant is unavailable");
            }
            try {
              nearest_request->operation_id = "SPATIAL_NEAREST";
              nearest_request->predicate_id.clear();
              nearest_request->query_crs_uuid =
                  source_input.spatial_crs_uuid;
            } catch (const std::bad_alloc&) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial nearest binding allocation was refused");
            } catch (const std::length_error&) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial nearest binding length was refused");
            }
            if (!evaluate_point(
                    nearest,
                    api::EngineCanonicalExpressionConsumer::projection,
                    &nearest_request->encoded_query_point) ||
                top_k == dag.expressions.end() ||
                !top_k->literal_or_parameter_ref.has_value()) {
              return fail(spatial_evaluation_resource_refused
                              ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                              : "SB_MODEL_SPATIAL_COORDINATE_INVALID_V1",
                          "SPATIAL_NEAREST binding is incomplete");
            }
            const auto parsed = std::from_chars(
                top_k->literal_or_parameter_ref->data(),
                top_k->literal_or_parameter_ref->data() +
                    top_k->literal_or_parameter_ref->size(),
                nearest_request->top_k);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != top_k->literal_or_parameter_ref->data() +
                                  top_k->literal_or_parameter_ref->size() ||
                nearest_request->top_k == 0 ||
                nearest_request->top_k > 4096) {
              return fail("SB_MODEL_SPATIAL_TOP_K_REFUSED_V1",
                          "SPATIAL_NEAREST top-k is invalid");
            }
            std::array<char, 24> retained_nearest_query_bytes{};
            const bool nearest_query_bytes_retained =
                nearest_request->encoded_query_point.size() ==
                retained_nearest_query_bytes.size();
            if (nearest_query_bytes_retained) {
              std::ranges::transform(
                  nearest_request->encoded_query_point,
                  retained_nearest_query_bytes.begin(),
                  [](const std::uint8_t byte) {
                    return static_cast<char>(byte);
                  });
            }
            const auto nearest_source_count =
                nearest_request->source_rows.size();
            const auto nearest_top_k = nearest_request->top_k;
            const auto nearest_memory_grant =
                nearest_request->maximum_memory_bytes;
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "spatial nearest execution was cancelled before dispatch");
            }
            auto nearest_result =
                api::nosql::ExecuteSpatialNativeV2(
                    std::move(*nearest_request));
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "spatial nearest execution was cancelled after dispatch");
            }
            if (!nearest_result.accepted || !nearest_result.root_publishable) {
              return fail(nearest_result.diagnostic_id.empty()
                              ? "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"
                              : nearest_result.diagnostic_id,
                          nearest_result.detail.empty()
                              ? "spatial nearest execution did not publish"
                              : nearest_result.detail);
            }
            api::nosql::SpatialPoint2dV1 nearest_query_point;
            if (!nearest_query_bytes_retained ||
                !api::nosql::DecodeSpatialPoint2dV1(
                    std::string_view(retained_nearest_query_bytes.data(),
                                     retained_nearest_query_bytes.size()),
                    &nearest_query_point)) {
              return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                          "accepted spatial nearest query point changed");
            }
            std::size_t eligible_count = 0;
            for (const auto& retained_row : read.visible_rows) {
              if (cancellation_requested()) {
                return fail(cancellation_diagnostic(),
                            "spatial nearest eligibility replay was cancelled");
              }
              const auto* retained_point =
                  value_for(retained_row, "spatial_value");
              api::nosql::SpatialPoint2dV1 source_point;
              if (retained_point == nullptr ||
                  !api::nosql::DecodeSpatialPoint2dV1(
                      std::string_view(*retained_point), &source_point)) {
                return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "spatial nearest retained source point changed");
              }
              if (match_query_point.has_value() &&
                  (source_point.x != match_query_point->x ||
                   source_point.y != match_query_point->y)) {
                continue;
              }
              ++eligible_count;
            }
            const auto expected_count = std::min<std::size_t>(
                eligible_count, nearest_top_k);
            if (!nearest_result.exact_fallback_selected ||
                !nearest_result.candidate_recheck_complete ||
                !nearest_result.mga_recheck_complete ||
                nearest_result.cancellation_observed ||
                nearest_result.cancellation_probe_failed ||
                !nearest_result.memory_receipt_complete ||
                nearest_result.current_live_memory_bytes >
                    nearest_result.peak_live_memory_bytes ||
                nearest_result.peak_live_memory_bytes >
                    nearest_result.memory_grant_bytes ||
                nearest_result.memory_grant_bytes !=
                    nearest_memory_grant ||
                nearest_result.physical_operator_id !=
                    "SPATIAL_EXACT_GEOMETRY_SCAN_V1" ||
                nearest_result.diagnostic_id != "SB_EXECUTOR_OK" ||
                !nearest_result.detail.empty() ||
                nearest_source_count != eligible_count ||
                nearest_source_count > source_input.maximum_rows ||
                nearest_result.rows.size() != expected_count ||
                nearest_result.rows.size() > source_input.maximum_rows ||
                nearest_result.rows.size() > nearest_top_k ||
                (nearest_result.rows.size() != 0 &&
                 public_columns.size() >
                     source_input.maximum_cells /
                         nearest_result.rows.size())) {
              return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                          "spatial nearest execution receipt changed");
            }
            std::uint64_t nearest_api_peak = 0;
            if (!CheckedAdd(*visible_row_memory,
                            nearest_result.peak_live_memory_bytes,
                            &nearest_api_peak) ||
                nearest_api_peak > source_input.maximum_memory_bytes) {
              return fail(
                  "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "spatial nearest retained rows and API peak exceed the source memory grant");
            }
            struct ExpectedNearestRowV1 {
              std::size_t visible_ordinal{0};
              double distance{0.0};
            };
            const auto nearest_key_less =
                [&](const ExpectedNearestRowV1& left,
                    const ExpectedNearestRowV1& right) {
                  const auto& left_uuid =
                      read.visible_rows[left.visible_ordinal].row_uuid;
                  const auto& right_uuid =
                      read.visible_rows[right.visible_ordinal].row_uuid;
                  return left.distance < right.distance ||
                         (left.distance == right.distance &&
                          left_uuid < right_uuid);
                };
            std::uint64_t expected_nearest_bytes = 0;
            std::uint64_t nearest_oracle_peak = 0;
            if (!CheckedMultiply(expected_count,
                                 sizeof(ExpectedNearestRowV1),
                                 &expected_nearest_bytes) ||
                !CheckedAdd(*visible_row_memory,
                            nearest_result.current_live_memory_bytes,
                            &nearest_oracle_peak) ||
                !CheckedAdd(nearest_oracle_peak,
                            expected_nearest_bytes,
                            &nearest_oracle_peak) ||
                nearest_oracle_peak > source_input.maximum_memory_bytes) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial nearest verification exceeds the source memory grant");
            }
            std::unique_ptr<ExpectedNearestRowV1[]> expected_nearest;
            std::size_t expected_nearest_size = 0;
            try {
              if (expected_count != 0) {
                expected_nearest =
                    std::make_unique<ExpectedNearestRowV1[]>(expected_count);
              }
              for (std::size_t visible_ordinal = 0;
                   visible_ordinal < read.visible_rows.size();
                   ++visible_ordinal) {
                if (cancellation_requested()) {
                  return fail(
                      cancellation_diagnostic(),
                      "spatial nearest oracle construction was cancelled");
                }
                api::nosql::SpatialPoint2dV1 source_point;
                const auto& retained_row =
                    read.visible_rows[visible_ordinal];
                const auto* retained_point =
                    value_for(retained_row, "spatial_value");
                if (retained_point == nullptr ||
                    !api::nosql::DecodeSpatialPoint2dV1(
                        std::string_view(*retained_point), &source_point)) {
                  return fail("SB_MODEL_SPATIAL_COORDINATE_INVALID_V1",
                              "spatial nearest source point changed");
                }
                if (match_query_point.has_value() &&
                    (source_point.x != match_query_point->x ||
                     source_point.y != match_query_point->y)) {
                  continue;
                }
                const auto distance = std::hypot(
                    source_point.x - nearest_query_point.x,
                    source_point.y - nearest_query_point.y);
                if (!std::isfinite(distance) || distance < 0.0) {
                  return fail("SB_MODEL_SPATIAL_COORDINATE_INVALID_V1",
                              "spatial nearest oracle distance overflowed");
                }
                ExpectedNearestRowV1 candidate{visible_ordinal, distance};
                if (expected_nearest_size < expected_count) {
                  expected_nearest[expected_nearest_size++] = candidate;
                  std::push_heap(expected_nearest.get(),
                                 expected_nearest.get() +
                                     expected_nearest_size,
                                 nearest_key_less);
                } else if (expected_count != 0 &&
                           nearest_key_less(candidate,
                                            expected_nearest[0])) {
                  std::pop_heap(expected_nearest.get(),
                                expected_nearest.get() +
                                    expected_nearest_size,
                                nearest_key_less);
                  expected_nearest[expected_nearest_size - 1] = candidate;
                  std::push_heap(expected_nearest.get(),
                                 expected_nearest.get() +
                                     expected_nearest_size,
                                 nearest_key_less);
                }
              }
              if (cancellation_requested()) {
                return fail(cancellation_diagnostic(),
                            "spatial nearest ordering was cancelled");
              }
              if (expected_nearest_size != 0) {
                std::sort_heap(expected_nearest.get(),
                               expected_nearest.get() +
                                   expected_nearest_size,
                               nearest_key_less);
              }
              for (std::size_t ordinal = 0;
                   ordinal < expected_nearest_size; ++ordinal) {
                if (cancellation_requested()) {
                  return fail(cancellation_diagnostic(),
                              "spatial nearest receipt replay was cancelled");
                }
                const auto& expected = expected_nearest[ordinal];
                const auto& retained_row =
                    read.visible_rows[expected.visible_ordinal];
                const auto* retained_point =
                    value_for(retained_row, "spatial_value");
                const auto* retained_crs =
                    value_for(retained_row, "crs_uuid");
                const auto& actual = nearest_result.rows[ordinal];
                if (retained_point == nullptr || retained_crs == nullptr ||
                    actual.row_uuid != retained_row.row_uuid ||
                    actual.encoded_point.size() != retained_point->size() ||
                    !std::ranges::equal(
                        actual.encoded_point, *retained_point,
                        [](const std::uint8_t actual_byte,
                           const char retained_byte) {
                          return actual_byte ==
                                 static_cast<std::uint8_t>(retained_byte);
                        }) ||
                    actual.crs_uuid != *retained_crs ||
                    actual.crs_uuid != source_input.spatial_crs_uuid ||
                    actual.predicate_truth ||
                    actual.distance != expected.distance ||
                    std::signbit(actual.distance) !=
                        std::signbit(expected.distance)) {
                  return fail(
                      "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "spatial nearest result changed before publication");
                }
              }
            } catch (const std::bad_alloc&) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial nearest verification allocation was refused");
            } catch (const std::length_error&) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial nearest verification length was refused");
            }
            columnar_runtime_memory_receipt->peak_live_memory_bytes =
                std::max({columnar_runtime_memory_receipt
                              ->peak_live_memory_bytes,
                          nearest_api_peak, nearest_oracle_peak});
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "spatial nearest execution was cancelled before handoff");
            }
            rows = std::move(nearest_result.rows);
          } else if (!has_match) {
            auto source_request = make_spatial_request(
                std::move(current_source_rows), 0);
            if (!source_request.has_value()) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial source byte receipt is unavailable");
            }
            try {
              source_request->operation_id = "SPATIAL_SOURCE";
            } catch (const std::bad_alloc&) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial source binding allocation was refused");
            } catch (const std::length_error&) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "spatial source binding length was refused");
            }
            const auto source_memory_grant =
                source_request->maximum_memory_bytes;
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "spatial source execution was cancelled before dispatch");
            }
            auto sourced =
                api::nosql::ExecuteSpatialNativeV2(
                    std::move(*source_request));
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "spatial source execution was cancelled after dispatch");
            }
            if (!sourced.accepted || !sourced.root_publishable) {
              return fail(sourced.diagnostic_id.empty()
                              ? "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"
                              : sourced.diagnostic_id,
                          sourced.detail.empty()
                              ? "spatial source execution did not publish"
                              : sourced.detail);
            }
            if (!sourced.exact_fallback_selected ||
                !sourced.candidate_recheck_complete ||
                !sourced.mga_recheck_complete ||
                sourced.cancellation_observed ||
                sourced.cancellation_probe_failed ||
                !sourced.memory_receipt_complete ||
                sourced.current_live_memory_bytes >
                    sourced.peak_live_memory_bytes ||
                sourced.peak_live_memory_bytes >
                    sourced.memory_grant_bytes ||
                sourced.memory_grant_bytes != source_memory_grant ||
                sourced.physical_operator_id !=
                    "SPATIAL_EXACT_GEOMETRY_SCAN_V1" ||
                sourced.diagnostic_id != "SB_EXECUTOR_OK" ||
                !sourced.detail.empty() ||
                sourced.rows.size() != read.visible_rows.size() ||
                sourced.rows.size() > source_input.maximum_rows ||
                (sourced.rows.size() != 0 &&
                 public_columns.size() >
                     source_input.maximum_cells / sourced.rows.size())) {
              return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                          "spatial source execution receipt changed");
            }
            std::uint64_t retained_api_peak_memory = 0;
            if (!CheckedAdd(*visible_row_memory,
                            sourced.peak_live_memory_bytes,
                            &retained_api_peak_memory) ||
                retained_api_peak_memory >
                    source_input.maximum_memory_bytes) {
              return fail(
                  "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "spatial source retained rows and API peak exceed the source memory grant");
            }
            for (std::size_t ordinal = 0;
                 ordinal < read.visible_rows.size(); ++ordinal) {
              if (cancellation_requested()) {
                return fail(cancellation_diagnostic(),
                            "spatial source receipt replay was cancelled");
              }
              const auto& retained_row = read.visible_rows[ordinal];
              const auto* retained_row_uuid =
                  value_for(retained_row, "row_uuid");
              const auto* retained_point =
                  value_for(retained_row, "spatial_value");
              const auto* retained_crs =
                  value_for(retained_row, "crs_uuid");
              const auto& actual = sourced.rows[ordinal];
              if (retained_row_uuid == nullptr || retained_point == nullptr ||
                  retained_crs == nullptr ||
                  actual.row_uuid != retained_row.row_uuid ||
                  actual.row_uuid != *retained_row_uuid ||
                  actual.encoded_point.size() != retained_point->size() ||
                  !std::ranges::equal(
                      actual.encoded_point, *retained_point,
                      [](const std::uint8_t actual_byte,
                         const char retained_byte) {
                        return actual_byte ==
                               static_cast<std::uint8_t>(retained_byte);
                      }) ||
                  actual.crs_uuid != *retained_crs ||
                  actual.crs_uuid != source_input.spatial_crs_uuid ||
                  actual.predicate_truth || actual.distance != 0.0 ||
                  std::signbit(actual.distance)) {
                return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "spatial source result changed before publication");
              }
            }
            const auto provider_build_memory =
                Rcp079SpatialProviderBuildAdditionalBytesV1(
                    sourced.rows, public_columns, source_input,
                    property_uuid, security_receipt_uuid,
                    has_match, has_nearest);
            const auto retained_result_rows_memory =
                Rcp079SpatialResultRowsLogicalMemoryBytesV1(sourced.rows);
            std::uint64_t provider_build_peak_memory = 0;
            if (!provider_build_memory.has_value() ||
                !retained_result_rows_memory.has_value() ||
                !CheckedAdd(*visible_row_memory,
                            *retained_result_rows_memory,
                            &provider_build_peak_memory) ||
                !CheckedAdd(provider_build_peak_memory,
                            *provider_build_memory,
                            &provider_build_peak_memory) ||
                provider_build_peak_memory >
                    source_input.maximum_memory_bytes) {
              return fail(
                  "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "spatial provider materialization exceeds the source memory grant");
            }
            columnar_runtime_memory_receipt->peak_live_memory_bytes =
                std::max({spatial_preflight_peak_memory,
                          retained_api_peak_memory,
                          provider_build_peak_memory});
            rows = std::move(sourced.rows);
          }
          const auto spatial_provider_build_memory =
              Rcp079SpatialProviderBuildAdditionalBytesV1(
                  rows, public_columns, source_input, property_uuid,
                  security_receipt_uuid, has_match, has_nearest);
          const auto retained_spatial_rows_memory =
              Rcp079SpatialResultRowsLogicalMemoryBytesV1(rows);
          std::uint64_t spatial_provider_peak_memory = 0;
          if (!spatial_provider_build_memory.has_value() ||
              !retained_spatial_rows_memory.has_value() ||
              !CheckedAdd(*visible_row_memory,
                          *retained_spatial_rows_memory,
                          &spatial_provider_peak_memory) ||
              !CheckedAdd(spatial_provider_peak_memory,
                          *spatial_provider_build_memory,
                          &spatial_provider_peak_memory) ||
              spatial_provider_peak_memory >
                  source_input.maximum_memory_bytes) {
            return fail(
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                "spatial provider materialization exceeds the source memory grant");
          }
          columnar_runtime_memory_receipt->peak_live_memory_bytes =
              std::max({columnar_runtime_memory_receipt
                            ->peak_live_memory_bytes,
                        spatial_preflight_peak_memory,
                        spatial_provider_peak_memory});
          try {
            batch.properties.ordering_id =
                has_nearest ? "spatial_distance_row_uuid_ascending_v1"
                            : "fixture_order";
            batch.batch.columns = public_columns;
            batch.batch.rows.reserve(rows.size());
            batch.ordered_row_identities.reserve(rows.size());
            for (auto& row : rows) {
              if (cancellation_requested()) {
                return fail(cancellation_diagnostic(),
                            "spatial provider materialization was cancelled");
              }
              exec::DescriptorTuple tuple;
              tuple.values.reserve(public_columns.size());
              for (std::size_t ordinal = 0; ordinal < public_columns.size();
                   ++ordinal) {
                if (cancellation_requested()) {
                  return fail(
                      cancellation_diagnostic(),
                      "spatial provider value materialization was cancelled");
                }
                api::EngineTypedValue value;
                value.descriptor = public_columns[ordinal].descriptor;
                value.setState(api::EngineValueState::value);
                if (ordinal == 0) value.encoded_value = row.row_uuid;
                if (ordinal == 1) {
                  value.binary_value = std::move(row.encoded_point);
                }
                if (ordinal == 2) {
                  value.encoded_value = std::move(row.crs_uuid);
                }
                if (ordinal == 3 && has_match) value.encoded_value = "true";
                if ((ordinal == 3 && !has_match && has_nearest) ||
                    ordinal == 4) {
                  value.encoded_value = Rcp079CanonicalReal64(row.distance);
                }
                tuple.values.push_back(std::move(value));
              }
              batch.batch.rows.push_back(std::move(tuple));
              exec::ModelProviderRowIdentityV1 identity;
              identity.row_uuid = std::move(row.row_uuid);
              batch.ordered_row_identities.push_back(std::move(identity));
            }
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "spatial provider materialization was cancelled before publication");
            }
          } catch (const std::bad_alloc&) {
            return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                        "spatial provider allocation was refused");
          } catch (const std::length_error&) {
            return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                        "spatial provider length was refused");
          }
        } else {
          exec::DescriptorBatch logical_rows;
          std::vector<std::uint32_t> logical_descriptor_ids;
          logical_rows.columns.reserve(persisted.columns.size());
          logical_descriptor_ids.reserve(persisted.columns.size());
          std::uint32_t synthetic_descriptor = 0x80000000u;
          for (const auto& column : persisted.columns) {
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "columnar descriptor reconstruction was cancelled");
            }
            const auto identifier = std::ranges::find_if(
                dag.expressions, [&](const auto& expression) {
                  return expression.expression_kind ==
                             api::RelationalExpressionKind::kIdentifier &&
                         expression.bound_name_uuid ==
                             std::optional<std::string>(
                                 column.column_uuid);
                });
            const auto descriptor_id =
                identifier == dag.expressions.end()
                    ? synthetic_descriptor++
                    : identifier->result_descriptor_id;
            auto descriptor = column.value_descriptor;
            const auto public_column = std::ranges::find_if(
                public_columns, [&](const auto& candidate) {
                  return candidate.descriptor_id == descriptor_id &&
                         candidate.stable_name == column.canonical_name_key;
                });
            if (public_column != public_columns.end()) {
              // Public execution columns have already been bound to the exact
              // live datatype descriptor selected by the statement DAG.  Feed
              // that same descriptor into reconstruction so the provider
              // cannot return the persisted outer handle as a result carrier.
              descriptor = public_column->descriptor;
            }
            descriptor.descriptor_kind = "scalar";
            logical_rows.columns.push_back(
                {column.canonical_name_key, descriptor, column.nullable,
                 descriptor_id});
            logical_descriptor_ids.push_back(descriptor_id);
          }
          std::vector<std::string> row_uuids;
          row_uuids.reserve(read.visible_rows.size());
          for (const auto& row : read.visible_rows) {
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "columnar row reconstruction was cancelled");
            }
            exec::DescriptorTuple tuple;
            tuple.values.reserve(persisted.columns.size());
            for (std::size_t ordinal = 0; ordinal < persisted.columns.size();
                 ++ordinal) {
              if (cancellation_requested()) {
                return fail(cancellation_diagnostic(),
                            "columnar value reconstruction was cancelled");
              }
              const auto* encoded = value_for(
                  row, persisted.columns[ordinal].canonical_name_key);
              if (encoded == nullptr) {
                return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "persistent columnar row omits a logical column");
              }
              api::EngineTypedValue value;
              value.descriptor = logical_rows.columns[ordinal].descriptor;
              if (*encoded == "<NULL>") {
                value.setState(api::EngineValueState::sql_null);
              } else {
                value.encoded_value = *encoded;
                value.setState(api::EngineValueState::value);
              }
              tuple.values.push_back(std::move(value));
            }
            logical_rows.rows.push_back(std::move(tuple));
            row_uuids.push_back(row.row_uuid);
          }
          const auto retained_visible_row_memory =
              Rcp079VisibleRowsMemoryBytesV1(read.visible_rows);
          if (!retained_visible_row_memory.has_value() ||
              *retained_visible_row_memory >=
                  source_input.maximum_memory_bytes) {
            return fail(
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                "columnar retained MGA carriers exhaust the source memory grant");
          }
          api::nosql::ColumnarExecutionRequestV2 columnar_request;
          columnar_request.operation_ids = source_input.operation_ids;
          columnar_request.operation_id = source_input.operation_id;
          columnar_request.relation_uuid = source_input.object_uuid;
          columnar_request.row_uuids = std::move(row_uuids);
          columnar_request.statement_context = source_input.mga_statement_context;
          columnar_request.current_statement_context =
              source_input.mga_statement_context;
          columnar_request.source_generation = source_input.descriptor_generation;
          columnar_request.catalog_generation = source_input.catalog_generation;
          columnar_request.summary_generation = source_input.provider_generation;
          columnar_request.maximum_rows = source_input.maximum_rows;
          columnar_request.maximum_input_cells =
              static_cast<std::size_t>(maximum_reconstruction_cells);
          columnar_request.maximum_cells = source_input.maximum_cells;
          columnar_request.maximum_memory_bytes =
              source_input.maximum_memory_bytes -
              *retained_visible_row_memory;
          columnar_request.cancellation_requested = [](const void* context) {
            return (*static_cast<const std::function<bool()>*>(context))();
          };
          columnar_request.cancellation_context = &cancellation_requested;
          columnar_request.security_admitted = true;
          columnar_request.exact_reconstruction_fallback_available = true;
          std::uint64_t row_binding_peak_structural_bytes = 0;
          std::uint64_t pre_api_peak_memory = 0;
          std::uint64_t contextual_pre_api_peak_memory = 0;
          std::uint64_t expected_columnar_api_memory_grant =
              columnar_request.maximum_memory_bytes;
          std::string setup_diagnostic;
          std::string setup_detail;
          const auto prepare_projection = [&]() {
            if (!has_project) return true;
            const auto project = std::ranges::find_if(
                dag.expressions, [](const auto& expression) {
                  return expression.operator_name == "COLUMNAR_PROJECT";
                });
            if (project == dag.expressions.end() ||
                project->child_expression_ids.size() < 2) {
              setup_diagnostic = "SB_MODEL_COLUMNAR_PROJECTION_INVALID_V1";
              setup_detail = "COLUMNAR_PROJECT root is invalid";
              return false;
            }
            for (std::size_t index = 1;
                 index < project->child_expression_ids.size(); ++index) {
              if (cancellation_requested()) {
                setup_diagnostic = cancellation_diagnostic();
                setup_detail = "columnar projection binding was cancelled";
                return false;
              }
              const auto expression = std::ranges::find_if(
                  dag.expressions, [&](const auto& candidate) {
                    return candidate.expression_id ==
                           project->child_expression_ids[index];
                  });
              if (expression == dag.expressions.end() ||
                  !expression->bound_name_uuid.has_value()) {
                setup_diagnostic = "SB_MODEL_COLUMNAR_PROJECTION_INVALID_V1";
                setup_detail = "COLUMNAR_PROJECT column is unbound";
                return false;
              }
              const auto column = std::ranges::find_if(
                  persisted.columns, [&](const auto& candidate) {
                    return candidate.column_uuid ==
                           *expression->bound_name_uuid;
                  });
              if (column == persisted.columns.end()) {
                setup_diagnostic = "SB_MODEL_COLUMNAR_PROJECTION_INVALID_V1";
                setup_detail = "COLUMNAR_PROJECT column is absent";
                return false;
              }
              columnar_request.projected_columns.push_back(
                  static_cast<std::size_t>(
                      std::distance(persisted.columns.begin(), column)));
            }
            return true;
          };
          const auto validate_pre_api_memory = [&]() {
            const auto pre_api_batch_memory =
                Rcp079DescriptorBatchLogicalMemoryBytesV1(logical_rows);
            const auto pre_api_request_memory =
                Rcp079ColumnarRequestNonBatchLogicalMemoryBytesV2(
                    columnar_request);
            pre_api_peak_memory = 0;
            if (!pre_api_batch_memory.has_value() ||
                !pre_api_request_memory.has_value() ||
                !CheckedAdd(*retained_visible_row_memory,
                            *pre_api_batch_memory, &pre_api_peak_memory) ||
                !CheckedAdd(pre_api_peak_memory, *pre_api_request_memory,
                            &pre_api_peak_memory) ||
                !CheckedAdd(pre_api_peak_memory,
                            row_binding_peak_structural_bytes,
                            &pre_api_peak_memory) ||
                !Rcp079AdmitContextualMemoryPeakV2(
                    pre_api_peak_memory, contextual_pre_api_peak_memory,
                    source_input.maximum_memory_bytes,
                    &pre_api_peak_memory)) {
              setup_diagnostic = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
              setup_detail =
                  "columnar row binding exceeded the source memory grant";
              return false;
            }
            return true;
          };
          if (has_filter) {
            const auto filter = std::ranges::find_if(
                dag.expressions, [](const auto& expression) {
                  return expression.operator_name == "COLUMNAR_FILTER";
                });
            if (filter == dag.expressions.end() ||
                filter->child_expression_ids.size() != 2) {
              return fail("SB_MODEL_COLUMNAR_FILTER_INVALID_V1",
                          "COLUMNAR_FILTER root is invalid");
            }
            CanonicalRelationalExpressionRowBinding row_binding;
            std::string detail;
            if (!PrepareInputRowBindingForComposition(
                    dag, filter->child_expression_ids[1],
                    logical_descriptor_ids, &row_binding, &detail,
                    &row_binding_peak_structural_bytes)) {
              return fail("SB_MODEL_COLUMNAR_FILTER_INVALID_V1", detail);
            }
            CanonicalRelationalExpressionRuntimeServices filter_services;
            filter_services.comparison_evaluator =
                [context](const api::EngineTypedValue& left,
                          const api::EngineTypedValue& right,
                          int* comparison, std::string* diagnostic_id,
                          std::string* refusal_detail) {
                  return CompareCanonicalRelationalScalarsV1(
                      context, left, right, comparison, diagnostic_id,
                      refusal_detail);
                };
            BindCanonicalPersistedRowDescriptorAuthorityForSpatialColumnarV1(
                context, &filter_services);
            std::vector<ContextualTextDirectRouteTargetV2>
                contextual_targets;
            std::vector<ContextualTextDirectRouteEqualityV2>
                contextual_equalities;
            if (contextual_text_activation) {
              if (!PrepareContextualTextDirectRouteAuthorityV2(
                      contextual_text_activation, dag, source_node_id,
                      row_binding, logical_descriptor_ids, persisted,
                      logical_rows,
                      filter_services.persisted_row_descriptor_authority,
                      &contextual_targets, &contextual_equalities,
                      &setup_diagnostic, &detail)) {
                return fail(std::move(setup_diagnostic), std::move(detail));
              }
              filter_services
                  .prevalidated_persisted_row_descriptor_authority =
                  [&contextual_targets](
                      const std::uint32_t bound_descriptor_id,
                      const api::RelationalTypeDescriptor& bound,
                      const api::EngineDescriptor& persisted_descriptor,
                      const api::RelationalNullability effective_nullability) {
                    return MatchPrevalidatedContextualTextTargetV2(
                        contextual_targets, bound_descriptor_id, bound,
                        persisted_descriptor, effective_nullability);
                  };
              filter_services.contextual_text_equality_type_authority =
                  [&contextual_equalities](
                      const std::uint32_t comparison_expression_id,
                      const std::uint32_t left_expression_id,
                      const std::uint32_t right_expression_id,
                      const std::uint32_t literal_expression_id,
                      const std::uint64_t literal_occurrence,
                      const std::uint64_t node_id,
                      const std::uint32_t literal_descriptor_handle,
                      std::string* refusal_detail) {
                    return AuthorizePrevalidatedContextualTextEqualityTypeV2(
                        contextual_equalities, comparison_expression_id,
                        left_expression_id, right_expression_id,
                        literal_expression_id, literal_occurrence, node_id,
                        literal_descriptor_handle, refusal_detail);
                  };
              filter_services.contextual_text_equality_evaluator =
                  [contextual_text_activation, &contextual_equalities](
                      const std::uint32_t comparison_expression_id,
                      const std::uint32_t left_expression_id,
                      const std::uint32_t right_expression_id,
                      const api::EngineTypedValue& target_value,
                      api::EngineSqlTruthValue* truth,
                      std::string* diagnostic_id,
                      std::string* refusal_detail) {
                    return EvaluateContextualTextEqualityV2(
                        contextual_text_activation, contextual_equalities,
                        comparison_expression_id, left_expression_id,
                        right_expression_id, target_value, truth,
                        diagnostic_id, refusal_detail);
                  };
            }
            CanonicalRelationalExpressionRuntime runtime(
                dag, std::move(filter_services));
            if (contextual_text_activation) {
              for (const auto& row : logical_rows.rows) {
                std::string inferred_type;
                if (!runtime.InferTypeForConsumer(
                        filter->child_expression_ids[1], row_binding,
                        row.values,
                        api::EngineCanonicalExpressionConsumer::filter,
                        &inferred_type, &detail)) {
                  return fail("SB_MODEL_COLUMNAR_FILTER_INVALID_V1", detail);
                }
              }
              const auto runtime_retained_memory =
                  runtime.RetainedLogicalMemoryBytesV1();
              const auto contextual_memory =
                  Rcp079ContextualExecutionLogicalMemoryPlan(
                      contextual_text_activation, contextual_targets,
                      contextual_equalities);
              if (!runtime_retained_memory.has_value() ||
                  !contextual_memory.has_value() ||
                  !CheckedAdd(row_binding_peak_structural_bytes,
                              *runtime_retained_memory,
                              &row_binding_peak_structural_bytes) ||
                  !Rcp079ReserveContextualPostConsumeGrantV2(
                      source_input.maximum_memory_bytes,
                      *retained_visible_row_memory,
                      contextual_memory->post_api_retained_bytes,
                      &expected_columnar_api_memory_grant)) {
                return fail(
                    "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "contextual TEXT direct-route memory receipt overflowed");
              }
              contextual_pre_api_peak_memory =
                  contextual_memory->pre_barrier_peak_bytes;
              contextual_post_consume_retained_memory =
                  contextual_memory->post_api_retained_bytes;
              columnar_request.maximum_memory_bytes =
                  expected_columnar_api_memory_grant;
              try {
                columnar_request.filter_truth_values.resize(
                    logical_rows.rows.size(),
                    api::EngineSqlTruthValue::unknown);
              } catch (const std::bad_alloc&) {
                return fail(
                    "ENGINE.RESOURCE.EXHAUSTED",
                    "contextual TEXT filter truth allocation was refused");
              } catch (const std::length_error&) {
                return fail(
                    "ENGINE.RESOURCE.EXHAUSTED",
                    "contextual TEXT filter truth extent was refused");
              }
              if (!prepare_projection() || !validate_pre_api_memory()) {
                return fail(std::move(setup_diagnostic),
                            std::move(setup_detail));
              }
              columnar_request.logical_rows = std::move(logical_rows);
              if (cancellation_requested()) {
                return fail(cancellation_diagnostic(),
                            "contextual TEXT activation was cancelled");
              }
              std::string activation_diagnostic;
              if (!ActivateContextualTextDirectRouteV2(
                      contextual_text_activation, &activation_diagnostic,
                      &detail)) {
                return fail(std::move(activation_diagnostic),
                            std::move(detail));
              }
            }
            const auto& rows = contextual_text_activation
                                   ? columnar_request.logical_rows.rows
                                   : logical_rows.rows;
            for (std::size_t row_ordinal = 0; row_ordinal < rows.size();
                 ++row_ordinal) {
              const auto& row = rows[row_ordinal];
              if (cancellation_requested()) {
                return fail(cancellation_diagnostic(),
                            "columnar filter evaluation was cancelled");
              }
              api::EngineSqlTruthValue truth =
                  api::EngineSqlTruthValue::unknown;
              if (!runtime.EvaluatePredicateForConsumer(
                      filter->child_expression_ids[1], row_binding, row.values,
                      api::EngineCanonicalExpressionConsumer::filter, &truth,
                      &detail)) {
                return fail("SB_MODEL_COLUMNAR_FILTER_INVALID_V1", detail);
              }
              if (contextual_text_activation) {
                columnar_request.filter_truth_values[row_ordinal] = truth;
              } else {
                columnar_request.filter_truth_values.push_back(truth);
              }
            }
          }
          if (!contextual_text_activation) {
            if (!validate_pre_api_memory()) {
              return fail(std::move(setup_diagnostic),
                          std::move(setup_detail));
            }
            columnar_request.logical_rows = std::move(logical_rows);
            if (!prepare_projection()) {
              return fail(std::move(setup_diagnostic),
                          std::move(setup_detail));
            }
          }
          if (cancellation_requested()) {
            return fail(cancellation_diagnostic(),
                        "columnar execution was cancelled before reconstruction");
          }
          auto reconstructed =
              api::nosql::ExecuteColumnarLogicalV2(
                  std::move(columnar_request));
          if (model_cancellation_probe_failed->load(
                  std::memory_order_relaxed)) {
            return fail("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                        "columnar cancellation probe failed");
          }
          if (!reconstructed.accepted || !reconstructed.root_publishable ||
              reconstructed.batch.columns.size() != public_columns.size()) {
            return fail(reconstructed.diagnostic_id.empty()
                            ? "SB_MODEL_TYPED_EXCHANGE_INVALID_V1"
                            : reconstructed.diagnostic_id,
                        reconstructed.detail.empty()
                            ? "columnar reconstruction did not publish"
                            : reconstructed.detail);
          }
          if (!reconstructed.exact_fallback_selected ||
              !reconstructed.exact_reconstruction_complete ||
              !reconstructed.predicate_recheck_complete ||
              !reconstructed.mga_recheck_complete ||
              reconstructed.cancellation_observed ||
              reconstructed.cancellation_probe_failed ||
              !reconstructed.memory_receipt_complete ||
              reconstructed.current_live_memory_bytes >
                  reconstructed.peak_live_memory_bytes ||
              reconstructed.peak_live_memory_bytes >
                  reconstructed.memory_grant_bytes ||
              reconstructed.memory_grant_bytes !=
                  expected_columnar_api_memory_grant ||
              reconstructed.physical_operator_id !=
                  "COLUMNAR_ROW_RECONSTRUCTION_SCAN_V1" ||
              !reconstructed.fallback_reason_id.empty() ||
              reconstructed.diagnostic_id != "SB_EXECUTOR_OK" ||
              !reconstructed.detail.empty() ||
              !std::ranges::equal(
                  reconstructed.batch.columns, public_columns,
                  [](const auto& actual, const auto& expected) {
                    return Rcp079ExactExecutorColumnV1(actual, expected);
                  })) {
            return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                        "columnar result descriptor carrier changed");
          }
          std::uint64_t api_peak_with_retained_rows = 0;
          if (!CheckedAdd(*retained_visible_row_memory,
                          contextual_post_consume_retained_memory,
                          &api_peak_with_retained_rows) ||
              !CheckedAdd(api_peak_with_retained_rows,
                          reconstructed.peak_live_memory_bytes,
                          &api_peak_with_retained_rows) ||
              api_peak_with_retained_rows >
                  source_input.maximum_memory_bytes) {
            return fail(
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                "columnar reconstruction peak exceeded the source memory grant");
          }
          const bool source_only =
              source_input.operation_ids.size() == 1 &&
              source_input.operation_ids.front() == "COLUMNAR_SOURCE" &&
              source_input.operation_id == "COLUMNAR_SOURCE";
          if (source_only &&
              (reconstructed.row_uuids.size() !=
                   read.visible_rows.size() ||
               reconstructed.batch.rows.size() !=
                   read.visible_rows.size())) {
            return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                        "columnar source result cardinality changed");
          }
          for (const auto& row : reconstructed.batch.rows) {
            if (cancellation_requested()) {
              return fail(cancellation_diagnostic(),
                          "columnar result validation was cancelled");
            }
            if (row.values.size() != public_columns.size()) {
              return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                          "columnar result row width changed");
            }
            for (std::size_t ordinal = 0; ordinal < row.values.size();
                 ++ordinal) {
              const auto& value = row.values[ordinal];
              const exec::ExecutorColumnDescriptor value_column{
                  public_columns[ordinal].stable_name, value.descriptor,
                  public_columns[ordinal].nullable,
                  public_columns[ordinal].descriptor_id};
              const bool exact_state =
                  (value.state == api::EngineValueState::value &&
                   !value.is_null) ||
                  (value.state == api::EngineValueState::sql_null &&
                   value.is_null && value.encoded_value.empty() &&
                   value.binary_value.empty() &&
                   public_columns[ordinal].nullable);
              if (!exact_state ||
                  !Rcp079ExactExecutorColumnV1(
                      value_column, public_columns[ordinal])) {
                return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "columnar result value descriptor changed");
              }
            }
          }
          if (source_only) {
            for (std::size_t row = 0; row < read.visible_rows.size(); ++row) {
              const auto& source_row = read.visible_rows[row];
              const auto& actual_row = reconstructed.batch.rows[row];
              if (reconstructed.row_uuids[row] != source_row.row_uuid ||
                  actual_row.values.size() != persisted.columns.size()) {
                return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "columnar source row identity or width changed");
              }
              for (std::size_t ordinal = 0;
                   ordinal < persisted.columns.size(); ++ordinal) {
                const auto* expected_encoded = value_for(
                    source_row,
                    persisted.columns[ordinal].canonical_name_key);
                if (expected_encoded == nullptr) {
                  return fail(
                      "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "columnar source row omitted a field during receipt replay");
                }
                const auto& actual = actual_row.values[ordinal];
                const bool expected_null = *expected_encoded == "<NULL>";
                if (!CanonicalQueryEngineDescriptorExactlyEqual(
                        actual.descriptor,
                        public_columns[ordinal].descriptor) ||
                    (expected_null
                         ? (actual.state !=
                                api::EngineValueState::sql_null ||
                            !actual.is_null ||
                            !actual.encoded_value.empty() ||
                            !actual.binary_value.empty())
                         : (actual.state != api::EngineValueState::value ||
                            actual.is_null ||
                            actual.encoded_value != *expected_encoded ||
                            !actual.binary_value.empty()))) {
                  return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                              "columnar source reconstructed value changed");
                }
              }
            }
          }
          batch.properties.ordering_id = "fixture_order";
          batch.batch = std::move(reconstructed.batch);
          batch.ordered_row_identities.reserve(
              reconstructed.row_uuids.size());
          for (auto& row_uuid : reconstructed.row_uuids) {
            exec::ModelProviderRowIdentityV1 identity;
            identity.row_uuid = std::move(row_uuid);
            batch.ordered_row_identities.push_back(std::move(identity));
          }
          if (columnar_runtime_memory_receipt != nullptr) {
            columnar_runtime_memory_receipt->peak_live_memory_bytes =
                std::max(pre_api_peak_memory,
                         api_peak_with_retained_rows);
          }
        }
        try {
          batch.provider_uuid = source_input.provider_uuid;
          batch.provider_generation = source_input.provider_generation;
          batch.selected_alternative_uuid =
              source_input.selected_alternative_uuid;
          batch.capability_uuid = source_input.capability_uuid;
          batch.exact_fallback_selected = true;
          batch.result_handle_uuid = source_input.result_handle_uuid;
          batch.causal_counter_id = source_input.causal_counter_id;
          batch.output_descriptor_ids = source_input.output_descriptor_ids;
          batch.mga_statement_context = source_input.mga_statement_context;
          batch.security_receipt_uuid = security_receipt_uuid;
          batch.properties.property_uuid = property_uuid;
          batch.properties.partitioning_id = "single_local_partition";
          batch.properties.uniqueness_id = "row_uuid";
          batch.properties.exact = true;
          batch.properties.residual_recheck_complete = true;
          batch.properties.base_row_mga_recheck_complete = true;
          batch.properties.security_recheck_complete = true;
          batch.residual_recheck_complete = true;
          batch.base_row_mga_recheck_complete = true;
          batch.security_recheck_complete = true;
        } catch (const std::bad_alloc&) {
          return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "model provider metadata allocation was refused");
        } catch (const std::length_error&) {
          return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                      "model provider metadata length was refused");
        }
        if (columnar_runtime_memory_receipt != nullptr) {
          const auto provider_memory =
              Rcp079ModelProviderBatchLogicalMemoryBytesV1(batch);
          const auto output_memory =
              Rcp079ProjectedModelSourceOutputLogicalMemoryBytesV1(
                  source_input, batch);
          const auto uniqueness_memory =
              Rcp079ColumnarUniquenessLogicalMemoryBytesV1(
                  batch.ordered_row_identities);
          std::uint64_t output_copy_peak = 0;
          std::uint64_t uniqueness_peak = 0;
          if (!provider_memory.has_value() || !output_memory.has_value() ||
              !uniqueness_memory.has_value() ||
              !CheckedAdd(*provider_memory, *output_memory,
                          &output_copy_peak) ||
              !CheckedAdd(output_copy_peak,
                          contextual_post_consume_retained_memory,
                          &output_copy_peak) ||
              !CheckedAdd(*provider_memory, *uniqueness_memory,
                          &uniqueness_peak) ||
              !CheckedAdd(uniqueness_peak,
                          contextual_post_consume_retained_memory,
                          &uniqueness_peak)) {
            return fail(
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                "columnar provider/exchange memory receipt overflowed");
          }
          columnar_runtime_memory_receipt->provider_logical_memory_bytes =
              *provider_memory;
          columnar_runtime_memory_receipt->peak_live_memory_bytes =
              std::max({columnar_runtime_memory_receipt
                            ->peak_live_memory_bytes,
                        output_copy_peak, uniqueness_peak});
          columnar_runtime_memory_receipt->memory_grant_bytes =
              source_input.maximum_memory_bytes;
          columnar_runtime_memory_receipt->complete =
              columnar_runtime_memory_receipt->peak_live_memory_bytes <=
              columnar_runtime_memory_receipt->memory_grant_bytes;
          if (!columnar_runtime_memory_receipt->complete) {
            return fail(
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                "columnar provider/exchange peak exceeded the source memory grant");
          }
        }
        provider.ok = true;
        return provider;
      };

  CaptureRcp079ModelLegV1(
      leg_capture, source->node_id, family, implementation_id,
      spatial ? "canonical.spatial.exact-scan.v1"
              : "canonical.columnar.reconstruction.v1",
      family + ".local.v1", persisted.descriptor_uuid,
      persisted.descriptor_generation,
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource,
      exec::PhysicalNodeKind::kScan, execution_request);
  if (leg_capture != nullptr) {
    leg_capture->exact_output_columns = public_columns;
    leg_capture->columnar_runtime_memory_receipt =
        columnar_runtime_memory_receipt;
    leg_capture->cancellation_probe_failed =
        model_cancellation_probe_failed;
  }
  if (leg_capture != nullptr) return result;
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kScan;
  registration.implementation_id = implementation_id;
  registration.executor_capability_uuid = capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 =
      columnar_runtime_memory_receipt != nullptr;
  registration.execute =
      [execution_request, persisted_descriptor_uuid =
                              persisted.descriptor_uuid,
       implementation_id, columnar_runtime_memory_receipt,
       model_cancellation_probe_failed](
          const exec::TypedPhysicalNodeDag& selected_dag,
          const exec::PhysicalNodeRecord& selected_node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = selected_dag.selected_plan_uuid;
        step.executed_physical_node_id = selected_node.physical_node_id;
        step.causal_counter_id = selected_node.causal_counter_id;
        step.output_descriptor_ids = selected_node.output_descriptor_ids;
        step.mga_statement_context = selected_dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        if (!inputs.empty() || selected_dag.abi_version != 2 ||
            selected_node.memory_bytes_required == 0 ||
            selected_node.memory_bytes_required >
                selected_dag.memory_budget_bytes ||
            selected_node.memory_bytes_required !=
                execution_request.input.maximum_memory_bytes ||
            selected_node.implementation_id != implementation_id ||
            selected_node.selected_alternative_uuid !=
                execution_request.input.selected_alternative_uuid ||
            selected_node.executor_capability_uuid !=
                execution_request.input.capability_uuid ||
            selected_node.physical_node_id !=
                execution_request.input.physical_node_id ||
            selected_node.causal_counter_id !=
                execution_request.input.causal_counter_id ||
            selected_node.output_descriptor_ids !=
                execution_request.input.output_descriptor_ids ||
            !exec::PhysicalMgaStatementContextEqual(
                selected_dag.mga_statement_context,
                execution_request.input.mga_statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
          step.diagnostic.detail =
              "selected spatial/columnar physical node was substituted";
          return step;
        }
        const auto executed = exec::ExecuteModelFamilySourceV1(execution_request);
        step.data_access_observed = executed.data_access_observed;
        if (model_cancellation_probe_failed->load(
                std::memory_order_relaxed)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          step.diagnostic.detail =
              "model-family cancellation probe failed";
          return step;
        }
        if (!executed.accepted || !executed.root_published ||
            !executed.cleanup_complete || executed.cleanup_count != 1 ||
            !executed.output.exact_exchange_validated ||
            executed.output.family_id != execution_request.input.family_id ||
            executed.output.operation_ids !=
                execution_request.input.operation_ids ||
            executed.output.operation_id !=
                execution_request.input.operation_id ||
            executed.output.output_descriptor_ids !=
                selected_node.output_descriptor_ids ||
            !executed.output.exact_fallback_selected) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              executed.diagnostic_id.empty()
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : executed.diagnostic_id;
          step.diagnostic.detail =
              executed.detail.empty()
                  ? "spatial/columnar source execution did not complete"
                  : executed.detail;
          return step;
        }
        if (columnar_runtime_memory_receipt != nullptr) {
          std::uint64_t current_memory_bytes = 0;
          if (!columnar_runtime_memory_receipt->complete ||
              columnar_runtime_memory_receipt
                      ->provider_logical_memory_bytes == 0 ||
              columnar_runtime_memory_receipt->memory_grant_bytes !=
                  selected_node.memory_bytes_required ||
              columnar_runtime_memory_receipt->peak_live_memory_bytes >
                  columnar_runtime_memory_receipt->memory_grant_bytes ||
              !RuntimeMaterializedBatchMemoryBytes(executed.output.batch,
                                                   &current_memory_bytes) ||
              current_memory_bytes >
                  columnar_runtime_memory_receipt->peak_live_memory_bytes) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
            step.diagnostic.detail =
                "columnar runtime memory receipt is incomplete";
            return step;
          }
          PublishRuntimeMemoryObservation(
              &step, current_memory_bytes,
              columnar_runtime_memory_receipt->peak_live_memory_bytes);
        }
        step.result_handle_id = selected_node.physical_node_id;
        step.output_row_count = executed.output.batch.rows.size();
        step.rows_examined = executed.rows_examined;
        step.current_relation_descriptor_uuid = persisted_descriptor_uuid;
        step.current_relation_descriptor_generation =
            execution_request.input.descriptor_generation;
        step.materialized_output_batch = std::move(executed.output.batch);
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest selected;
  selected.selected_physical_dag = physical.physical_dag;
  selected.pre_access_statistics_snapshot_uuid =
      physical.physical_dag.statistics_snapshot_uuid;
  selected.mga_authority =
      BuildCanonicalExecutionMgaAuthority(input.context, physical.physical_dag);
  selected.runtime_limits.maximum_rows_per_batch = source_input.maximum_rows;
  selected.runtime_limits.maximum_columns_per_batch = expected_width;
  selected.runtime_limits.maximum_cells_per_batch = source_input.maximum_cells;
  selected.runtime_limits.maximum_total_materialized_rows =
      source_input.maximum_rows;
  selected.runtime_limits.maximum_total_materialized_cells =
      source_input.maximum_cells;
  // Contextual admission owns cancellation precedence inside this exact
  // provider and polls the real receipt-bound probe at provider entry and at
  // its final pre-barrier point.  Suppress only the generic physical-node
  // preemption that would otherwise publish the unrelated QRY-004 diagnostic.
  selected.cancellation_requested = input.contextual_text_activation
      ? std::function<bool()>([] { return false; })
      : cancellation_requested;
  selected.available_executors.push_back(std::move(registration));
  selected.engine_execution_authorized = true;
  selected.result_publication_request.statement_uuid =
      input.context.statement_uuid;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  selected.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(identity_scope + ":" +
                               input.context.current_monotonic_ns,
                           family + ".execution-attempt");
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id) + ":" +
              std::to_string(
                  input.context.snapshot_visible_through_local_transaction_id),
          family + ".transaction-effect-unchanged");
  selected.result_publication_request.maximum_row_count =
      source_input.maximum_rows;
  selected.result_publication_request.column_bindings = result_bindings;
  const auto execution = ExecuteSelectedCanonicalObjectFreeDag(
      input.context, selected, physical.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.data_access_observed ||
      !execution.runtime_actuals.accepted ||
      execution.dispatch.executed_steps.size() != 1 ||
      !execution.issues.empty()) {
    return refuse(execution.issues.empty()
                      ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                      : execution.issues.front().diagnostic_id,
                  execution.issues.empty()
                      ? "spatial/columnar selected execution did not complete"
                      : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = true;
  result.canonical_result_published = true;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(canonical_planning_request, execution);
  result.api_result.evidence.push_back(
      {"canonical.model_route",
       spatial
           ? "SBSQL_SPATIAL_SOURCE_TO_MGA_POINT_RECHECK_TO_TYPED_BATCH_V1"
           : "SBSQL_COLUMNAR_SOURCE_TO_MGA_RECONSTRUCTION_TO_TYPED_BATCH_V1"});
  result.api_result.evidence.push_back(
      {"canonical.model_operation_count", std::to_string(operations.size())});
  for (const auto& operation : operations) {
    result.api_result.evidence.push_back(
        {"canonical.model_operation", operation});
  }
  return result;
}


std::uint32_t CanonicalSpatialColumnarContextualInternalProofMaskForTest() {
  std::uint32_t mask = 0;
  auto activation = std::make_shared<ContextualTextDispatchActivationV2>();
  if (std::string_view(Rcp079ContextualCancellationDiagnosticV2(true, false)) ==
          "PROCESS.CANCELLED" &&
      !activation->joint_consumed) {
    mask |= 1U << 1;
  }

  ContextualTextDirectRouteEqualityV2 equality;
  equality.comparison_expression_id = 2;
  equality.literal_expression_id = 3;
  equality.target_expression_id = 4;
  equality.literal_occurrence = 5;
  equality.node_id = 6;
  equality.literal_descriptor_handle = 7;
  equality.literal_argument_ordinal = 1;
  equality.target_argument_ordinal = 2;
  const std::vector<ContextualTextDirectRouteEqualityV2> equalities{
      equality};
  bool ambiguous = false;
  const auto* exact = FindPrevalidatedContextualTextEqualityV2(
      equalities, 2, 3, 4, &ambiguous);
  if (exact == &equalities.front() && !ambiguous) {
    mask |= 1U << 2;
  }
  const auto* crossed = FindPrevalidatedContextualTextEqualityV2(
      equalities, 2, 4, 3, &ambiguous);
  if (crossed == nullptr && !ambiguous) {
    mask |= 1U << 3;
  }
  std::uint64_t peak = 0;
  if (Rcp079AdmitContextualMemoryPeakV2(10, 5, 15, &peak) && peak == 15) {
    mask |= 1U << 4;
  }
  if (!Rcp079AdmitContextualMemoryPeakV2(10, 5, 14, &peak)) {
    mask |= 1U << 5;
  }
  std::uint64_t api_grant = 0;
  if (Rcp079ReserveContextualPostConsumeGrantV2(100, 20, 30,
                                                &api_grant) &&
      api_grant == 50) {
    mask |= 1U << 6;
  }
  if (!Rcp079ReserveContextualPostConsumeGrantV2(100, 20, 80,
                                                 &api_grant)) {
    mask |= 1U << 7;
  }

  const auto route_operation = [](const std::uint32_t expression_id,
                                  const std::string& name) {
    api::RelationalExpressionRecord expression;
    expression.expression_id = expression_id;
    expression.expression_kind =
        api::RelationalExpressionKind::kFunctionCall;
    expression.operator_name = name;
    return expression;
  };
  const auto bound_expression = [](const std::uint32_t expression_id) {
    api::RelationalExpressionRecord expression;
    expression.expression_id = expression_id;
    return expression;
  };
  api::TypedRelationalDag exact_route;
  exact_route.wire_version = 2;
  exact_route.root_node_id = 1;
  api::RelationalDagNode source;
  source.node_id = 1;
  source.node_kind = api::RelationalDagNodeKind::kScan;
  source.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  source.bound_expression_ids = {8, 9, 10, 2, 7, 3, 4, 5, 6};
  exact_route.nodes.push_back(source);
  exact_route.expressions = {
      bound_expression(8), bound_expression(9), bound_expression(10),
      route_operation(2, "COLUMNAR_SOURCE"),
      route_operation(7, "COLUMNAR_FILTER"), bound_expression(3),
      bound_expression(4), bound_expression(5), bound_expression(6)};
  if (Rcp079ExactContextualTextDirectRouteCandidateV2(exact_route)) {
    mask |= 1U << 8;
  }
  auto projected_route = exact_route;
  projected_route.nodes.front().bound_expression_ids.push_back(11);
  projected_route.expressions.push_back(
      route_operation(11, "COLUMNAR_PROJECT"));
  if (Rcp079ExactContextualTextDirectRouteCandidateV2(projected_route)) {
    mask |= 1U << 9;
  }
  auto unbound_route_operation = exact_route;
  unbound_route_operation.expressions.push_back(
      route_operation(12, "COLUMNAR_FILTER"));
  if (!Rcp079ExactContextualTextDirectRouteCandidateV2(
          unbound_route_operation)) {
    mask |= 1U << 10;
  }
  auto extra_node_route = exact_route;
  auto extra_node = source;
  extra_node.node_id = 2;
  extra_node_route.nodes.push_back(std::move(extra_node));
  if (!Rcp079ExactContextualTextDirectRouteCandidateV2(extra_node_route)) {
    mask |= 1U << 11;
  }
  auto reordered_route = projected_route;
  const auto source_root = std::ranges::find(
      reordered_route.nodes.front().bound_expression_ids, 2U);
  const auto filter_root = std::ranges::find(
      reordered_route.nodes.front().bound_expression_ids, 7U);
  if (source_root !=
          reordered_route.nodes.front().bound_expression_ids.end() &&
      filter_root != reordered_route.nodes.front().bound_expression_ids.end()) {
    std::iter_swap(source_root, filter_root);
  }
  if (!Rcp079ExactContextualTextDirectRouteCandidateV2(reordered_route)) {
    mask |= 1U << 12;
  }
  auto duplicate_binding = exact_route;
  duplicate_binding.nodes.front().bound_expression_ids.push_back(4);
  if (!Rcp079ExactContextualTextDirectRouteCandidateV2(duplicate_binding)) {
    mask |= 1U << 13;
  }
  std::string type_detail;
  if (AuthorizePrevalidatedContextualTextEqualityTypeV2(
          equalities, 2, 3, 4, 3, 5, 6, 7, &type_detail) &&
      type_detail.empty()) {
    mask |= 1U << 14;
  }
  if (!AuthorizePrevalidatedContextualTextEqualityTypeV2(
          equalities, 2, 3, 4, 3, 5, 8, 7, &type_detail) &&
      !type_detail.empty()) {
    mask |= 1U << 15;
  }
  return mask;
}


}  // namespace scratchbird::engine::sblr
