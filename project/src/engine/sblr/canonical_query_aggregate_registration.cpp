// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_aggregate_registration.hpp"

#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"

#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_operations.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_AGGREGATE_REGISTRATION_AUTHORITY
std::string ExactCanonicalCoreDatatypeUuidV1(
    const std::string_view stable_name) {
  static const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto count = std::ranges::count_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  return count == 1 && found != manifest.manifest.descriptor_rows.end() &&
                 found->descriptor_uuid.valid()
             ? scratchbird::core::uuid::UuidToString(
                   found->descriptor_uuid.value)
             : std::string{};
}

std::string ExactCanonicalInt64TypeUuidV1() {
  return ExactCanonicalCoreDatatypeTypeUuidV1("int64");
}

std::string ExactCanonicalCoreDatatypeTypeUuidV1(
    const std::string_view stable_name) {
  static const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto count = std::ranges::count_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  if (count != 1 || found == manifest.manifest.descriptor_rows.end() ||
      !found->descriptor_uuid.valid()) {
    return {};
  }
  const auto descriptor_uuid = scratchbird::core::uuid::UuidToString(
      found->descriptor_uuid.value);
  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      "019d0000-0000-7000-8000-00000000d701",
      manifest.manifest.catalog_epoch, 1, descriptor_uuid,
      found->descriptor_epoch);
  // Core rows that have a registered codec identity carry a distinct type
  // UUID (notably int64).  Older core rows still use their catalog descriptor
  // UUID as the exact type identity until their codec row is registered.
  return identity.ok ? identity.row.type_uuid : descriptor_uuid;
}

bool RevalidatePreparedAggregateValueBindings(
    const std::vector<std::size_t>& value_columns,
    const std::vector<std::uint32_t>& value_descriptor_ids,
    const std::vector<PreparedAggregateValueBindingReceipt>& receipts,
    const exec::DescriptorBatch& input_batch,
    std::string* detail) {
  if (detail == nullptr) return false;
  detail->clear();
  if (receipts.empty()) return true;
  if (receipts.size() != value_columns.size() ||
      receipts.size() != value_descriptor_ids.size()) {
    *detail = "aggregate exact value-binding receipt arity drifted";
    return false;
  }
  for (std::size_t index = 0; index < receipts.size(); ++index) {
    const auto& receipt = receipts[index];
    if (receipt.value_column != value_columns[index] ||
        receipt.descriptor_id != value_descriptor_ids[index] ||
        receipt.value_column >= input_batch.columns.size()) {
      *detail = "aggregate exact value-binding receipt is unresolved";
      return false;
    }
    const auto& column = input_batch.columns[receipt.value_column];
    const auto canonical_type_id = dt::CanonicalTypeIdFromStableName(
        receipt.canonical_type_name);
    const auto canonical_stable_name = [&]() -> std::string_view {
      switch (canonical_type_id) {
        case dt::CanonicalTypeId::int8:
          return "int8";
        case dt::CanonicalTypeId::int16:
          return "int16";
        case dt::CanonicalTypeId::int32:
          return "int32";
        case dt::CanonicalTypeId::int64:
          return "int64";
        default:
          return {};
      }
    }();
    const auto canonical_type_uuid =
        canonical_stable_name.empty()
            ? std::string{}
            : ExactCanonicalCoreDatatypeTypeUuidV1(canonical_stable_name);
    if (canonical_stable_name.empty() || canonical_type_uuid.empty() ||
        receipt.type_uuid != canonical_type_uuid ||
        receipt.descriptor_uuid.empty() || receipt.type_uuid.empty() ||
        receipt.encoded_descriptor.empty() ||
        column.descriptor_id != receipt.descriptor_id ||
        column.nullable != receipt.nullable ||
        column.descriptor.descriptor_uuid.canonical !=
            receipt.descriptor_uuid ||
        column.descriptor.descriptor_kind != "scalar" ||
        column.descriptor.canonical_type_name !=
            receipt.canonical_type_name ||
        column.descriptor.encoded_descriptor !=
            receipt.encoded_descriptor ||
        !exec::IsCanonicalBoundedSignedIntegerDescriptor(
            column.descriptor) ||
        !api::QowCanonicalDescriptorIdentityV1(column.descriptor)) {
      *detail =
          "aggregate exact bounded-signed value binding drifted:" +
          std::string("stable=") +
          (!canonical_stable_name.empty() ? "1" : "0") +
          ":type_uuid=" +
          (receipt.type_uuid == canonical_type_uuid ? "1" : "0") +
          ":receipt_descriptor=" +
          (!receipt.descriptor_uuid.empty() ? "1" : "0") +
          ":receipt_encoded=" +
          (!receipt.encoded_descriptor.empty() ? "1" : "0") +
          ":column_id=" +
          (column.descriptor_id == receipt.descriptor_id ? "1" : "0") +
          ":column_nullability=" +
          (column.nullable == receipt.nullable ? "1" : "0") +
          ":column_uuid=" +
          (column.descriptor.descriptor_uuid.canonical ==
                   receipt.descriptor_uuid
               ? "1"
               : "0") +
          ":column_kind=" +
          (column.descriptor.descriptor_kind == "scalar" ? "1" : "0") +
          ":column_type=" +
          (column.descriptor.canonical_type_name ==
                   receipt.canonical_type_name
               ? "1"
               : "0") +
          ":column_encoded=" +
          (column.descriptor.encoded_descriptor ==
                   receipt.encoded_descriptor
               ? "1"
               : "0") +
          ":bounded_signed=" +
          (exec::IsCanonicalBoundedSignedIntegerDescriptor(column.descriptor)
               ? "1"
               : "0") +
          ":identity=" +
          (api::QowCanonicalDescriptorIdentityV1(column.descriptor) ? "1"
                                                                    : "0");
      return false;
    }
  }
  return true;
}

