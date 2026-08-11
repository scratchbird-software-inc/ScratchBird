// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_executor.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

bool CanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool CheckedAsofAdd(const std::uint64_t value, std::uint64_t* total) {
  if (total == nullptr ||
      value > std::numeric_limits<std::uint64_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}

bool CheckedAsofMultiply(const std::uint64_t left,
                         const std::uint64_t right,
                         std::uint64_t* product) {
  if (product == nullptr ||
      (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right)) {
    return false;
  }
  *product = left * right;
  return true;
}

bool AccountAsofBytes(const std::uint64_t bytes,
                      const std::uint64_t limit,
                      std::uint64_t* total) {
  return CheckedAsofAdd(bytes, total) && *total <= limit;
}

bool AccountAsofString(const std::string_view value,
                       const std::uint64_t limit,
                       std::uint64_t* total) {
  return value.size() != std::numeric_limits<std::size_t>::max() &&
         AccountAsofBytes(static_cast<std::uint64_t>(value.size()) + 1,
                          limit, total);
}

bool AccountAsofDescriptor(
    const internal_api::EngineDescriptor& descriptor,
    const std::uint64_t limit,
    std::uint64_t* total) {
  return AccountAsofString(descriptor.descriptor_uuid.canonical, limit,
                           total) &&
         AccountAsofString(descriptor.descriptor_kind, limit, total) &&
         AccountAsofString(descriptor.canonical_type_name, limit, total) &&
         AccountAsofString(descriptor.encoded_descriptor, limit, total);
}

bool AccountAsofValueDynamic(
    const internal_api::EngineTypedValue& value,
    const std::uint64_t limit,
    std::uint64_t* total) {
  return AccountAsofDescriptor(value.descriptor, limit, total) &&
         AccountAsofString(value.encoded_value, limit, total) &&
         AccountAsofBytes(static_cast<std::uint64_t>(value.binary_value.size()),
                          limit, total);
}

bool DeriveAsofNullableDescriptorEncoding(
    internal_api::EngineDescriptor* descriptor) {
  if (descriptor == nullptr || descriptor->encoded_descriptor.empty()) {
    return false;
  }
  std::string derived;
  derived.reserve(descriptor->encoded_descriptor.size());
  bool nullability_carrier_seen = false;
  std::size_t offset = 0;
  while (offset <= descriptor->encoded_descriptor.size()) {
    const auto separator = descriptor->encoded_descriptor.find(';', offset);
    const auto end = separator == std::string::npos
                         ? descriptor->encoded_descriptor.size()
                         : separator;
    const std::string_view field(descriptor->encoded_descriptor.data() + offset,
                                 end - offset);
    if (!derived.empty()) derived.push_back(';');
    if (field.starts_with("nullability=")) {
      derived.append("nullability=nullable");
      nullability_carrier_seen = true;
    } else if (field.starts_with("nullable=")) {
      derived.append("nullable=true");
      nullability_carrier_seen = true;
    } else {
      derived.append(field);
    }
    if (separator == std::string::npos) break;
    offset = separator + 1;
  }
  if (!nullability_carrier_seen) return false;
  descriptor->encoded_descriptor = std::move(derived);
  return true;
}

bool HasAsofNullableDescriptorCarrier(
    const internal_api::EngineDescriptor& descriptor) {
  std::size_t offset = 0;
  bool carrier = false;
  while (offset <= descriptor.encoded_descriptor.size()) {
    const auto separator = descriptor.encoded_descriptor.find(';', offset);
    const auto end = separator == std::string::npos
                         ? descriptor.encoded_descriptor.size()
                         : separator;
    const std::string_view field(
        descriptor.encoded_descriptor.data() + offset, end - offset);
    if (field.starts_with("nullability=") ||
        field.starts_with("nullable=")) {
      if (carrier) return false;
      carrier = true;
    }
    if (separator == std::string::npos) break;
    offset = separator + 1;
  }
  return carrier;
}

std::string AsofBindingReceipt(
    const CanonicalTimeSeriesAsofInputBindingV1& binding) {
  return std::to_string(binding.metric_expression_id) + "." +
         std::to_string(binding.tags_expression_id) + "." +
         std::to_string(binding.timestamp_expression_id) + "." +
         std::to_string(binding.row_uuid_expression_id) + "." +
         std::to_string(binding.metric_descriptor_id) + "." +
         std::to_string(binding.tags_descriptor_id) + "." +
         std::to_string(binding.timestamp_descriptor_id) + "." +
         std::to_string(binding.row_uuid_descriptor_id) + "." +
         std::to_string(binding.metric_column_ordinal) + "." +
         std::to_string(binding.tags_column_ordinal) + "." +
         std::to_string(binding.timestamp_column_ordinal) + "." +
         std::to_string(binding.row_uuid_column_ordinal) + "." +
         (binding.raw_time_series ? "r" :
          binding.downsample_time_series ? "d" : "n");
}

std::string AsofTransformationReceipt(
    const CanonicalTimeSeriesAsofJoinRequestV1& request) {
  return std::string("asof.b.t.") + std::to_string(request.tolerance_ns) +
         ".w." + std::to_string(request.maximum_comparisons) + ".l." +
         AsofBindingReceipt(request.left_binding) + ".r." +
         AsofBindingReceipt(request.right_binding) +
         ".n." + std::to_string(request.maximum_output_rows) +
         (request.left_outer ? ".o.v1" : ".i.v1");
}

}  // namespace

std::string CanonicalTimeSeriesAsofTransformationReceiptV1(
    const CanonicalTimeSeriesAsofJoinRequestV1& request) {
  return AsofTransformationReceipt(request);
}

ModelFamilyExecutionResultV1 ExecuteModelFamilySourceV1(
    const ModelFamilyExecutionRequestV1& request) {
  // QOW-SOURCE-CES05-MODEL-FAMILY-EXECUTOR-V1
  ModelFamilyExecutionResultV1 result;
  const auto refuse = [&](const char* diagnostic, std::string detail) {
    result.accepted = false;
    result.root_published = false;
    result.diagnostic_id = diagnostic;
    result.detail = std::move(detail);
  };

  const auto input_validation = ValidateModelFamilySourceInputV1(request.input);
  if (!input_validation.accepted) {
    refuse(input_validation.diagnostic_id.c_str(), input_validation.detail);
    return result;
  }
  const bool graph_family = request.input.family_id == "graph";
  const bool key_value_family = request.input.family_id == "key_value";
  const bool time_series_family = request.input.family_id == "time_series";
  if (request.current_catalog_generation != request.input.catalog_generation) {
    refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
           "model-family catalog generation changed before provider access");
    return result;
  }
  if (request.current_descriptor_generation !=
      request.input.descriptor_generation) {
    refuse(kModelTypedExchangeInvalid,
           "model-family descriptor generation changed before provider access");
    return result;
  }
  if (request.current_security_generation !=
          request.input.security_generation ||
      request.current_policy_generation != request.input.policy_generation ||
      request.current_resource_generation !=
          request.input.resource_generation) {
    refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
           "model-family authority generation changed before access");
    return result;
  }
  if (request.current_provider_generation !=
      request.input.provider_generation) {
    refuse("SB_MODEL_PROVIDER_GENERATION_STALE_V1",
           "model-family provider generation changed before access");
    return result;
  }

  if (request.abi_version != 1 || request.capability.abi_version != 1 ||
      request.capability.capability_descriptor_id !=
          "SB_MODEL_CAPABILITY_DESCRIPTOR_V1" ||
      request.capability.family_id != request.input.family_id ||
      (request.capability.family_id != "document" &&
       request.capability.family_id != "graph" &&
       request.capability.family_id != "key_value" &&
       request.capability.family_id != "time_series") ||
      request.capability.capability_uuid !=
          request.input.capability_uuid ||
      request.capability.provider_uuid != request.input.provider_uuid ||
      request.capability.provider_generation !=
          request.input.provider_generation ||
      !CanonicalUuid(request.capability.capability_uuid) ||
      !request.capability.local_scope || !request.capability.available ||
      !request.capability.exact ||
      !request.capability.cancellation_supported ||
      !request.capability.cleanup_supported ||
      !request.capability.residual_recheck_supported ||
      !request.capability.base_row_mga_recheck_supported ||
      !request.capability.security_recheck_supported ||
      request.capability.provider_visibility_authority_claimed ||
      request.capability.provider_finality_authority_claimed ||
      ((key_value_family || time_series_family) &&
       request.exact_fallback_selected !=
           request.input.exact_fallback_selected) ||
      !request.execute_provider || !request.cancellation_requested ||
      !request.cleanup_provider) {
    refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
           "model-family executor capability is unavailable or semantically invalid");
    return result;
  }
  if (request.exact_fallback_selected &&
      !request.capability.exact_collection_fallback_available) {
    refuse(graph_family
               ? "SB_MODEL_GRAPH_EXACT_FALLBACK_UNAVAILABLE_V1"
               : key_value_family
                     ? "SB_MODEL_KEY_VALUE_EXACT_FALLBACK_UNAVAILABLE_V1"
                     : time_series_family
                           ? "SB_MODEL_TIME_SERIES_EXACT_FALLBACK_UNAVAILABLE_V1"
                     : "SB_MODEL_DOCUMENT_EXACT_FALLBACK_UNAVAILABLE_V1",
           "the exact model-family fallback is unavailable");
    return result;
  }
  if (!request.security_admitted) {
    refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
           "model-family provider access was not admitted by engine security");
    return result;
  }
  if (!PhysicalMgaStatementContextValid(
          request.current_mga_statement_context) ||
      !PhysicalMgaStatementContextEqual(
          request.input.mga_statement_context,
          request.current_mga_statement_context)) {
    refuse(kModelMgaContextMismatch,
           "current engine MGA statement context differs from the selected input");
    return result;
  }
  if (request.input.maximum_memory_bytes == 0 ||
      request.input.maximum_rows == 0 || request.input.maximum_cells == 0) {
    refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
           "model-family executor has no bounded resource admission");
    return result;
  }
  try {
    if (request.cancellation_requested()) {
      refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
             "model-family execution was cancelled before data access");
      return result;
    }
  } catch (...) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family cancellation probe failed before data access");
    return result;
  }

  result.execution_started = true;
  bool cleanup_attempted = false;
  const auto cleanup_once = [&]() noexcept {
    if (cleanup_attempted) return result.cleanup_complete;
    cleanup_attempted = true;
    result.cleanup_count = 1;
    try {
      request.cleanup_provider();
      result.cleanup_complete = true;
    } catch (...) {
      result.cleanup_complete = false;
      result.accepted = false;
      result.root_published = false;
      result.diagnostic_id = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
      result.detail = "model-family provider cleanup failed";
    }
    return result.cleanup_complete;
  };

  if (request.fault_injected) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family provider leg failed before exchange");
    cleanup_once();
    return result;
  }
  ModelProviderExecutionResultV1 provider;
  try {
    provider = request.execute_provider(request.input);
  } catch (...) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family provider threw during execution");
    cleanup_once();
    return result;
  }
  result.data_access_observed = provider.data_access_observed;
  result.rows_examined = provider.rows_examined;
  if (!provider.ok) {
    refuse(provider.diagnostic_id.empty()
               ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
               : provider.diagnostic_id.c_str(),
           provider.detail.empty() ? provider.diagnostic_id
                                   : std::move(provider.detail));
    cleanup_once();
    return result;
  }
  try {
    if (request.cancellation_requested()) {
      refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
             "model-family execution was cancelled after provider access");
      cleanup_once();
      return result;
    }
  } catch (...) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family cancellation probe failed after provider access");
    cleanup_once();
    return result;
  }
  auto exchange = PublishModelFamilyExchangeV1(request.input,
                                                provider.provider_batch,
                                                request.cancellation_requested);
  if (!exchange.accepted) {
    refuse(exchange.diagnostic_id.c_str(), std::move(exchange.detail));
    cleanup_once();
    return result;
  }
  try {
    if (request.cancellation_requested()) {
      refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
             "model-family execution was cancelled before root publication");
      cleanup_once();
      return result;
    }
  } catch (...) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family cancellation probe failed before root publication");
    cleanup_once();
    return result;
  }

  result.accepted = true;
  result.root_published = exchange.root_publishable;
  result.output = std::move(exchange.output);
  if (!cleanup_once()) {
    result.accepted = false;
    result.root_published = false;
  }
  return result;
}