bool BindPreparedGroupedComparisonCeilings(
    PreparedGroupedCountSumRoot* prepared,
    const std::uint64_t maximum_input_row_count) {
  if (prepared == nullptr || !prepared->ok ||
      prepared->grouping_sets.empty() || prepared->key_terms.empty()) {
    return false;
  }
  std::uint64_t adjacent = maximum_input_row_count;
  if (adjacent != 0) --adjacent;
  std::uint64_t triangular = 0;
  if ((maximum_input_row_count & 1U) == 0) {
    if (!CheckedMultiply(maximum_input_row_count / 2, adjacent,
                         &triangular)) {
      return false;
    }
  } else if (!CheckedMultiply(maximum_input_row_count, adjacent / 2,
                              &triangular)) {
    return false;
  }
  std::uint64_t self_and_admission = 0;
  if (!CheckedMultiply(maximum_input_row_count, 2,
                       &self_and_admission) ||
      !CheckedAdd(triangular, self_and_admission,
                  &self_and_admission)) {
    return false;
  }
  std::uint64_t per_aggregate = 0;
  for (const auto& grouping_set : prepared->grouping_sets) {
    std::uint64_t set_comparisons = 0;
    if (!CheckedMultiply(grouping_set.key_term_ordinals.size(),
                         self_and_admission, &set_comparisons) ||
        !CheckedAdd(per_aggregate, set_comparisons, &per_aggregate)) {
      return false;
    }
  }
  per_aggregate = std::max<std::uint64_t>(1, per_aggregate);
  std::uint64_t combined = 0;
  if (!CheckedMultiply(per_aggregate, 2, &combined) ||
      per_aggregate > std::numeric_limits<std::size_t>::max() ||
      combined > std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  prepared->maximum_grouping_key_comparison_count =
      static_cast<std::size_t>(per_aggregate);
  prepared->maximum_combined_grouping_key_comparison_count =
      static_cast<std::size_t>(combined);
  return true;
}

bool RevalidatePreparedGroupedKeyBindings(
    const PreparedGroupedCountSumRoot& prepared,
    const exec::DescriptorBatch& input_batch,
    std::string* detail) {
  if (detail == nullptr) return false;
  detail->clear();
  if (prepared.key_terms.empty() ||
      prepared.key_binding_receipts.size() != prepared.key_terms.size()) {
    *detail = "grouped aggregate exact key-binding receipt arity drifted";
    return false;
  }
  for (std::size_t index = 0; index < prepared.key_terms.size(); ++index) {
    const auto& term = prepared.key_terms[index];
    const auto& receipt = prepared.key_binding_receipts[index];
    if (receipt.value_column != term.column ||
        receipt.descriptor_id != term.expression_descriptor_id ||
        receipt.value_column >= input_batch.columns.size()) {
      *detail = "grouped aggregate exact key-binding receipt is unresolved";
      return false;
    }
    const auto& column = input_batch.columns[receipt.value_column];
    const auto expected_nullability =
        receipt.nullable ? std::string_view("nullable")
                         : std::string_view("non_null");
    if (receipt.canonical_type_name != "int64" ||
        receipt.descriptor_uuid.empty() || receipt.type_uuid.empty() ||
        receipt.encoded_descriptor !=
            "type_uuid=" + receipt.type_uuid +
                ";nullability=" + std::string(expected_nullability) ||
        column.descriptor_id != receipt.descriptor_id ||
        column.nullable != receipt.nullable ||
        column.descriptor.descriptor_uuid.canonical !=
            receipt.descriptor_uuid ||
        column.descriptor.descriptor_kind != "scalar" ||
        column.descriptor.canonical_type_name !=
            receipt.canonical_type_name ||
        column.descriptor.encoded_descriptor !=
            receipt.encoded_descriptor ||
        !api::QowCanonicalDescriptorIdentityV1(column.descriptor)) {
      *detail = "grouped aggregate exact int64 key binding drifted";
      return false;
    }
  }
  return true;
}

bool AggregateFilterInputDescriptorExact(
    const exec::DescriptorBatch& input,
    const std::size_t filter_column,
    const std::uint32_t filter_descriptor_id,
    std::string* detail) {
  if (detail == nullptr) return false;
  if (filter_column >= input.columns.size() ||
      input.columns[filter_column].descriptor_id != filter_descriptor_id ||
      input.columns[filter_column].descriptor.canonical_type_name !=
          "boolean") {
    *detail =
        "global aggregate FILTER input descriptor is not exact canonical "
        "boolean";
    return false;
  }
  return true;
}

bool DecodeAggregateFilterTruthValue(
    const api::EngineTypedValue& value,
    api::EngineSqlTruthValue* truth,
    std::string* detail) {
  if (truth == nullptr || detail == nullptr) return false;
  api::EngineCanonicalExpressionEvaluationRequest expression_request;
  expression_request.consumer =
      api::EngineCanonicalExpressionConsumer::aggregate;
  expression_request.operation =
      api::EngineCanonicalExpressionOperation::identity;
  expression_request.left_value = value;
  expression_request.result_descriptor = value.descriptor;
  api::EngineCanonicalExpressionEvaluationResult expression_result;
  if (!api::QowEvaluateCanonicalTypedExpressionV1(
          expression_request, &expression_result, detail) ||
      !api::QowCanonicalTruthFromTypedValueV1(
          expression_result.value, truth, detail)) {
    const std::string expression_detail = *detail;
    *detail =
        "global aggregate FILTER input is not exact SQL boolean "
        "three-valued state: " + expression_detail;
    return false;
  }
  return true;
}

bool ValidateAggregateFilterTruthValues(
    const exec::DescriptorBatch& input,
    const std::size_t filter_column,
    const std::uint32_t filter_descriptor_id,
    std::string* detail) noexcept {
  if (detail == nullptr) return false;
  try {
    detail->clear();
    if (!AggregateFilterInputDescriptorExact(
            input, filter_column, filter_descriptor_id, detail)) {
      return false;
    }
    for (const auto& row : input.rows) {
      if (filter_column >= row.values.size()) {
        *detail = "global aggregate FILTER input cardinality is unresolved";
        return false;
      }
      api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::unspecified;
      if (!DecodeAggregateFilterTruthValue(
              row.values[filter_column], &truth, detail)) {
        return false;
      }
    }
  } catch (const std::length_error&) {
    detail->clear();
    return false;
  } catch (const std::bad_alloc&) {
    detail->clear();
    return false;
  }
  return true;
}

bool MaterializeAggregateFilterTruthValues(
    const exec::DescriptorBatch& input,
    const std::size_t filter_column,
    const std::uint32_t filter_descriptor_id,
    const std::uint64_t maximum_retained_bytes,
    std::vector<api::EngineSqlTruthValue>* filter_truth_values,
    std::uint64_t* retained_bytes,
    std::string* detail) noexcept {
  if (filter_truth_values == nullptr || retained_bytes == nullptr ||
      detail == nullptr) {
    return false;
  }
  try {
    detail->clear();
    if (!AggregateFilterInputDescriptorExact(
            input, filter_column, filter_descriptor_id, detail)) {
      return false;
    }
    std::uint64_t logical_bytes = 0;
    if (!CheckedMultiply(input.rows.size(),
                         sizeof(api::EngineSqlTruthValue),
                         &logical_bytes)) {
      *detail = "global aggregate FILTER truth carrier size overflowed";
      return false;
    }
    if (logical_bytes > maximum_retained_bytes) {
      *detail =
          "global aggregate FILTER truth carrier exceeds its admitted memory "
          "bound";
      return false;
    }

    std::vector<api::EngineSqlTruthValue> candidate;
    if (!input.rows.empty()) candidate.reserve(input.rows.size());
    std::uint64_t capacity_bytes = 0;
    if (!CheckedMultiply(candidate.capacity(),
                         sizeof(api::EngineSqlTruthValue),
                         &capacity_bytes) ||
        capacity_bytes > maximum_retained_bytes) {
      *detail =
          "global aggregate FILTER retained truth carrier exceeds its "
          "admitted memory bound";
      return false;
    }
    for (const auto& row : input.rows) {
      if (filter_column >= row.values.size()) {
        *detail = "global aggregate FILTER input cardinality is unresolved";
        return false;
      }
      api::EngineSqlTruthValue truth =
          api::EngineSqlTruthValue::unspecified;
      if (!DecodeAggregateFilterTruthValue(
              row.values[filter_column], &truth, detail)) {
        return false;
      }
      candidate.push_back(truth);
    }
    *filter_truth_values = std::move(candidate);
    *retained_bytes = capacity_bytes;
  } catch (const std::length_error&) {
    detail->clear();
    return false;
  } catch (const std::bad_alloc&) {
    detail->clear();
    return false;
  }
  return true;
}

bool BindTimezoneOrderAuthority(
    const api::EngineRequestContext& context,
    exec::CanonicalDescriptorOrderTerm* term,
    std::string* detail) {
  if (term == nullptr || detail == nullptr) return false;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  (void)context;
  *detail =
      "temporal ordering requires the production engine timezone catalog";
  return false;
#else
  const auto resolved = api::LookupEngineTimezoneSeedAuthority(context);
  if (!resolved.ok || !resolved.authority.active ||
      resolved.authority.resource_epoch == 0 ||
      resolved.authority.timezone_epoch == 0) {
    *detail = resolved.diagnostic.code.empty()
                  ? "temporal ordering lacks current engine timezone authority"
                  : resolved.diagnostic.code;
    return false;
  }
  term->resource_epoch = resolved.authority.resource_epoch;
  term->timezone_epoch = resolved.authority.timezone_epoch;
  term->timezone_seed.active = resolved.authority.active;
  term->timezone_seed.seed_pack_name = resolved.authority.seed_pack_name;
  term->timezone_seed.seed_pack_version =
      resolved.authority.seed_pack_version;
  term->timezone_seed.content_hash = resolved.authority.content_hash;
  term->timezone_seed.timezone_records =
      resolved.authority.timezone_records;
  term->timezone_seed.timezone_transition_records =
      resolved.authority.timezone_transition_records;
  term->timezone_seed.timezone_leap_second_records =
      resolved.authority.timezone_leap_second_records;
  term->timezone_seed.timezone_names = resolved.authority.timezone_names;
  return true;
#endif
}

bool BindCanonicalDescriptorEqualityTerm(
    const api::EngineRequestContext& context,
    const exec::ExecutorColumnDescriptor& column,
    const std::size_t column_ordinal,
    exec::CanonicalDescriptorOrderTerm* term,
    std::string* detail) {
  if (term == nullptr || detail == nullptr) return false;
  *term = {};
  detail->clear();
  term->column = column_ordinal;
  term->expression_descriptor_id = column.descriptor_id;
  term->direction = exec::CanonicalDescriptorOrderDirection::ascending;
  term->null_placement = exec::CanonicalDescriptorNullPlacement::first;
  const auto& type = column.descriptor.canonical_type_name;
  if (type == "text" || type == "varchar" || type == "char" ||
      type == "character") {
    const auto collation_uuid = ExactEncodedDescriptorField(
        column.descriptor.encoded_descriptor, "collation_uuid");
    if (!collation_uuid.has_value()) {
      *detail =
          "aggregate character equality lacks an exact bound collation";
      return false;
    }
    term->collation_uuid = *collation_uuid;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
    constexpr std::string_view kContractCollationUuid =
        "019f0000-0000-7200-8000-00000000c011";
    if (term->collation_uuid != kContractCollationUuid ||
        context.resource_epoch == 0 || context.catalog_generation_id == 0) {
      *term = {};
      *detail =
          "aggregate character equality lacks exact contract collation "
          "authority";
      return false;
    }
    // The query-route contract library cannot link the production resource
    // catalog. Its single named fixture remains authority-bound to the
    // engine-issued request epochs and is compiled out of production builds.
    term->resource_epoch = context.resource_epoch;
    term->collation_epoch = context.catalog_generation_id;
    term->text_seed.active = true;
    term->text_seed.seed_pack_name = "qow.query-route.resources";
    term->text_seed.seed_pack_version = "1";
    term->text_seed.charset_name = "UTF-8";
    term->text_seed.collation_name = "qow_aggregate_binary";
    term->text_seed.collation_case_insensitive = false;
    term->text_seed.collation_accent_insensitive = false;
#else
    api::EngineUuid resource_uuid;
    resource_uuid.canonical = term->collation_uuid;
    const auto resolved = api::LookupEngineResourceDescriptorByUuid(
        context, resource_uuid, "collation");
    if (!resolved.ok || !resolved.resource_descriptor.present ||
        resolved.resource_descriptor.resource_uuid.canonical !=
            term->collation_uuid) {
      *term = {};
      *detail =
          "aggregate character equality lacks current engine collation "
          "authority";
      return false;
    }
    term->resource_epoch = resolved.resource_descriptor.resource_epoch;
    term->collation_epoch = resolved.resource_descriptor.family_epoch;
    term->text_seed.active = true;
    term->text_seed.seed_pack_name =
        resolved.resource_descriptor.seed_pack_name;
    term->text_seed.seed_pack_version =
        resolved.resource_descriptor.seed_pack_version;
    term->text_seed.charset_name =
        resolved.resource_descriptor.parent_canonical_name;
    term->text_seed.collation_name =
        resolved.resource_descriptor.canonical_name;
    term->text_seed.collation_case_insensitive =
        resolved.resource_descriptor.case_insensitive;
    term->text_seed.collation_accent_insensitive =
        resolved.resource_descriptor.accent_insensitive;
#endif
  } else if (type == "time" || type == "timestamp") {
    const bool carries_timezone_profile =
        column.descriptor.encoded_descriptor.find("timezone_profile_id=") !=
        std::string::npos;
    const auto timezone_profile = ExactEncodedDescriptorField(
        column.descriptor.encoded_descriptor, "timezone_profile_id");
    if (carries_timezone_profile && !timezone_profile.has_value()) {
      *detail =
          "aggregate temporal equality carries a malformed timezone profile";
      return false;
    }
    if (timezone_profile.has_value() &&
        !BindTimezoneOrderAuthority(context, term, detail)) {
      *term = {};
      return false;
    }
  }
  const auto validation =
      exec::ValidateCanonicalDescriptorOrderTerm(*term, column);
  if (!validation.ok) {
    *term = {};
    *detail = validation.detail;
    return false;
  }
  return true;
}

bool BindCanonicalAggregateEqualityTerms(
    const api::EngineRequestContext& context,
    const exec::DescriptorBatch& input_batch,
    exec::CanonicalAggregateRuntimeRequest* request,
    std::string* detail) {
  if (request == nullptr || detail == nullptr) return false;
  request->aggregate_equality_terms.clear();
  request->aggregate_equality_authority_profile.clear();
  const auto function = request->descriptor.function;
  const bool frequency_identity_required =
      function == exec::CanonicalAggregateFunction::mode ||
      function == exec::CanonicalAggregateFunction::approx_count_distinct ||
      function == exec::CanonicalAggregateFunction::approx_top_k;
  const bool equality_required =
      request->distinct || frequency_identity_required;
  if (!equality_required) {
    const auto profile =
        exec::BindCanonicalAggregateEqualityAuthorityProfile(request,
                                                             input_batch);
    if (!profile.ok) {
      *detail = profile.detail;
      return false;
    }
    return true;
  }
  request->aggregate_equality_terms.reserve(request->value_columns.size());
  for (std::size_t index = 0; index < request->value_columns.size(); ++index) {
    const auto column = request->value_columns[index];
    if (column >= input_batch.columns.size() ||
        index >= request->value_expression_descriptor_ids.size() ||
        input_batch.columns[column].descriptor_id !=
            request->value_expression_descriptor_ids[index]) {
      request->aggregate_equality_terms.clear();
      *detail =
          "aggregate equality expression is not descriptor-exact";
      return false;
    }
    exec::CanonicalDescriptorOrderTerm term;
    if (!BindCanonicalDescriptorEqualityTerm(
            context, input_batch.columns[column], column, &term, detail)) {
      request->aggregate_equality_terms.clear();
      return false;
    }
    request->aggregate_equality_terms.push_back(std::move(term));
  }
  const std::size_t frequency_phase_count =
      frequency_identity_required
          ? (request->forced_strategy ==
                     exec::CanonicalAggregateExecutionStrategy::
                         partitioned_combine
                 ? 2U
                 : 1U)
          : 0U;
  const std::size_t identity_phase_count =
      (request->distinct ? 1U : 0U) + frequency_phase_count;
  const auto row_count = input_batch.rows.size();
  std::uint64_t generations = row_count;
  std::uint64_t comparisons = row_count;
  if (request->value_columns.size() != 0 &&
      generations <= std::numeric_limits<std::uint64_t>::max() /
                         request->value_columns.size()) {
    generations *= request->value_columns.size();
  } else if (request->value_columns.size() != 0) {
    *detail = "aggregate equality-key generation bound overflowed";
    return false;
  }
  if (generations > std::numeric_limits<std::uint64_t>::max() /
                        identity_phase_count) {
    *detail = "aggregate equality-key generation bound overflowed";
    return false;
  }
  generations *= identity_phase_count;
  if (row_count > 1) {
    if (row_count > std::numeric_limits<std::uint64_t>::max() /
                        (row_count - 1)) {
      *detail = "aggregate equality comparison bound overflowed";
      return false;
    }
    comparisons = row_count * (row_count - 1) / 2;
  } else {
    comparisons = 0;
  }
  if (comparisons > std::numeric_limits<std::uint64_t>::max() /
                        identity_phase_count) {
    *detail = "aggregate equality comparison bound overflowed";
    return false;
  }
  comparisons *= identity_phase_count;
  if (generations > std::numeric_limits<std::size_t>::max() ||
      comparisons > std::numeric_limits<std::size_t>::max()) {
    *detail = "aggregate equality work exceeds the platform size domain";
    return false;
  }
  request->maximum_equality_key_generation_count =
      static_cast<std::size_t>(generations);
  request->maximum_equality_comparison_count =
      static_cast<std::size_t>(comparisons);
  const auto profile =
      exec::BindCanonicalAggregateEqualityAuthorityProfile(request,
                                                           input_batch);
  if (!profile.ok) {
    request->aggregate_equality_terms.clear();
    request->aggregate_equality_authority_profile.clear();
    *detail = profile.detail.empty() ? profile.diagnostic_code
                                     : profile.detail;
    return false;
  }
  return true;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveAggregateRegistryRegistration(
    PreparedGlobalAggregateRoot prepared,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::uint64_t maximum_filter_truth_memory_bytes,
    api::EngineRequestContext mga_context,
    const bool strict_dispatcher_memory) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  registration.implementation_id = "aggregate.registry-core.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 = true;
  registration.honors_dispatcher_memory_limit_v1 =
      strict_dispatcher_memory;
  registration.execute =
      [prepared = std::move(prepared), maximum_input_row_count,
       maximum_filter_truth_memory_bytes,
       mga_context = std::move(mga_context), strict_dispatcher_memory](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "aggregate registry did not receive its bounded typed input";
          return step;
        }
        const auto& input_batch =
            *inputs.front().materialized_output_batch;
        std::string value_binding_detail;
        if (!RevalidatePreparedAggregateValueBindings(
                prepared.value_columns, prepared.value_descriptor_ids,
                prepared.exact_value_binding_receipts, input_batch,
                &value_binding_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail = std::move(value_binding_detail);
          return step;
        }
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        const exec::PhysicalNodeRecord* runtime_node = &node;
        std::size_t callback_memory_bound = 0;
        if (strict_dispatcher_memory) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildStrictUnaryOperatorLocalPhysicalDag(
                  dag, node, input_batch, 8, 128 * 1024,
                  &*scoped_execution_dag, &callback_memory_bound,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "aggregate registry " + scope_detail;
            return step;
          }
          execution_dag = &*scoped_execution_dag;
          const auto selected_runtime_node = std::ranges::find_if(
              scoped_execution_dag->nodes, [&](const auto& candidate) {
                return candidate.physical_node_id == node.physical_node_id;
              });
          if (selected_runtime_node == scoped_execution_dag->nodes.end()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "aggregate registry execution view is unresolved";
            return step;
          }
          runtime_node = &*selected_runtime_node;
        } else if (node.physical_node_id != dag.root_physical_node_id) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildOperatorLocalPhysicalDag(
                  dag, node.physical_node_id, &*scoped_execution_dag,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "aggregate registry execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        const auto aggregate_memory_bound =
            SelectedNodeAggregateMemoryBound(*execution_dag,
                                             *runtime_node);
        std::uint64_t input_memory_bytes = 1;
        std::uint64_t input_payload_bytes = 0;
        if (!aggregate_memory_bound.has_value() ||
            (strict_dispatcher_memory &&
             *aggregate_memory_bound != callback_memory_bound) ||
            !AddBatchMemoryBytes(input_batch, &input_memory_bytes) ||
            !RuntimeMaterializedBatchMemoryBytes(
                input_batch, &input_payload_bytes) ||
            input_memory_bytes > *aggregate_memory_bound ||
            maximum_filter_truth_memory_bytes >
                *aggregate_memory_bound - input_memory_bytes) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "aggregate FILTER carrier or input exceeds its selected-node "
              "memory grant";
          return step;
        }
        if (!prepared.filter_column.has_value() &&
            maximum_filter_truth_memory_bytes != 0) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "aggregate without FILTER received a truth-carrier receipt";
          return step;
        }
        std::optional<std::vector<api::EngineSqlTruthValue>>
            filter_truth_values;
        std::uint64_t retained_filter_truth_bytes = 0;
        if (prepared.filter_column.has_value()) {
          std::vector<api::EngineSqlTruthValue> materialized_filter;
          std::string filter_detail;
          if (!MaterializeAggregateFilterTruthValues(
                  input_batch, *prepared.filter_column,
                  prepared.filter_descriptor_id,
                  maximum_filter_truth_memory_bytes,
                  &materialized_filter, &retained_filter_truth_bytes,
                  &filter_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
            step.diagnostic.detail = std::move(filter_detail);
            return step;
          }
          filter_truth_values = std::move(materialized_filter);
        }
        const auto admitted_row_count =
            filter_truth_values.has_value()
                ? static_cast<std::size_t>(std::ranges::count(
                      *filter_truth_values,
                      api::EngineSqlTruthValue::true_value))
                : input_batch.rows.size();
        std::uint64_t maximum_order_comparison_count = 0;
        if ((admitted_row_count > 1 &&
             !CheckedMultiply(admitted_row_count,
                              admitted_row_count - 1,
                              &maximum_order_comparison_count)) ||
            (maximum_order_comparison_count /= 2,
             !CheckedMultiply(maximum_order_comparison_count,
                              prepared.aggregate_order_terms.size(),
                              &maximum_order_comparison_count)) ||
            *aggregate_memory_bound >
                std::numeric_limits<std::size_t>::max() ||
            maximum_order_comparison_count >
                std::numeric_limits<std::size_t>::max()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "aggregate runtime resource ceilings overflowed";
          return step;
        }
        exec::CanonicalAggregateRuntimeRequest aggregate_request;
        aggregate_request.selected_physical_node_id = node.physical_node_id;
        aggregate_request.descriptor = prepared.aggregate_descriptor;
        aggregate_request.value_columns = prepared.value_columns;
        aggregate_request.value_expression_descriptor_ids =
            prepared.value_descriptor_ids;
        aggregate_request.direct_arguments = prepared.direct_arguments;
        aggregate_request.result_column = prepared.result_column;
        aggregate_request.filter_truth_values =
            std::move(filter_truth_values);
        std::uint64_t logical_filter_truth_bytes = 0;
        if ((prepared.filter_column.has_value() &&
             !CheckedMultiply(input_batch.rows.size(),
                              sizeof(api::EngineSqlTruthValue),
                              &logical_filter_truth_bytes)) ||
            logical_filter_truth_bytes > retained_filter_truth_bytes) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "aggregate FILTER truth-carrier receipt is inconsistent";
          return step;
        }
        aggregate_request.retained_memory_bytes =
            static_cast<std::size_t>(retained_filter_truth_bytes -
                                     logical_filter_truth_bytes);
        aggregate_request.distinct = prepared.distinct;
        aggregate_request.aggregate_order_terms =
            prepared.aggregate_order_terms;
        aggregate_request.aggregate_separator =
            prepared.aggregate_separator;
        aggregate_request.listagg_overflow_mode =
            prepared.listagg_overflow_mode;
        aggregate_request.listagg_max_output_bytes =
            prepared.listagg_max_output_bytes;
        aggregate_request.listagg_truncation_indicator =
            prepared.listagg_truncation_indicator;
        aggregate_request.listagg_with_count =
            prepared.listagg_with_count;
        aggregate_request.forced_strategy =
            exec::CanonicalAggregateExecutionStrategy::serial;
        aggregate_request.maximum_transition_count =
            std::max<std::size_t>(1, input_batch.rows.size());
        aggregate_request.maximum_distinct_value_count =
            std::max<std::size_t>(1, admitted_row_count);
        aggregate_request.maximum_aggregate_order_term_count =
            std::max<std::size_t>(
                1, prepared.aggregate_order_terms.size());
        aggregate_request.maximum_order_comparison_count =
            static_cast<std::size_t>(std::max<std::uint64_t>(
                1, maximum_order_comparison_count));
        aggregate_request.maximum_state_bytes =
            static_cast<std::size_t>(*aggregate_memory_bound);
        aggregate_request.maximum_final_output_bytes =
            static_cast<std::size_t>(*aggregate_memory_bound);
        aggregate_request.maximum_finalization_workspace_bytes =
            static_cast<std::size_t>(*aggregate_memory_bound);
        aggregate_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(
                mga_context, *execution_dag);
        std::string equality_detail;
        if (!BindCanonicalAggregateEqualityTerms(
                mga_context, input_batch, &aggregate_request,
                &equality_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-EQUALITY-V1";
          step.diagnostic.detail = std::move(equality_detail);
          return step;
        }
        auto aggregate_result = exec::ExecuteCanonicalAggregateRuntime(
            aggregate_request, *execution_dag, input_batch);
        if (!aggregate_result.diagnostic.ok) {
          step.diagnostic = std::move(aggregate_result.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                aggregate_result, *execution_dag, node,
                aggregate_request.mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-AGGREGATE-EXECUTION-V1";
          step.diagnostic.detail =
              "aggregate registry execution receipt changed";
          return step;
        }
        std::uint64_t output_memory_bytes = 0;
        if (!RuntimeMaterializedBatchMemoryBytes(
                aggregate_result.output_batch, &output_memory_bytes) ||
            aggregate_result.executed_strategy !=
                exec::CanonicalAggregateExecutionStrategy::serial ||
            aggregate_result.input_payload_bytes != input_payload_bytes ||
            aggregate_result.fixed_retained_memory_bytes !=
                retained_filter_truth_bytes ||
            aggregate_result.current_memory_bytes != output_memory_bytes ||
            aggregate_result.current_memory_bytes >
                aggregate_result.peak_memory_bytes ||
            aggregate_result.peak_memory_bytes >
                *aggregate_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "aggregate runtime phase memory receipt is inconsistent";
          return step;
        }
        step.authority = aggregate_result.authority;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = aggregate_result.output_batch.rows.size();
        step.materialized_output_batch =
            std::move(aggregate_result.output_batch);
        PublishRuntimeMemoryObservation(
            &step, output_memory_bytes,
            aggregate_result.peak_memory_bytes);
        step.mga_statement_context =
            aggregate_request.mga_authority.statement_context;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveGroupedCountSumRegistration(
    PreparedGroupedCountSumRoot prepared,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::size_t maximum_output_row_count,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kAggregate;
  registration.implementation_id = "aggregate.registry-grouping-sets.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 = true;
  registration.honors_dispatcher_memory_limit_v1 = true;
  registration.execute =
      [prepared = std::move(prepared), maximum_input_row_count,
       maximum_output_row_count, mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "composed grouped COUNT/SUM input or projection is invalid";
          return step;
        }
        const auto& input_batch =
            *inputs.front().materialized_output_batch;
        std::string key_binding_detail;
        if (!RevalidatePreparedGroupedKeyBindings(
                prepared, input_batch, &key_binding_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail = std::move(key_binding_detail);
          return step;
        }
        exec::TypedPhysicalNodeDag scoped_execution_dag;
        std::size_t callback_memory_bound = 0;
        std::string scope_detail;
        if (!BuildStrictUnaryOperatorLocalPhysicalDag(
                dag, node, input_batch, 8, 128 * 1024,
                &scoped_execution_dag, &callback_memory_bound,
                &scope_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail =
              "grouped aggregate " + scope_detail;
          return step;
        }
        const exec::TypedPhysicalNodeDag* execution_dag =
            &scoped_execution_dag;
        const auto runtime_node = std::ranges::find_if(
            scoped_execution_dag.nodes, [&](const auto& candidate) {
              return candidate.physical_node_id == node.physical_node_id;
            });
        if (runtime_node == scoped_execution_dag.nodes.end()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "composed grouped aggregate execution view is unresolved";
          return step;
        }
        if (!prepared.grouping_projection_columns.empty()) {
          auto& execution_view = scoped_execution_dag;
          if (runtime_node->output_descriptor_ids.size() !=
                  prepared.key_terms.size() + 2 +
                      prepared.grouping_projection_columns.size()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "composed grouping projection descriptor shape drifted";
            return step;
          }
          runtime_node->output_descriptor_ids.resize(
              prepared.key_terms.size() + 2);
        }
        const auto aggregate_memory_bound =
            SelectedNodeAggregateMemoryBound(*execution_dag,
                                             *runtime_node);
        if (!aggregate_memory_bound.has_value() ||
            *aggregate_memory_bound != callback_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "grouped aggregate finalization memory grant is unresolved";
          return step;
        }
        const auto make_aggregate = [aggregate_memory_bound](
                                        const PreparedGlobalAggregateRoot&
                                            prepared_aggregate) {
          exec::CanonicalAggregateRuntimeRequest aggregate;
          aggregate.descriptor = prepared_aggregate.aggregate_descriptor;
          aggregate.value_columns = prepared_aggregate.value_columns;
          aggregate.value_expression_descriptor_ids =
              prepared_aggregate.value_descriptor_ids;
          aggregate.direct_arguments = prepared_aggregate.direct_arguments;
          aggregate.result_column = prepared_aggregate.result_column;
          aggregate.distinct = prepared_aggregate.distinct;
          aggregate.aggregate_order_terms =
              prepared_aggregate.aggregate_order_terms;
          aggregate.aggregate_separator =
              prepared_aggregate.aggregate_separator;
          aggregate.listagg_overflow_mode =
              prepared_aggregate.listagg_overflow_mode;
          aggregate.listagg_max_output_bytes =
              prepared_aggregate.listagg_max_output_bytes;
          aggregate.listagg_truncation_indicator =
              prepared_aggregate.listagg_truncation_indicator;
          aggregate.listagg_with_count =
              prepared_aggregate.listagg_with_count;
          aggregate.forced_strategy =
              exec::CanonicalAggregateExecutionStrategy::serial;
          aggregate.maximum_final_output_bytes = *aggregate_memory_bound;
          aggregate.maximum_finalization_workspace_bytes =
              *aggregate_memory_bound;
          return aggregate;
        };
        exec::CanonicalGroupedAggregateSetRuntimeRequest grouped_request;
        auto& first = grouped_request.first_aggregate;
        first.aggregate_request = make_aggregate(prepared.count);
        first.aggregate_request.selected_physical_node_id =
            node.physical_node_id;
        first.aggregate_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(
                mga_context, *execution_dag);
        first.group_key_terms = prepared.key_terms;
        first.group_result_columns = prepared.key_result_columns;
        first.grouping_sets = prepared.grouping_sets;
        first.maximum_grouping_key_comparison_count =
            prepared.maximum_grouping_key_comparison_count;
        first.maximum_group_count = maximum_output_row_count;
        first.maximum_output_rows = maximum_output_row_count;
        first.maximum_combined_final_output_bytes =
            *aggregate_memory_bound;
        auto sum = make_aggregate(prepared.sum);
        sum.mga_authority = first.aggregate_request.mga_authority;
        grouped_request.additional_aggregates = {std::move(sum)};
        grouped_request.maximum_combined_grouping_key_comparison_count =
            prepared.maximum_combined_grouping_key_comparison_count;
        grouped_request.maximum_combined_final_output_bytes =
            *aggregate_memory_bound;
        auto grouped = exec::ExecuteCanonicalGroupedAggregateSetRuntime(
            grouped_request, *execution_dag, input_batch);
        if (!grouped.diagnostic.ok) {
          step.diagnostic = std::move(grouped.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                grouped, *execution_dag, node,
                first.aggregate_request.mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-EXECUTION-V1";
          step.diagnostic.detail =
              "grouped COUNT/SUM execution receipt changed";
          return step;
        }
        std::uint64_t input_memory_bytes = 0;
        std::uint64_t unprojected_output_memory_bytes = 0;
        std::uint64_t peak_memory_bytes = grouped.peak_memory_bytes;
        if (!RuntimeMaterializedBatchMemoryBytes(input_batch,
                                                 &input_memory_bytes) ||
            !RuntimeMaterializedBatchMemoryBytes(
                grouped.output_batch,
                &unprojected_output_memory_bytes) ||
            grouped.input_payload_bytes != input_memory_bytes ||
            grouped.fixed_retained_memory_bytes != input_memory_bytes ||
            grouped.output_payload_bytes !=
                unprojected_output_memory_bytes ||
            grouped.current_memory_bytes !=
                unprojected_output_memory_bytes ||
            grouped.current_memory_bytes > grouped.peak_memory_bytes ||
            grouped.peak_memory_bytes > *aggregate_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "grouped aggregate-set runtime memory receipt is inconsistent";
          return step;
        }
        if (!grouped.group_identity_proven ||
            !grouped.shared_state_authority_used ||
            grouped.aggregate_count != 2 ||
            grouped.groups.size() != grouped.output_batch.rows.size() ||
            grouped.output_batch.rows.size() > maximum_output_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
          step.diagnostic.detail =
              "composed grouped COUNT/SUM shared state is unproven";
          return step;
        }
        std::uint64_t observed_set_memory_bytes = 0;
        std::uint64_t expected_indicator_memory_bytes = 0;
        std::uint64_t validation_phase_memory_bytes = 0;
        if (!LogicalBitVectorPayloadBytes(prepared.grouping_sets.size(),
                                          &observed_set_memory_bytes) ||
            !LogicalBitVectorPayloadBytes(prepared.key_terms.size(),
                                          &expected_indicator_memory_bytes) ||
            !CheckedAdd(grouped.fixed_retained_memory_bytes,
                        unprojected_output_memory_bytes,
                        &validation_phase_memory_bytes) ||
            !CheckedAdd(validation_phase_memory_bytes,
                        observed_set_memory_bytes,
                        &validation_phase_memory_bytes) ||
            !CheckedAdd(validation_phase_memory_bytes,
                        expected_indicator_memory_bytes,
                        &validation_phase_memory_bytes) ||
            validation_phase_memory_bytes > *aggregate_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "grouped aggregate validation memory exceeded its grant";
          return step;
        }
        peak_memory_bytes =
            std::max(peak_memory_bytes, validation_phase_memory_bytes);
        {
          std::vector<bool> grouping_sets_observed(
              prepared.grouping_sets.size(), false);
          for (const auto& group : grouped.groups) {
            if (group.grouping_set_ordinal >=
                    prepared.grouping_sets.size() ||
                group.grouping_indicators.size() !=
                    prepared.key_terms.size()) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "composed grouped runtime returned invalid metadata";
              return step;
            }
            grouping_sets_observed[group.grouping_set_ordinal] = true;
            const auto expected =
                exec::ComputeCanonicalAggregateGroupingMetadata(
                    prepared.key_terms.size(),
                    prepared.grouping_sets[group.grouping_set_ordinal]);
            if (!expected.diagnostic.ok ||
                group.grouping_indicators !=
                    expected.grouping_indicators ||
                group.grouping_id != expected.grouping_id) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "composed grouping metadata identity drifted";
              return step;
            }
          }
          std::size_t empty_grouping_set_count = 0;
          for (std::size_t set_ordinal = 0;
               set_ordinal < prepared.grouping_sets.size(); ++set_ordinal) {
            const bool empty_grouping_set =
                prepared.grouping_sets[set_ordinal]
                    .key_term_ordinals.empty();
            const bool expected_observed =
                !input_batch.rows.empty() || empty_grouping_set;
            if (empty_grouping_set) ++empty_grouping_set_count;
            if (grouping_sets_observed[set_ordinal] !=
                expected_observed) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "composed grouped runtime grouping-set cardinality drifted";
              return step;
            }
          }
          if (input_batch.rows.empty() &&
              grouped.groups.size() != empty_grouping_set_count) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "composed empty-input grouping-set cardinality drifted";
            return step;
          }
        }
        std::uint64_t expected_output_memory_bytes =
            unprojected_output_memory_bytes;
        if (!prepared.grouping_projection_columns.empty()) {
          if (prepared.grouping_projection_columns.size() !=
                  prepared.key_terms.size() + 1 ||
              grouped.output_batch.columns.size() !=
                  prepared.key_terms.size() + 2) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
            step.diagnostic.detail =
                "composed grouping projection result shape drifted";
            return step;
          }
          for (const auto& metadata : grouped.groups) {
            if (metadata.grouping_id >
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()) ||
                !CheckedAdd(expected_output_memory_bytes,
                            prepared.key_terms.size(),
                            &expected_output_memory_bytes) ||
                !CheckedAdd(
                    expected_output_memory_bytes,
                    CanonicalUnsignedDecimalWidth(metadata.grouping_id),
                    &expected_output_memory_bytes)) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "QOW-DIAG-RELATIONAL-LIVE-GROUPED-AGGREGATE-INPUT-V1";
              step.diagnostic.detail =
                  "composed GROUPING projection payload is invalid";
              return step;
            }
          }
          std::uint64_t prospective_projection_phase_memory_bytes = 0;
          if (!CheckedAdd(grouped.fixed_retained_memory_bytes,
                          expected_output_memory_bytes,
                          &prospective_projection_phase_memory_bytes) ||
              prospective_projection_phase_memory_bytes >
                  *aggregate_memory_bound) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-OPT-017-REFUSAL-V1";
            step.diagnostic.detail =
                "grouped aggregate projection exceeded its memory grant";
            return step;
          }
          peak_memory_bytes = std::max(
              peak_memory_bytes,
              prospective_projection_phase_memory_bytes);
          grouped.output_batch.columns.insert(
              grouped.output_batch.columns.end(),
              prepared.grouping_projection_columns.begin(),
              prepared.grouping_projection_columns.end());
          for (std::size_t group_ordinal = 0;
               group_ordinal < grouped.groups.size(); ++group_ordinal) {
            auto& output_row = grouped.output_batch.rows[group_ordinal];
            const auto& metadata = grouped.groups[group_ordinal];
            for (std::size_t key_ordinal = 0;
                 key_ordinal < prepared.key_terms.size(); ++key_ordinal) {
              api::EngineTypedValue indicator;
              indicator.descriptor =
                  prepared.grouping_projection_columns[key_ordinal]
                      .descriptor;
              indicator.encoded_value =
                  metadata.grouping_indicators[key_ordinal] ? "1" : "0";
              indicator.state = api::EngineValueState::value;
              output_row.values.push_back(std::move(indicator));
            }
            api::EngineTypedValue grouping_id;
            grouping_id.descriptor =
                prepared.grouping_projection_columns.back().descriptor;
            grouping_id.encoded_value =
                std::to_string(metadata.grouping_id);
            grouping_id.state = api::EngineValueState::value;
            output_row.values.push_back(std::move(grouping_id));
          }
          const auto projected = exec::ValidateCanonicalDescriptorBatch(
              grouped.output_batch, node.output_descriptor_ids);
          if (!projected.ok) {
            step.diagnostic = projected;
            return step;
          }
        }
        std::uint64_t output_memory_bytes = 0;
        std::uint64_t projection_phase_memory_bytes = 0;
        if (!RuntimeMaterializedBatchMemoryBytes(
                grouped.output_batch, &output_memory_bytes) ||
            output_memory_bytes != expected_output_memory_bytes ||
            !CheckedAdd(grouped.fixed_retained_memory_bytes,
                        output_memory_bytes,
                        &projection_phase_memory_bytes)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "grouped aggregate projection memory receipt overflowed";
          return step;
        }
        peak_memory_bytes =
            std::max(peak_memory_bytes, projection_phase_memory_bytes);
        if (output_memory_bytes > peak_memory_bytes ||
            peak_memory_bytes > *aggregate_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "grouped aggregate projection exceeded its memory grant";
          return step;
        }
        step.authority = grouped.authority;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = step.input_row_count;
        step.output_row_count = grouped.output_batch.rows.size();
        step.materialized_output_batch = std::move(grouped.output_batch);
        PublishRuntimeMemoryObservation(
            &step, output_memory_bytes, peak_memory_bytes);
        step.mga_statement_context =
            std::move(grouped.mga_statement_context);
        return step;
      };
  return registration;
}

}  // namespace scratchbird::engine::sblr