CanonicalTimeSeriesAsofJoinResultV1 ExecuteCanonicalTimeSeriesAsofJoinV1(
    const CanonicalTimeSeriesAsofJoinRequestV1& request) {
  CanonicalTimeSeriesAsofJoinResultV1 result;
  const auto refuse = [&](std::string code, std::string detail,
                          const std::size_t row = 0) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    result.diagnostic.row_index = row;
    result.output_batch = {};
    result.matched_right_ordinals.clear();
    return result;
  };
  const auto cancelled = [&]() {
    try {
      return request.cancellation_requested && request.cancellation_requested();
    } catch (...) {
      return true;
    }
  };
  if (request.physical_dag.abi_version != 2 ||
      !request.cancellation_requested || request.tolerance_ns < 0 ||
      request.maximum_output_rows == 0 ||
      request.maximum_comparisons == 0 ||
      request.maximum_memory_bytes == 0 ||
      request.maximum_memory_bytes !=
          request.physical_dag.memory_budget_bytes) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "ASOF join contract is incomplete or lacks ABI-v2 MGA authority");
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "ASOF join was cancelled before input validation");
  }
  const auto authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority.ok) return refuse(authority.diagnostic_code, authority.detail);
  const PhysicalNodeRecord* root = nullptr;
  const PhysicalNodeRecord* left_node = nullptr;
  const PhysicalNodeRecord* right_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) root = &node;
  }
  const auto expected_semantic = request.left_outer
                                     ? "join.asof.left.v1"
                                     : "join.asof.inner.v1";
  const auto expected_implementation = request.left_outer
                                           ? "join.asof.left.typed.v1"
                                           : "join.asof.inner.typed.v1";
  const auto expected_transformation_rule =
      CanonicalTimeSeriesAsofTransformationReceiptV1(request);
  if (root == nullptr || request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id ||
      root->node_kind != PhysicalNodeKind::kJoin ||
      root->input_physical_node_ids.size() != 2 ||
      root->logical_semantic_variant_id != expected_semantic ||
      root->implementation_id != expected_implementation ||
      !CanonicalUuid(root->transformation_uuid) ||
      root->transformation_rule_id != expected_transformation_rule) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "ASOF direction, tolerance, or disposition differs from the selected root receipt");
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == root->input_physical_node_ids[0]) {
      left_node = &node;
    }
    if (node.physical_node_id == root->input_physical_node_ids[1]) {
      right_node = &node;
    }
  }
  if (left_node == nullptr || right_node == nullptr) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                  "ASOF input node is unresolved");
  }
  if (root->output_descriptor_ids.size() !=
          left_node->output_descriptor_ids.size() +
              right_node->output_descriptor_ids.size() ||
      !std::equal(left_node->output_descriptor_ids.begin(),
                  left_node->output_descriptor_ids.end(),
                  root->output_descriptor_ids.begin()) ||
      !std::equal(right_node->output_descriptor_ids.begin(),
                  right_node->output_descriptor_ids.end(),
                  root->output_descriptor_ids.begin() +
                      left_node->output_descriptor_ids.size())) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "ASOF output descriptor order does not concatenate inputs");
  }
  auto left_validation = ValidateCanonicalDescriptorBatch(
      request.left_batch, left_node->output_descriptor_ids);
  if (!left_validation.ok) {
    return refuse(left_validation.diagnostic_code, left_validation.detail);
  }
  auto right_validation = ValidateCanonicalDescriptorBatch(
      request.right_batch, right_node->output_descriptor_ids);
  if (!right_validation.ok) {
    return refuse(right_validation.diagnostic_code, right_validation.detail);
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "ASOF join was cancelled during descriptor validation");
  }
  if (request.left_keys.size() != request.left_batch.rows.size() ||
      request.right_keys.size() != request.right_batch.rows.size() ||
      (request.right_is_time_series_raw &&
       request.right_tie_break_row_uuids.size() !=
           request.right_batch.rows.size()) ||
      (!request.right_is_time_series_raw &&
       !request.right_tie_break_row_uuids.empty()) ||
      request.left_batch.rows.size() > request.maximum_output_rows) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "ASOF key cardinality or output row bound is invalid");
  }
  const bool left_time_series = request.left_binding.raw_time_series ||
                                request.left_binding.downsample_time_series;
  const bool right_time_series = request.right_binding.raw_time_series ||
                                 request.right_binding.downsample_time_series;
  if (left_time_series == right_time_series ||
      (request.left_binding.raw_time_series &&
       request.left_binding.downsample_time_series) ||
      (request.right_binding.raw_time_series &&
       request.right_binding.downsample_time_series) ||
      request.right_is_time_series_raw !=
          request.right_binding.raw_time_series) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "ASOF input model-family binding is incomplete");
  }
  std::uint64_t key_preflight_memory = sizeof(result);
  std::uint64_t key_vector_bytes = 0;
  bool key_preflight_ok =
      CheckedAsofMultiply(
          static_cast<std::uint64_t>(request.left_keys.size()),
          sizeof(CanonicalTimeSeriesAsofKeyV1), &key_vector_bytes) &&
      AccountAsofBytes(key_vector_bytes, request.maximum_memory_bytes,
                       &key_preflight_memory) &&
      CheckedAsofMultiply(
          static_cast<std::uint64_t>(request.right_keys.size()),
          sizeof(CanonicalTimeSeriesAsofKeyV1), &key_vector_bytes) &&
      AccountAsofBytes(key_vector_bytes, request.maximum_memory_bytes,
                       &key_preflight_memory);
  const auto account_keys = [&](const auto& keys) {
    for (const auto& key : keys) {
      key_preflight_ok =
          key_preflight_ok &&
          AccountAsofString(key.metric_uuid, request.maximum_memory_bytes,
                            &key_preflight_memory) &&
          AccountAsofString(key.canonical_tags,
                            request.maximum_memory_bytes,
                            &key_preflight_memory);
    }
  };
  account_keys(request.left_keys);
  account_keys(request.right_keys);
  for (const auto& tie : request.right_tie_break_row_uuids) {
    key_preflight_ok =
        key_preflight_ok &&
        AccountAsofString(tie, request.maximum_memory_bytes,
                          &key_preflight_memory);
  }
  const auto account_bound_key_cells =
      [&](const DescriptorBatch& batch,
          const CanonicalTimeSeriesAsofInputBindingV1& binding) {
        for (const auto& row : batch.rows) {
          if (binding.metric_column_ordinal >= row.values.size() ||
              binding.tags_column_ordinal >= row.values.size() ||
              binding.timestamp_column_ordinal >= row.values.size()) {
            key_preflight_ok = false;
            continue;
          }
          key_preflight_ok =
              key_preflight_ok &&
              AccountAsofString(
                  row.values[binding.metric_column_ordinal].encoded_value,
                  request.maximum_memory_bytes, &key_preflight_memory) &&
              AccountAsofString(
                  row.values[binding.tags_column_ordinal].encoded_value,
                  request.maximum_memory_bytes, &key_preflight_memory) &&
              AccountAsofString(
                  row.values[binding.timestamp_column_ordinal].encoded_value,
                  request.maximum_memory_bytes, &key_preflight_memory);
        }
      };
  account_bound_key_cells(request.left_batch, request.left_binding);
  account_bound_key_cells(request.right_batch, request.right_binding);
  if (!key_preflight_ok) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "ASOF bound-key preflight exceeded its memory bound");
  }
  const auto validate_bound_keys = [&]<typename Keys>(
                                       const DescriptorBatch& batch,
                                       const PhysicalNodeRecord& node,
                                       const Keys& keys,
                                       const CanonicalTimeSeriesAsofInputBindingV1&
                                           binding,
                                       const bool right_input) {
    const auto width = batch.columns.size();
    const bool core_expression_ids_distinct =
        binding.metric_expression_id != binding.tags_expression_id &&
        binding.metric_expression_id != binding.timestamp_expression_id &&
        binding.tags_expression_id != binding.timestamp_expression_id;
    const bool core_descriptor_ids_distinct =
        binding.metric_descriptor_id != binding.tags_descriptor_id &&
        binding.metric_descriptor_id != binding.timestamp_descriptor_id &&
        binding.tags_descriptor_id != binding.timestamp_descriptor_id;
    const bool core_ordinals_distinct =
        binding.metric_column_ordinal != binding.tags_column_ordinal &&
        binding.metric_column_ordinal != binding.timestamp_column_ordinal &&
        binding.tags_column_ordinal != binding.timestamp_column_ordinal;
    if (binding.metric_expression_id == 0 ||
        binding.tags_expression_id == 0 ||
        binding.timestamp_expression_id == 0 ||
        binding.metric_descriptor_id == 0 ||
        binding.tags_descriptor_id == 0 ||
        binding.timestamp_descriptor_id == 0 ||
        binding.metric_column_ordinal >= width ||
        binding.tags_column_ordinal >= width ||
        binding.timestamp_column_ordinal >= width ||
        binding.metric_column_ordinal >= node.output_descriptor_ids.size() ||
        binding.tags_column_ordinal >= node.output_descriptor_ids.size() ||
        binding.timestamp_column_ordinal >= node.output_descriptor_ids.size() ||
        !core_expression_ids_distinct || !core_descriptor_ids_distinct ||
        !core_ordinals_distinct) {
      return false;
    }
    const auto& metric_column = batch.columns[binding.metric_column_ordinal];
    const auto& tags_column = batch.columns[binding.tags_column_ordinal];
    const auto& timestamp_column =
        batch.columns[binding.timestamp_column_ordinal];
    if (metric_column.descriptor_id != binding.metric_descriptor_id ||
        tags_column.descriptor_id != binding.tags_descriptor_id ||
        timestamp_column.descriptor_id != binding.timestamp_descriptor_id ||
        node.output_descriptor_ids[binding.metric_column_ordinal] !=
            binding.metric_descriptor_id ||
        node.output_descriptor_ids[binding.tags_column_ordinal] !=
            binding.tags_descriptor_id ||
        node.output_descriptor_ids[binding.timestamp_column_ordinal] !=
            binding.timestamp_descriptor_id ||
        metric_column.descriptor.canonical_type_name != "uuid" ||
        tags_column.descriptor.canonical_type_name != "text" ||
        (timestamp_column.descriptor.canonical_type_name != "timestamp" &&
         timestamp_column.descriptor.canonical_type_name != "timestamp_tz")) {
      return false;
    }
    if (binding.raw_time_series &&
        (binding.row_uuid_expression_id == 0 ||
         binding.row_uuid_descriptor_id == 0 ||
         binding.row_uuid_column_ordinal >= width ||
         binding.row_uuid_column_ordinal >= node.output_descriptor_ids.size() ||
         batch.columns[binding.row_uuid_column_ordinal].descriptor_id !=
             binding.row_uuid_descriptor_id ||
         node.output_descriptor_ids[binding.row_uuid_column_ordinal] !=
             binding.row_uuid_descriptor_id ||
         batch.columns[binding.row_uuid_column_ordinal]
                 .descriptor.canonical_type_name != "uuid" ||
         binding.row_uuid_expression_id == binding.metric_expression_id ||
         binding.row_uuid_expression_id == binding.tags_expression_id ||
         binding.row_uuid_expression_id == binding.timestamp_expression_id ||
         binding.row_uuid_descriptor_id == binding.metric_descriptor_id ||
         binding.row_uuid_descriptor_id == binding.tags_descriptor_id ||
         binding.row_uuid_descriptor_id == binding.timestamp_descriptor_id ||
         binding.row_uuid_column_ordinal == binding.metric_column_ordinal ||
         binding.row_uuid_column_ordinal == binding.tags_column_ordinal ||
         binding.row_uuid_column_ordinal == binding.timestamp_column_ordinal)) {
      return false;
    }
    if (!binding.raw_time_series &&
        (binding.row_uuid_expression_id != 0 ||
         binding.row_uuid_descriptor_id != 0 ||
         binding.row_uuid_column_ordinal != 0)) {
      return false;
    }
    for (std::size_t ordinal = 0; ordinal < batch.rows.size(); ++ordinal) {
      if (cancelled()) return false;
      const auto& row = batch.rows[ordinal];
      const auto exact_value = [&](const std::size_t column_ordinal) {
        return column_ordinal < row.values.size() &&
               row.values[column_ordinal].state ==
                   internal_api::EngineValueState::value &&
               !row.values[column_ordinal].is_null &&
               row.values[column_ordinal].binary_value.empty();
      };
      bool tag_cancellation = false;
      std::int64_t timestamp_ns = 0;
      if (!exact_value(binding.metric_column_ordinal) ||
          !exact_value(binding.tags_column_ordinal) ||
          !exact_value(binding.timestamp_column_ordinal) ||
          row.values[binding.metric_column_ordinal].encoded_value !=
              keys[ordinal].metric_uuid ||
          row.values[binding.tags_column_ordinal].encoded_value !=
              keys[ordinal].canonical_tags ||
          !CanonicalUuid(keys[ordinal].metric_uuid) ||
          !ValidateCanonicalTimeSeriesTagsV1(
              keys[ordinal].canonical_tags, request.cancellation_requested,
              &tag_cancellation) ||
          tag_cancellation ||
          !ParseCanonicalTimeSeriesTimestampNsV1(
              row.values[binding.timestamp_column_ordinal].encoded_value,
              &timestamp_ns) ||
          timestamp_ns != keys[ordinal].timestamp_ns) {
        return false;
      }
      if (binding.raw_time_series &&
          (!exact_value(binding.row_uuid_column_ordinal) ||
           !CanonicalUuid(
               row.values[binding.row_uuid_column_ordinal].encoded_value) ||
           (right_input &&
            row.values[binding.row_uuid_column_ordinal].encoded_value !=
                request.right_tie_break_row_uuids[ordinal]))) {
        return false;
      }
    }
    return true;
  };
  if (!validate_bound_keys(request.left_batch, *left_node,
                           request.left_keys, request.left_binding, false) ||
      !validate_bound_keys(request.right_batch, *right_node,
                           request.right_keys, request.right_binding, true)) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "ASOF join was cancelled during bound-key validation");
    }
    return refuse("SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1",
                  "ASOF key vectors differ from their bound typed columns");
  }
  if (request.right_is_time_series_raw &&
      std::ranges::any_of(request.right_tie_break_row_uuids,
                          [](const auto& row_uuid) {
                            return !CanonicalUuid(row_uuid);
                          })) {
    return refuse("SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1",
                  "ASOF raw-right tie-break row identity is invalid");
  }
  const auto left_count = request.left_batch.rows.size();
  const auto right_count = request.right_batch.rows.size();
  std::uint64_t comparisons = 0;
  std::uint64_t uniqueness_comparisons = 0;
  std::uint64_t comparison_work = 0;
  // Bound-key validation inputs and the materialized result remain live at
  // the same time. Continue from the key-validation peak instead of treating
  // it as a disjoint phase.
  std::uint64_t retained_memory = key_preflight_memory;
  std::uint64_t vector_bytes = 0;
  const auto output_width = root->output_descriptor_ids.size();
  bool work_ok = CheckedAsofMultiply(
      static_cast<std::uint64_t>(left_count),
      static_cast<std::uint64_t>(right_count), &comparisons);
  if (work_ok && right_count >= 2) {
    work_ok = CheckedAsofMultiply(
        static_cast<std::uint64_t>(right_count),
        static_cast<std::uint64_t>(right_count - 1),
        &uniqueness_comparisons);
    uniqueness_comparisons /= 2;
  }
  comparison_work = uniqueness_comparisons;
  work_ok = work_ok && CheckedAsofAdd(comparisons, &comparison_work);
  bool memory_ok =
      work_ok &&
      CheckedAsofMultiply(static_cast<std::uint64_t>(output_width),
                          sizeof(ExecutorColumnDescriptor), &vector_bytes) &&
      AccountAsofBytes(vector_bytes, request.maximum_memory_bytes,
                       &retained_memory);
  for (const auto& column : request.left_batch.columns) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "ASOF join was cancelled during descriptor preflight");
    }
    memory_ok = memory_ok &&
                AccountAsofString(column.stable_name,
                                  request.maximum_memory_bytes,
                                  &retained_memory) &&
                AccountAsofDescriptor(column.descriptor,
                                      request.maximum_memory_bytes,
                                      &retained_memory);
  }
  for (const auto& column : request.right_batch.columns) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "ASOF join was cancelled during descriptor preflight");
    }
    memory_ok = memory_ok &&
                AccountAsofString(column.stable_name,
                                  request.maximum_memory_bytes,
                                  &retained_memory) &&
                AccountAsofDescriptor(column.descriptor,
                                      request.maximum_memory_bytes,
                                      &retained_memory);
  }
  memory_ok =
      memory_ok &&
      CheckedAsofMultiply(static_cast<std::uint64_t>(left_count),
                          sizeof(DescriptorTuple), &vector_bytes) &&
      AccountAsofBytes(vector_bytes, request.maximum_memory_bytes,
                       &retained_memory) &&
      CheckedAsofMultiply(static_cast<std::uint64_t>(left_count),
                          sizeof(std::int64_t), &vector_bytes) &&
      AccountAsofBytes(vector_bytes, request.maximum_memory_bytes,
                       &retained_memory);

  std::uint64_t unmatched_right_dynamic = 0;
  for (const auto& column : request.right_batch.columns) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "ASOF join was cancelled during value preflight");
    }
    memory_ok = memory_ok &&
                AccountAsofDescriptor(column.descriptor,
                                      request.maximum_memory_bytes,
                                      &unmatched_right_dynamic);
  }
  std::uint64_t maximum_right_dynamic = unmatched_right_dynamic;
  for (const auto& row : request.right_batch.rows) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "ASOF join was cancelled during value preflight");
    }
    std::uint64_t row_dynamic = 0;
    for (const auto& value : row.values) {
      memory_ok = memory_ok &&
                  AccountAsofValueDynamic(value,
                                          request.maximum_memory_bytes,
                                          &row_dynamic);
    }
    maximum_right_dynamic = std::max(maximum_right_dynamic, row_dynamic);
  }
  for (const auto& row : request.left_batch.rows) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "ASOF join was cancelled during value preflight");
    }
    memory_ok = memory_ok &&
                CheckedAsofMultiply(static_cast<std::uint64_t>(output_width),
                                    sizeof(internal_api::EngineTypedValue),
                                    &vector_bytes) &&
                AccountAsofBytes(vector_bytes, request.maximum_memory_bytes,
                                 &retained_memory);
    for (const auto& value : row.values) {
      memory_ok = memory_ok &&
                  AccountAsofValueDynamic(value,
                                          request.maximum_memory_bytes,
                                          &retained_memory);
    }
    memory_ok = memory_ok &&
                AccountAsofBytes(maximum_right_dynamic,
                                 request.maximum_memory_bytes,
                                 &retained_memory);
  }
  if (!memory_ok) {
    return refuse(
        "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
        "ASOF retained descriptor/value result memory bound was exceeded");
  }
  const auto& result_mga = request.mga_authority.statement_context;
  if (memory_ok) {
    memory_ok =
      AccountAsofString(request.physical_dag.selected_plan_uuid,
                        request.maximum_memory_bytes, &retained_memory) &&
      AccountAsofString(result_mga.statement_uuid,
                        request.maximum_memory_bytes, &retained_memory) &&
      AccountAsofString(result_mga.statement_timestamp,
                        request.maximum_memory_bytes, &retained_memory) &&
      AccountAsofString(result_mga.owning_transaction_uuid,
                        request.maximum_memory_bytes, &retained_memory) &&
      AccountAsofString(result_mga.statement_snapshot_uuid,
                        request.maximum_memory_bytes, &retained_memory) &&
      AccountAsofString(result_mga.statement_metadata_snapshot_uuid,
                        request.maximum_memory_bytes, &retained_memory) &&
      AccountAsofString(result_mga.snapshot_kind,
                        request.maximum_memory_bytes, &retained_memory);
  }
  if (memory_ok) {
    std::uint64_t exclusion_bytes = 0;
    memory_ok =
        CheckedAsofMultiply(
            static_cast<std::uint64_t>(
                result_mga.active_excluded_local_transaction_ids.size()),
            sizeof(std::uint64_t), &exclusion_bytes) &&
        AccountAsofBytes(exclusion_bytes, request.maximum_memory_bytes,
                         &retained_memory) &&
        CheckedAsofMultiply(
            static_cast<std::uint64_t>(
                result_mga.in_doubt_excluded_local_transaction_ids.size()),
            sizeof(std::uint64_t), &exclusion_bytes) &&
        AccountAsofBytes(exclusion_bytes, request.maximum_memory_bytes,
                         &retained_memory);
  }
  if (!memory_ok) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "ASOF retained result metadata exceeded its memory bound");
  }
  if (request.left_outer &&
      std::ranges::any_of(request.right_batch.columns,
                          [](const auto& column) {
                            return !HasAsofNullableDescriptorCarrier(
                                column.descriptor);
                          })) {
    return refuse(
        "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
        "ASOF LEFT input descriptor lacks a nullable derivation carrier");
  }
  if (comparison_work > request.maximum_comparisons) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "ASOF comparison work bound was exceeded");
  }
  for (std::size_t left = 0; left < right_count; ++left) {
    for (std::size_t right = left + 1; right < right_count; ++right) {
      if (cancelled()) {
        return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "ASOF right-key uniqueness validation was cancelled");
      }
      const bool duplicate =
          request.right_is_time_series_raw
              ? request.right_tie_break_row_uuids[left] ==
                    request.right_tie_break_row_uuids[right]
              : request.right_keys[left].metric_uuid ==
                        request.right_keys[right].metric_uuid &&
                    request.right_keys[left].canonical_tags ==
                        request.right_keys[right].canonical_tags &&
                    request.right_keys[left].timestamp_ns ==
                        request.right_keys[right].timestamp_ns;
      if (duplicate) {
        return refuse(
            "SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1",
            request.right_is_time_series_raw
                ? "ASOF raw-right row identity is not unique"
                : "ASOF right key is duplicate without a signed tie identity");
      }
    }
  }

  result.output_batch.columns.reserve(output_width);
  result.output_batch.columns.insert(result.output_batch.columns.end(),
                                     request.left_batch.columns.begin(),
                                     request.left_batch.columns.end());
  result.output_batch.columns.insert(result.output_batch.columns.end(),
                                     request.right_batch.columns.begin(),
                                     request.right_batch.columns.end());
  if (request.left_outer) {
    for (std::size_t ordinal = request.left_batch.columns.size();
         ordinal < result.output_batch.columns.size(); ++ordinal) {
      result.output_batch.columns[ordinal].nullable = true;
      if (!DeriveAsofNullableDescriptorEncoding(
              &result.output_batch.columns[ordinal].descriptor)) {
        return refuse(
            "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
            "ASOF LEFT result descriptor lacks an exact nullability carrier");
      }
    }
  }
  result.output_batch.rows.reserve(left_count);
  result.matched_right_ordinals.reserve(left_count);
  for (std::size_t left = 0; left < left_count; ++left) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "ASOF join was cancelled during matching", left);
    }
    std::int64_t match = -1;
    std::int64_t latest_timestamp = std::numeric_limits<std::int64_t>::min();
    for (std::size_t right = 0; right < right_count; ++right) {
      if (cancelled()) {
        return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "ASOF join was cancelled during candidate comparison",
                      left);
      }
      const auto& left_key = request.left_keys[left];
      const auto& right_key = request.right_keys[right];
      if (left_key.metric_uuid != right_key.metric_uuid ||
          left_key.canonical_tags != right_key.canonical_tags ||
          right_key.timestamp_ns > left_key.timestamp_ns) {
        continue;
      }
      const __int128 distance = static_cast<__int128>(left_key.timestamp_ns) -
                                right_key.timestamp_ns;
      if (distance > request.tolerance_ns ||
          right_key.timestamp_ns < latest_timestamp) {
        continue;
      }
      const bool smaller_raw_tie =
          request.right_is_time_series_raw && match >= 0 &&
          right_key.timestamp_ns == latest_timestamp &&
          request.right_tie_break_row_uuids[right] <
              request.right_tie_break_row_uuids[static_cast<std::size_t>(match)];
      if (right_key.timestamp_ns > latest_timestamp || match < 0 ||
          smaller_raw_tie) {
        latest_timestamp = right_key.timestamp_ns;
        match = static_cast<std::int64_t>(right);
      }
    }
    if (match < 0 && !request.left_outer) continue;
    DescriptorTuple joined;
    joined.values.reserve(output_width);
    joined.values.insert(joined.values.end(),
                         request.left_batch.rows[left].values.begin(),
                         request.left_batch.rows[left].values.end());
    if (match >= 0) {
      const auto& right_values =
          request.right_batch.rows[static_cast<std::size_t>(match)].values;
      joined.values.insert(joined.values.end(), right_values.begin(),
                           right_values.end());
    } else {
      for (std::size_t ordinal = request.left_batch.columns.size();
           ordinal < result.output_batch.columns.size(); ++ordinal) {
        internal_api::EngineTypedValue null_value;
        null_value.descriptor =
            result.output_batch.columns[ordinal].descriptor;
        null_value.setState(internal_api::EngineValueState::sql_null);
        joined.values.push_back(std::move(null_value));
      }
    }
    if (request.left_outer) {
      for (std::size_t ordinal = request.left_batch.columns.size();
           ordinal < result.output_batch.columns.size(); ++ordinal) {
        joined.values[ordinal].descriptor =
            result.output_batch.columns[ordinal].descriptor;
      }
    }
    result.output_batch.rows.push_back(std::move(joined));
    result.matched_right_ordinals.push_back(match);
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "ASOF join was cancelled before root publication");
  }
  if (result.output_batch.columns.size() !=
      root->output_descriptor_ids.size()) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                  "ASOF output descriptor width changed before publication");
  }
  for (std::size_t column = 0;
       column < result.output_batch.columns.size(); ++column) {
    if (cancelled()) {
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "ASOF output descriptor validation was cancelled");
    }
    const auto& bound = result.output_batch.columns[column];
    bool duplicate_descriptor_id = false;
    for (std::size_t prior = 0; prior < column; ++prior) {
      if (cancelled()) {
        return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "ASOF output descriptor validation was cancelled");
      }
      duplicate_descriptor_id =
          duplicate_descriptor_id ||
          result.output_batch.columns[prior].descriptor_id ==
              bound.descriptor_id;
    }
    if (duplicate_descriptor_id ||
        bound.descriptor_id != root->output_descriptor_ids[column] ||
        bound.descriptor_id == 0 || bound.stable_name.empty() ||
        !CanonicalUuid(bound.descriptor.descriptor_uuid.canonical) ||
        bound.descriptor.descriptor_kind != "scalar" ||
        bound.descriptor.canonical_type_name.empty() ||
        bound.descriptor.encoded_descriptor.empty()) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                    "ASOF output descriptor is unresolved");
    }
  }
  for (const auto& row : result.output_batch.rows) {
    if (row.values.size() != result.output_batch.columns.size()) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                    "ASOF output row width changed before publication");
    }
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      if (cancelled()) {
        return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "ASOF output cell validation was cancelled");
      }
      const auto& value = row.values[column];
      const auto& bound = result.output_batch.columns[column];
      const bool exact_descriptor =
          value.descriptor.descriptor_uuid.canonical ==
              bound.descriptor.descriptor_uuid.canonical &&
          value.descriptor.descriptor_kind ==
              bound.descriptor.descriptor_kind &&
          value.descriptor.canonical_type_name ==
              bound.descriptor.canonical_type_name &&
          value.descriptor.encoded_descriptor ==
              bound.descriptor.encoded_descriptor;
      const bool exact_value_state =
          value.state == internal_api::EngineValueState::sql_null
              ? value.is_null && value.encoded_value.empty() &&
                    value.binary_value.empty() && bound.nullable
              : value.state == internal_api::EngineValueState::value &&
                    !value.is_null;
      if (!exact_descriptor || !exact_value_state) {
        return refuse("QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
                      "ASOF output typed value is not canonical");
      }
    }
  }
  const auto publish_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!publish_authority.ok) {
    return refuse(publish_authority.diagnostic_code,
                  publish_authority.detail);
  }
  if (cancelled()) {
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "ASOF join was cancelled at final publication");
  }
  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = root->physical_node_id;
  result.causal_counter_id = root->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

}  // namespace scratchbird::engine::executor
