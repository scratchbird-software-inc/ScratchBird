// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_executor.hpp"
#include "temp_spill_executor.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <new>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
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

std::string RuntimeDerivedUuid(const std::string_view seed) {
  std::uint64_t high = 1469598103934665603ULL;
  std::uint64_t low = 1099511628211ULL;
  for (const auto ch : seed) {
    high = (high ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
    low = (low + static_cast<unsigned char>(ch)) * 1469598103934665603ULL;
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string raw(32, '0');
  for (std::size_t index = 0; index < 16; ++index) {
    raw[index] = kHex[(high >> ((15 - index) * 4)) & 0xf];
    raw[16 + index] = kHex[(low >> ((15 - index) * 4)) & 0xf];
  }
  raw[12] = '7';
  raw[16] = kHex[(static_cast<unsigned>(raw[16] <= '9'
                                            ? raw[16] - '0'
                                            : raw[16] - 'a' + 10) &
                  0x3) |
                 0x8];
  return raw.substr(0, 8) + "-" + raw.substr(8, 4) + "-" +
         raw.substr(12, 4) + "-" + raw.substr(16, 4) + "-" +
         raw.substr(20, 12);
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

class TempOperationCleanupGuard {
 public:
  void Arm(
      scratchbird::core::memory::TempWorkspaceLifecycleManager* manager,
      const std::string* operation_id) noexcept {
    manager_ = manager;
    operation_id_ = operation_id;
    armed_ = manager_ != nullptr && operation_id_ != nullptr;
  }

  void Disarm() noexcept { armed_ = false; }

  ~TempOperationCleanupGuard() noexcept {
    if (!armed_) return;
    try {
      manager_->CleanupOperation(*operation_id_);
    } catch (...) {
    }
  }

 private:
  scratchbird::core::memory::TempWorkspaceLifecycleManager* manager_ =
      nullptr;
  const std::string* operation_id_ = nullptr;
  bool armed_ = false;
};

bool ExactRcp080SortArtifactCeiling(const std::uint64_t maximum_rows,
                                    std::uint64_t* bytes) {
  if (bytes == nullptr || maximum_rows == 0) return false;
  // SBTWID15: 80-byte header, u64 count, then u32 key length + key,
  // u32 payload length + decimal payload, and u64 ordinal for each row.
  std::uint64_t digit_sum = 0;
  std::uint64_t range_begin = 0;
  std::uint64_t power = 10;
  for (std::uint64_t digits = 1; range_begin < maximum_rows; ++digits) {
    const auto range_end =
        power == 0 ? maximum_rows : std::min(maximum_rows, power);
    const auto count = range_end - range_begin;
    std::uint64_t range_digits = 0;
    if (!CheckedAsofMultiply(count, digits, &range_digits) ||
        !CheckedAsofAdd(range_digits, &digit_sum)) {
      return false;
    }
    range_begin = range_end;
    if (range_begin == maximum_rows) break;
    if (power > std::numeric_limits<std::uint64_t>::max() / 10) {
      power = 0;
    } else {
      power *= 10;
    }
  }
  std::uint64_t fixed_rows = 0;
  std::uint64_t two_digit_sums = 0;
  *bytes = 88;
  // Per row fixed bytes: two u32 lengths, u64 ordinal, and the eleven-byte
  // "rcp080.row." prefix. Key and payload each add the decimal ordinal.
  return CheckedAsofMultiply(maximum_rows, 27, &fixed_rows) &&
         CheckedAsofMultiply(digit_sum, 2, &two_digit_sums) &&
         CheckedAsofAdd(fixed_rows, bytes) &&
         CheckedAsofAdd(two_digit_sums, bytes);
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
  const bool relational_family = request.input.family_id == "relational";
  const bool graph_family = request.input.family_id == "graph";
  const bool key_value_family = request.input.family_id == "key_value";
  const bool time_series_family = request.input.family_id == "time_series";
  const bool vector_family = request.input.family_id == "vector";
  const bool search_family = request.input.family_id == "search";
  const bool spatial_family = request.input.family_id == "spatial";
  const bool columnar_family = request.input.family_id == "columnar";
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
      (request.capability.family_id != "relational" &&
       request.capability.family_id != "document" &&
       request.capability.family_id != "graph" &&
       request.capability.family_id != "key_value" &&
       request.capability.family_id != "time_series" &&
       request.capability.family_id != "vector" &&
       request.capability.family_id != "search" &&
       request.capability.family_id != "spatial" &&
       request.capability.family_id != "columnar") ||
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
      ((key_value_family || time_series_family || vector_family ||
        search_family || spatial_family || columnar_family) &&
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
    refuse(relational_family
               ? "SB_MODEL_RELATIONAL_EXACT_FALLBACK_UNAVAILABLE_V1"
               : graph_family
               ? "SB_MODEL_GRAPH_EXACT_FALLBACK_UNAVAILABLE_V1"
               : key_value_family
                     ? "SB_MODEL_KEY_VALUE_EXACT_FALLBACK_UNAVAILABLE_V1"
                     : time_series_family
                           ? "SB_MODEL_TIME_SERIES_EXACT_FALLBACK_UNAVAILABLE_V1"
                     : vector_family
                           ? "SB_MODEL_VECTOR_EXACT_FALLBACK_UNAVAILABLE_V1"
                     : search_family
                           ? "SB_MODEL_SEARCH_EXACT_FALLBACK_UNAVAILABLE_V1"
                     : spatial_family
                           ? "SB_MODEL_SPATIAL_EXACT_FALLBACK_UNAVAILABLE_V1"
                     : columnar_family
                           ? "SB_MODEL_COLUMNAR_EXACT_FALLBACK_UNAVAILABLE_V1"
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

  try {
    if (request.cancellation_requested()) {
      refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
             "model-family execution was cancelled before data access");
      cleanup_once();
      return result;
    }
  } catch (...) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family cancellation probe failed before data access");
    cleanup_once();
    return result;
  }

  if (request.fault_injected) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family provider leg failed before exchange");
    cleanup_once();
    return result;
  }
  ModelProviderExecutionResultV1 provider;
  try {
    result.provider_entered = true;
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
  ModelExchangeResultV1 exchange;
  try {
    exchange = PublishModelFamilyExchangeV1(
        request.input, provider.provider_batch,
        request.cancellation_requested);
  } catch (const std::bad_alloc&) {
    refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
           "model-family exchange allocation was refused");
    cleanup_once();
    return result;
  } catch (const std::length_error&) {
    refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
           "model-family exchange allocation length was refused");
    cleanup_once();
    return result;
  } catch (...) {
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family exchange publication threw");
    cleanup_once();
    return result;
  }
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

  result.output = std::move(exchange.output);
  if (!cleanup_once()) {
    result.output = {};
    result.accepted = false;
    result.root_published = false;
    return result;
  }
  try {
    if (request.cancellation_requested()) {
      result.output = {};
      refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
             "model-family execution was cancelled during cleanup");
      return result;
    }
  } catch (...) {
    result.output = {};
    refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
           "model-family cancellation probe failed after cleanup");
    return result;
  }
  result.accepted = true;
  result.root_published = exchange.root_publishable;
  return result;
}

ModelFamilyCompositionExecutionResultV1 ExecuteModelFamilyCompositionV1(
    const ModelFamilyCompositionExecutionRequestV1& request) {
  // QOW-SOURCE-RCP-080-COMPLETE-MULTIMODEL-EXECUTION-V1
  using scratchbird::core::memory::TempStorageClass;
  using scratchbird::core::memory::TempWorkspaceAllocationRequest;
  using scratchbird::core::memory::TempWorkspaceLifetime;
  ModelFamilyCompositionExecutionResultV1 result;
  result.rule_receipts = request.admitted_plan.rule_receipts;
  const auto receipt = [&](const std::string_view rule_id,
                           const bool complete) {
    for (auto& value : result.rule_receipts) {
      if (value.rule_id == rule_id) {
        value.complete = complete;
        return;
      }
    }
  };
  const auto cancelled = [&]() {
    try {
      return !request.cancellation_requested ||
             request.cancellation_requested();
    } catch (...) {
      return true;
    }
  };
  bool temp_reservation_started = false;
  bool temp_reservation_cleaned = false;
  bool temp_reservation_cleanup_attempted = false;
  bool temp_spill_artifact_cleaned = false;
  TempOperationCleanupGuard temp_reservation_guard;
  const auto cleanup_temp_once = [&]() noexcept {
    if (!temp_reservation_started || temp_reservation_cleaned) return true;
    if (temp_reservation_cleanup_attempted) return false;
    temp_reservation_cleanup_attempted = true;
    ++result.spill_cleanup_count;
    try {
      const auto cleanup = request.engine_temp_workspace->CleanupOperation(
          request.spill_owner.operation_id);
      temp_reservation_cleaned = cleanup.ok() && cleanup.cleaned_count == 1;
      if (temp_reservation_cleaned) temp_reservation_guard.Disarm();
      return temp_reservation_cleaned;
    } catch (...) {
      return false;
    }
  };
  std::size_t exchange_cleanup_cursor = 0;
  std::size_t consumer_cleanup_cursor = 0;
  const auto cleanup_exchanges_once = [&]() noexcept {
    bool complete = true;
    while (exchange_cleanup_cursor < result.started_exchange_ordinals.size()) {
      const auto ordinal =
          result.started_exchange_ordinals[exchange_cleanup_cursor++];
      ++result.exchange_cleanup_count;
      try {
        request.legs[ordinal].cleanup_exchange();
      } catch (...) {
        complete = false;
      }
    }
    return complete;
  };
  const auto cleanup_consumers_once = [&]() noexcept {
    bool complete = true;
    while (consumer_cleanup_cursor <
           result.started_relational_consumer_ids.size()) {
      const auto node_id = result.started_relational_consumer_ids[
          consumer_cleanup_cursor++];
      ++result.relational_consumer_cleanup_count;
      try {
        request.cleanup_relational_consumer(node_id);
      } catch (...) {
        complete = false;
      }
    }
    return complete;
  };
  const auto finish_cleanup = [&]() noexcept {
    const bool consumers = cleanup_consumers_once();
    const bool exchanges = cleanup_exchanges_once();
    const bool temp = cleanup_temp_once();
    result.total_cleanup_count =
        result.provider_cleanup_count + result.exchange_cleanup_count +
        result.relational_consumer_cleanup_count + result.spill_cleanup_count;
    result.cleanup_complete =
        consumers && exchanges && temp &&
        result.provider_cleanup_count == result.started_leg_ordinals.size();
    receipt("COORD-021-V1", result.cleanup_complete);
    return result.cleanup_complete;
  };
  const auto refuse = [&](std::string diagnostic, std::string detail) {
    result.accepted = false;
    result.root_published = false;
    result.no_partial_root = true;
    result.root_output_batch = {};
    result.root_publication_receipt_uuid.clear();
    result.diagnostic_id = std::move(diagnostic);
    result.detail = std::move(detail);
    finish_cleanup();
    if (!result.cleanup_complete) {
      result.diagnostic_id = "SB_MODEL_CLEANUP_INCOMPLETE_V1";
      result.detail = "composition cleanup cardinality is incomplete";
    }
    return result;
  };

  if (request.abi_version != 1 || !request.admitted_plan.accepted ||
      !request.admitted_plan.deterministic ||
      !request.admitted_plan.data_access_allowed ||
      !request.admitted_plan.root_publication_candidate ||
      !request.admitted_plan.no_partial_root ||
      !CanonicalUuid(request.admitted_plan.dependency_dag_receipt_uuid) ||
      !CanonicalUuid(
          request.admitted_plan.composition_admission_receipt_uuid) ||
      request.legs.size() != request.admitted_plan.stable_schedule.size() ||
      request.legs.size() < 3 || request.legs.size() > 9 ||
      !request.execute_relational_consumer ||
      !request.cleanup_relational_consumer ||
      !request.cancellation_requested ||
      !request.revalidate_publication_state ||
      request.current_selected_plan_generation == 0 ||
      !PhysicalMgaStatementContextValid(
          request.current_mga_statement_context)) {
    return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                  "composition executor did not receive one complete admitted dependency plan");
  }
  if (request.admitted_plan.relational_consumers.empty() ||
      std::ranges::any_of(
          request.admitted_plan.relational_consumers,
          [](const auto& consumer) {
            return consumer.input_physical_node_uuids.size() != 2;
          })) {
    return refuse(
        "SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
        "each composition consumer requires exactly two inputs and one root consumer");
  }
  std::vector<const scratchbird::engine::optimizer::ModelFamilyScheduledLegV1*>
      schedule_by_ordinal(request.legs.size(), nullptr);
  std::set<std::string> result_handle_uuids;
  for (const auto& scheduled : request.admitted_plan.stable_schedule) {
    if (scheduled.leg.lexical_source_ordinal >= schedule_by_ordinal.size() ||
        schedule_by_ordinal[scheduled.leg.lexical_source_ordinal] != nullptr) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "composition schedule ordinal is absent or duplicated");
    }
    schedule_by_ordinal[scheduled.leg.lexical_source_ordinal] = &scheduled;
  }
  for (std::size_t ordinal = 0; ordinal < request.legs.size(); ++ordinal) {
    const auto& leg = request.legs[ordinal];
    if (schedule_by_ordinal[ordinal] == nullptr) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "composition schedule is incomplete");
    }
    const auto& scheduled = *schedule_by_ordinal[ordinal];
    const auto& admitted = scheduled.leg;
    const auto& input = leg.execution.input;
    const auto& capability = leg.execution.capability;
    if (leg.lexical_source_ordinal != ordinal ||
        scheduled.leg.lexical_source_ordinal != ordinal ||
        scheduled.composition_admission_receipt_uuid !=
            request.admitted_plan.composition_admission_receipt_uuid ||
        scheduled.composition_arity != request.legs.size() ||
        !leg.pause_exchange || !leg.resume_exchange ||
        !leg.cleanup_exchange ||
        leg.execution.input.multimodel_composition_receipt_uuid !=
            scheduled.composition_admission_receipt_uuid ||
        leg.execution.input.multimodel_lexical_source_ordinal != ordinal ||
        leg.execution.input.multimodel_composition_arity !=
            request.legs.size() ||
        !leg.execution.input.multimodel_common_statement_context ||
        !PhysicalMgaStatementContextEqual(
            leg.execution.input.mga_statement_context,
            request.current_mga_statement_context) ||
        !PhysicalMgaStatementContextEqual(
            leg.execution.current_mga_statement_context,
            request.current_mga_statement_context)) {
      return refuse(kModelMgaContextMismatch,
                    "composition leg admission receipt, ordinal, arity, or common MGA context was substituted");
    }
    if (input.family_id != admitted.family_id ||
        input.operation_ids != admitted.operation_ids ||
        input.operation_id != admitted.operation_id ||
        input.object_uuid != admitted.bound_object_uuid ||
        input.physical_node_id != admitted.root_physical_node_id ||
        input.selected_alternative_uuid != admitted.selected_alternative_uuid ||
        input.provider_uuid != admitted.provider_uuid ||
        input.provider_generation != admitted.provider_generation ||
        input.capability_uuid != admitted.capability_uuid ||
        input.causal_counter_id != scheduled.causal_counter_id ||
        !CanonicalUuid(input.result_handle_uuid) ||
        !result_handle_uuids.insert(input.result_handle_uuid).second ||
        input.output_descriptor_ids != admitted.output_descriptor_ids ||
        input.catalog_epoch_uuid != admitted.catalog_snapshot_uuid ||
        input.security_context_uuid != admitted.security_context_uuid ||
        input.policy_snapshot_uuid != admitted.policy_snapshot_uuid ||
        input.resource_contract_uuid != admitted.resource_contract_uuid ||
        input.catalog_generation != admitted.catalog_generation ||
        input.descriptor_generation != admitted.descriptor_generation ||
        input.security_generation != admitted.security_generation ||
        input.policy_generation != admitted.policy_generation ||
        input.resource_generation != admitted.resource_generation ||
        input.maximum_rows != admitted.maximum_rows ||
        input.maximum_cells != admitted.maximum_cells ||
        input.maximum_memory_bytes != admitted.memory_grant_bytes ||
        leg.execution.current_catalog_generation !=
            admitted.current_catalog_generation ||
        leg.execution.current_descriptor_generation !=
            admitted.current_descriptor_generation ||
        leg.execution.current_security_generation !=
            admitted.current_security_generation ||
        leg.execution.current_policy_generation !=
            admitted.current_policy_generation ||
        leg.execution.current_resource_generation !=
            admitted.current_resource_generation ||
        leg.execution.current_provider_generation !=
            admitted.current_provider_generation ||
        leg.execution.current_capability_generation !=
            admitted.current_capability_generation ||
        capability.capability_uuid != admitted.capability_uuid ||
        capability.family_id != admitted.family_id ||
        capability.provider_uuid != admitted.provider_uuid ||
        capability.provider_generation != admitted.provider_generation ||
        capability.capability_generation != admitted.capability_generation ||
        capability.abi_version != admitted.capability_abi_version ||
        capability.local_scope != admitted.local_scope ||
        capability.available != admitted.capability_admitted ||
        capability.exact != admitted.exact ||
        capability.exact_collection_fallback_available !=
            admitted.exact_fallback_available ||
        capability.cancellation_supported != admitted.cancellation_supported ||
        capability.cleanup_supported != admitted.cleanup_supported ||
        leg.execution.exact_fallback_selected !=
            admitted.exact_fallback_selected ||
        leg.execution.security_admitted != admitted.security_admitted) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "composition runtime leg identity, capability, generation, descriptor, or bound was substituted");
    }
  }
  if (request.current_selected_plan_generation !=
      request.admitted_plan.selected_plan_generation) {
    return refuse("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                  "composition selected-plan generation changed before access");
  }
  if (request.backpressure_low_watermark_rows == 0 ||
      request.backpressure_high_watermark_rows <=
          request.backpressure_low_watermark_rows) {
    return refuse("SB_MODEL_BACKPRESSURE_PROTOCOL_FAILED_V1",
                  "composition executor high/low watermarks are invalid");
  }
  if (cancelled()) {
    result.cancellation_fanout_complete = true;
    receipt("COORD-019-V1", true);
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "composition was cancelled before any data access");
  }

  std::filesystem::path spill_directory;
  if (request.admitted_plan.spill_reservation_required) {
    std::uint64_t maximum_spill_rows = 0;
    for (const auto& scheduled : request.admitted_plan.stable_schedule) {
      if (!CheckedAsofAdd(scheduled.leg.maximum_rows,
                          &maximum_spill_rows)) {
        return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                      "spill row ceiling accounting overflowed");
      }
    }
    if (maximum_spill_rows != 0 &&
        maximum_spill_rows - 1 >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
      return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                    "spill row ordinal exceeds the signed payload domain");
    }
    std::uint64_t exact_artifact_byte_ceiling = 0;
    if (!ExactRcp080SortArtifactCeiling(maximum_spill_rows,
                                        &exact_artifact_byte_ceiling) ||
        exact_artifact_byte_ceiling >
            request.admitted_plan.admitted_spill_bytes) {
      return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                    "exact SBTWID15 artifact bound exceeds the admitted spill byte ceiling");
    }
    const bool exact_spill_authority = std::ranges::all_of(
        request.admitted_plan.stable_schedule, [&](const auto& scheduled) {
          const auto& leg = scheduled.leg;
          return leg.resource_contract_uuid ==
                     request.spill_resource_contract_uuid &&
                 leg.current_resource_contract_uuid ==
                     request.spill_resource_contract_uuid &&
                 leg.policy_generation == request.spill_owner.policy_generation &&
                 leg.current_policy_generation ==
                     request.spill_owner.policy_generation &&
                 leg.security_generation ==
                     request.spill_owner.security_generation &&
                 leg.current_security_generation ==
                     request.spill_owner.security_generation;
        });
    if (request.engine_temp_workspace == nullptr ||
        !CanonicalUuid(request.spill_operation_uuid) ||
        !CanonicalUuid(request.spill_resource_contract_uuid) ||
        request.spill_owner.statement_id !=
            request.current_mga_statement_context.statement_uuid ||
        request.spill_owner.transaction_id !=
            request.current_mga_statement_context.owning_transaction_uuid ||
        request.spill_owner.snapshot_boundary !=
            request.current_mga_statement_context.statement_snapshot_uuid ||
        request.spill_owner.metadata_boundary !=
            request.current_mga_statement_context
                .statement_metadata_snapshot_uuid ||
        request.spill_owner.operation_id != request.spill_operation_uuid ||
        request.spill_owner.resource_budget_reference !=
            request.spill_resource_contract_uuid ||
        request.spill_owner.policy_generation == 0 ||
        request.spill_owner.security_generation == 0 ||
        !exact_spill_authority ||
        request.admitted_plan.admitted_spill_bytes == 0) {
      return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                    "engine temp workspace owner or exact spill grant is unbound");
    }
    TempWorkspaceAllocationRequest reservation;
    reservation.storage_class = TempStorageClass::spill_file;
    reservation.lifetime = TempWorkspaceLifetime::operation_lifetime;
    reservation.owner = request.spill_owner;
    reservation.bytes = request.admitted_plan.admitted_spill_bytes;
    reservation.purpose = "rcp080.multimodel.bounded.spill.v1";
    temp_reservation_started = true;
    temp_reservation_guard.Arm(request.engine_temp_workspace,
                               &request.spill_owner.operation_id);
    scratchbird::core::memory::TempWorkspaceResult reserved;
    try {
      reserved =
          request.engine_temp_workspace->ReserveTempFilespace(reservation);
    } catch (const std::bad_alloc&) {
      return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                    "temp spill reservation allocation was refused");
    } catch (...) {
      return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                    "temp spill reservation threw");
    }
    if (!reserved.ok() || !reserved.record.has_value() ||
        reserved.record->owner.statement_id != reservation.owner.statement_id ||
        reserved.record->owner.operation_id != reservation.owner.operation_id ||
        reserved.record->owner.resource_budget_reference !=
            reservation.owner.resource_budget_reference ||
        reserved.record->reserved_bytes != reservation.bytes ||
        reserved.record->path.empty() ||
        reserved.record->disk_reservation_evidence.requested_bytes !=
            reservation.bytes ||
        !reserved.record->disk_reservation_evidence.logical_quota_reserved ||
        (!reserved.record->disk_reservation_evidence.sparse_file_created &&
         !reserved.record->disk_reservation_evidence
              .physical_preallocation_complete)) {
      return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                    "engine temp workspace refused the exact byte reservation");
    }
    result.spill_reserved = true;
    result.spill_reserved_bytes = reserved.record->reserved_bytes;
    spill_directory = reserved.record->path.parent_path();
  }

  std::map<std::string, DescriptorBatch> node_outputs;
  std::vector<std::uint64_t> received_row_ordinals;
  std::uint64_t next_received_ordinal = 0;
  bool lateral_first_consumer_executed = false;
  std::set<std::uint64_t> consumer_causal_counters;
  std::uint64_t prior_consumer_causal_counter = 0;
  result.execution_started = true;
  const auto fanout_cancel = std::make_shared<std::atomic_bool>(false);
  const bool lateral_composition = std::ranges::any_of(
      request.admitted_plan.dependency_edges, [](const auto& edge) {
        return edge.producer_lexical_source_ordinal == 0 &&
               edge.consumer_lexical_source_ordinal == 1 &&
               edge.edge_kind == "correlation";
      });
  for (const auto& wave : request.admitted_plan.parallel_waves) {
    const bool lateral_right_wave =
        lateral_composition &&
        wave.size() == 1 && wave.front() == 1;
    if (lateral_right_wave) {
      const auto outer = node_outputs.find(
          schedule_by_ordinal[0]->leg.physical_node_uuid);
      const auto first_consumer = std::ranges::find_if(
          request.admitted_plan.relational_consumers,
          [&](const auto& consumer) {
            return consumer.input_physical_node_uuids.size() == 2 &&
                   consumer.input_physical_node_uuids[0] ==
                       schedule_by_ordinal[0]->leg.physical_node_uuid &&
                   consumer.input_physical_node_uuids[1] ==
                       schedule_by_ordinal[1]->leg.physical_node_uuid;
          });
      if (outer == node_outputs.end() ||
          !request.legs[1].execute_correlated_provider ||
          first_consumer == request.admitted_plan.relational_consumers.end()) {
        return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                      "lateral correlation input, consumer, or execution callback is absent");
      }
      result.visible_dependency_rows = outer->second.rows.size();
      if (outer->second.rows.empty()) {
        for (std::uint16_t ordinal = 0; ordinal < request.legs.size();
             ++ordinal) {
          if (std::ranges::find(result.launched_leg_ordinals, ordinal) ==
                  result.launched_leg_ordinals.end() &&
              std::ranges::find(result.unstarted_leg_ordinals, ordinal) ==
                  result.unstarted_leg_ordinals.end()) {
            result.unstarted_leg_ordinals.push_back(ordinal);
          }
        }
        receipt("COORD-018-V1", true);
        break;
      }
      const auto& right_bound = schedule_by_ordinal[1]->leg;
      DescriptorBatch correlated_output;
      bool have_correlated_output = false;
      std::uint64_t correlated_row_count = 0;
      std::uint64_t correlated_cells = 0;
      std::uint64_t correlated_memory = sizeof(DescriptorBatch);
      for (std::size_t invocation = 0;
           invocation < outer->second.rows.size(); ++invocation) {
        result.launched_leg_ordinals.push_back(1);
        auto execution = request.legs[1].execution;
        const auto original_probe = execution.cancellation_requested;
        const auto composition_probe = request.cancellation_requested;
        execution.cancellation_requested =
            [fanout_cancel, original_probe, composition_probe]() {
              if (fanout_cancel->load(std::memory_order_acquire)) return true;
              try {
                return !original_probe || original_probe() ||
                       !composition_probe || composition_probe();
              } catch (...) {
                return true;
              }
            };
        ModelFamilyExecutionResultV1 executed;
        try {
          executed = request.legs[1].execute_correlated_provider(
              execution, outer->second.rows[invocation], invocation);
        } catch (...) {
          executed.diagnostic_id = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          executed.detail = "a correlated model-family leg threw";
        }
        ++result.right_evaluation_count;
        if (executed.execution_started) {
          result.started_leg_ordinals.push_back(1);
        } else {
          result.unstarted_leg_ordinals.push_back(1);
        }
        if ((executed.provider_entered &&
             result.provider_entry_count ==
                 std::numeric_limits<std::uint64_t>::max()) ||
            (executed.data_access_observed &&
             result.observed_data_access_count ==
                 std::numeric_limits<std::uint64_t>::max())) {
          return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                        "composition provider observation count overflowed");
        }
        result.provider_entry_count += executed.provider_entered ? 1 : 0;
        result.observed_data_access_count +=
            executed.data_access_observed ? 1 : 0;
        result.provider_cleanup_count += executed.cleanup_count;
        result.leg_terminal_diagnostic_ids.push_back(executed.diagnostic_id);
        if (!executed.accepted || !executed.root_published ||
            !executed.cleanup_complete || executed.cleanup_count != 1) {
          if (executed.diagnostic_id == "SB_MODEL_EXECUTION_CANCELLED_V1") {
            result.cancelled_leg_ordinals.push_back(1);
          } else {
            result.failed_leg_ordinals.push_back(1);
            result.failure_frozen = true;
            receipt("COORD-020-V1", true);
          }
          result.cancellation_fanout_complete = true;
          receipt("COORD-019-V1", true);
          return refuse(executed.diagnostic_id.empty()
                            ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                            : executed.diagnostic_id,
                        executed.detail);
        }
        result.completed_leg_ordinals.push_back(1);
        result.started_exchange_ordinals.push_back(1);
        std::uint64_t batch_cells = 0;
        if (executed.output.batch.columns.size() > right_bound.maximum_columns ||
            executed.output.batch.rows.size() > right_bound.maximum_rows ||
            executed.output.batch.columns.size() !=
                right_bound.output_descriptor_uuids.size() ||
            executed.output.operation_ids != right_bound.operation_ids ||
            executed.output.operation_id != right_bound.operation_id ||
            executed.output.properties.property_uuid !=
                right_bound.delivered_property_uuid) {
          return refuse(kModelTypedExchangeInvalid,
                        "correlated right exchange exceeded its admitted identity or bound");
        }
        for (std::size_t column = 0;
             column < executed.output.batch.columns.size(); ++column) {
          if (executed.output.batch.columns[column]
                  .descriptor.descriptor_uuid.canonical !=
              right_bound.output_descriptor_uuids[column]) {
            return refuse(kModelTypedExchangeInvalid,
                          "correlated right descriptor lineage changed");
          }
        }
        std::deque<std::uint64_t> exchange_queue;
        bool paused = false;
        for (const auto& row : executed.output.batch.rows) {
          if (row.values.size() != executed.output.batch.columns.size() ||
              std::numeric_limits<std::uint64_t>::max() - batch_cells <
                  row.values.size() ||
              std::numeric_limits<std::uint64_t>::max() -
                      result.cells_received <
                  row.values.size()) {
            return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                          "correlated right exchange row bound is invalid");
          }
          batch_cells += row.values.size();
          result.cells_received += row.values.size();
          exchange_queue.push_back(next_received_ordinal++);
          ++result.rows_received;
          if (!paused && exchange_queue.size() >=
                             request.backpressure_high_watermark_rows) {
            try {
              request.legs[1].pause_exchange();
            } catch (...) {
              return refuse("SB_MODEL_BACKPRESSURE_PROTOCOL_FAILED_V1",
                            "correlated exchange pause callback failed");
            }
            paused = true;
            ++result.pause_count;
          }
          while (paused && exchange_queue.size() >
                               request.backpressure_low_watermark_rows) {
            received_row_ordinals.push_back(exchange_queue.front());
            exchange_queue.pop_front();
          }
          if (paused && exchange_queue.size() ==
                            request.backpressure_low_watermark_rows) {
            try {
              request.legs[1].resume_exchange();
            } catch (...) {
              return refuse("SB_MODEL_BACKPRESSURE_PROTOCOL_FAILED_V1",
                            "correlated exchange resume callback failed");
            }
            paused = false;
            ++result.resume_count;
          }
        }
        if (batch_cells > right_bound.maximum_cells) {
          return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                        "correlated right exchange exceeded its cell bound");
        }
        while (!exchange_queue.empty()) {
          received_row_ordinals.push_back(exchange_queue.front());
          exchange_queue.pop_front();
        }
        if (paused) {
          try {
            request.legs[1].resume_exchange();
          } catch (...) {
            return refuse("SB_MODEL_BACKPRESSURE_PROTOCOL_FAILED_V1",
                          "correlated final exchange resume failed");
          }
          ++result.resume_count;
        }

        DescriptorBatch outer_row;
        outer_row.columns = outer->second.columns;
        outer_row.rows.push_back(outer->second.rows[invocation]);
        result.started_relational_consumer_ids.push_back(
            first_consumer->physical_node_id);
        ModelFamilyRelationalConsumerExecutionResultV1 consumed;
        try {
          consumed = request.execute_relational_consumer(
              *first_consumer, outer_row, executed.output.batch);
        } catch (...) {
          result.failure_frozen = true;
          receipt("COORD-020-V1", true);
          return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                        "correlated relational consumer threw");
        }
        if (!consumed.ok ||
            consumed.executed_physical_node_id !=
                first_consumer->physical_node_id ||
            consumed.causal_counter_id != first_consumer->causal_counter_id ||
            consumed.selected_implementation_uuid !=
                first_consumer->selected_implementation_uuid ||
            !PhysicalMgaStatementContextEqual(
                consumed.mga_statement_context,
                first_consumer->mga_statement_context) ||
            consumed.security_receipt_uuid !=
                first_consumer->expected_security_receipt_uuid) {
          result.failure_frozen = true;
          receipt("COORD-020-V1", true);
          return refuse(consumed.diagnostic_id.empty()
                            ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                            : consumed.diagnostic_id,
                        consumed.detail);
        }
        if (consumed.output_batch.columns.size() !=
            first_consumer->output_descriptor_uuids.size()) {
          return refuse(kModelTypedExchangeInvalid,
                        "correlated consumer descriptor cardinality changed");
        }
        if (have_correlated_output &&
            correlated_output.columns.size() !=
                consumed.output_batch.columns.size()) {
          return refuse(kModelTypedExchangeInvalid,
                        "correlated consumer descriptor cardinality changed");
        }
        std::uint64_t prospective_memory = correlated_memory;
        for (std::size_t column = 0;
             column < consumed.output_batch.columns.size(); ++column) {
          if (consumed.output_batch.columns[column]
                  .descriptor.descriptor_uuid.canonical !=
                  first_consumer->output_descriptor_uuids[column] ||
              (have_correlated_output &&
               correlated_output.columns[column]
                       .descriptor.descriptor_uuid.canonical !=
                   consumed.output_batch.columns[column]
                       .descriptor.descriptor_uuid.canonical)) {
            return refuse(kModelTypedExchangeInvalid,
                          "correlated consumer descriptor lineage changed");
          }
          if (!have_correlated_output) {
            const auto& descriptor = consumed.output_batch.columns[column];
            if (!CheckedAsofAdd(sizeof(ExecutorColumnDescriptor),
                                &prospective_memory) ||
                !CheckedAsofAdd(descriptor.stable_name.size(),
                                &prospective_memory) ||
                !CheckedAsofAdd(
                    descriptor.descriptor.descriptor_uuid.canonical.size(),
                    &prospective_memory) ||
                !CheckedAsofAdd(descriptor.descriptor.descriptor_kind.size(),
                                &prospective_memory) ||
                !CheckedAsofAdd(
                    descriptor.descriptor.canonical_type_name.size(),
                    &prospective_memory) ||
                !CheckedAsofAdd(
                    descriptor.descriptor.encoded_descriptor.size(),
                    &prospective_memory)) {
              return refuse(kModelTypedExchangeInvalid,
                            "correlated consumer descriptor bound overflowed");
            }
          }
        }
        const auto incoming_rows = static_cast<std::uint64_t>(
            consumed.output_batch.rows.size());
        if (consumed.output_batch.columns.size() >
                first_consumer->maximum_columns ||
            correlated_row_count > first_consumer->maximum_rows ||
            incoming_rows >
                first_consumer->maximum_rows - correlated_row_count) {
          return refuse(kModelTypedExchangeInvalid,
                        "correlated consumer exceeded its admitted row bound");
        }
        const auto prospective_row_count =
            correlated_row_count + incoming_rows;
        if (prospective_row_count >
                std::numeric_limits<std::size_t>::max() ||
            prospective_row_count > correlated_output.rows.max_size()) {
          return refuse(kModelTypedExchangeInvalid,
                        "correlated consumer row allocation bound overflowed");
        }
        std::uint64_t prospective_cells = correlated_cells;
        for (const auto& row : consumed.output_batch.rows) {
          if (row.values.size() != consumed.output_batch.columns.size() ||
              !CheckedAsofAdd(row.values.size(), &prospective_cells) ||
              !CheckedAsofAdd(sizeof(DescriptorTuple),
                              &prospective_memory)) {
            return refuse(kModelTypedExchangeInvalid,
                          "correlated consumer row bound overflowed");
          }
          for (const auto& value : row.values) {
            if (!CheckedAsofAdd(sizeof(internal_api::EngineTypedValue),
                                &prospective_memory) ||
                !CheckedAsofAdd(
                    value.descriptor.descriptor_uuid.canonical.size(),
                    &prospective_memory) ||
                !CheckedAsofAdd(value.descriptor.descriptor_kind.size(),
                                &prospective_memory) ||
                !CheckedAsofAdd(
                    value.descriptor.canonical_type_name.size(),
                    &prospective_memory) ||
                !CheckedAsofAdd(
                    value.descriptor.encoded_descriptor.size(),
                    &prospective_memory) ||
                !CheckedAsofAdd(value.encoded_value.size(),
                                &prospective_memory) ||
                !CheckedAsofAdd(value.binary_value.size(),
                                &prospective_memory)) {
              return refuse(kModelTypedExchangeInvalid,
                            "correlated consumer value bound overflowed");
            }
          }
        }
        if (prospective_cells > first_consumer->maximum_cells ||
            prospective_memory > first_consumer->memory_grant_bytes) {
          return refuse(kModelTypedExchangeInvalid,
                        "correlated consumer exceeded its admitted output bound");
        }
        try {
          if (!have_correlated_output) {
            correlated_output.columns = consumed.output_batch.columns;
          }
          correlated_output.rows.reserve(
              static_cast<std::size_t>(prospective_row_count));
          correlated_output.rows.insert(
              correlated_output.rows.end(),
              std::make_move_iterator(consumed.output_batch.rows.begin()),
              std::make_move_iterator(consumed.output_batch.rows.end()));
        } catch (const std::bad_alloc&) {
          return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                        "correlated consumer output allocation was refused");
        }
        have_correlated_output = true;
        correlated_row_count = prospective_row_count;
        correlated_cells = prospective_cells;
        correlated_memory = prospective_memory;
        if (cancelled()) {
          result.cancellation_fanout_complete = true;
          receipt("COORD-019-V1", true);
          return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                        "composition was cancelled after a lateral invocation");
        }
      }
      const auto correlated_validation = ValidateCanonicalDescriptorBatch(
          correlated_output, first_consumer->output_descriptor_ids);
      if (!correlated_validation.ok ||
          first_consumer->causal_counter_id <= prior_consumer_causal_counter ||
          !consumer_causal_counters.insert(first_consumer->causal_counter_id)
               .second) {
        return refuse(kModelTypedExchangeInvalid,
                      "correlated consumer output or causal receipt is invalid");
      }
      node_outputs.emplace(first_consumer->physical_node_uuid,
                           std::move(correlated_output));
      prior_consumer_causal_counter = first_consumer->causal_counter_id;
      lateral_first_consumer_executed = true;
      if (node_outputs.at(first_consumer->physical_node_uuid).rows.empty()) {
        for (std::uint16_t ordinal = 0; ordinal < request.legs.size();
             ++ordinal) {
          if (std::ranges::find(result.launched_leg_ordinals, ordinal) ==
                  result.launched_leg_ordinals.end() &&
              std::ranges::find(result.unstarted_leg_ordinals, ordinal) ==
                  result.unstarted_leg_ordinals.end()) {
            result.unstarted_leg_ordinals.push_back(ordinal);
          }
        }
        receipt("COORD-018-V1", true);
        break;
      }
      continue;
    }
    std::vector<std::future<ModelFamilyExecutionResultV1>> futures;
    std::vector<ModelFamilyExecutionResultV1> executions;
    std::vector<std::uint16_t> execution_ordinals;
    const auto execution_count = wave.size();
    executions.resize(execution_count);
    futures.reserve(execution_count);
    execution_ordinals.reserve(execution_count);
    for (std::size_t invocation = 0; invocation < execution_count;
         ++invocation) {
      const auto ordinal = wave[invocation];
      result.launched_leg_ordinals.push_back(ordinal);
      auto execution = request.legs[ordinal].execution;
      const auto original_probe = execution.cancellation_requested;
      const auto composition_probe = request.cancellation_requested;
      execution.cancellation_requested =
          [fanout_cancel, original_probe, composition_probe]() {
        if (fanout_cancel->load(std::memory_order_acquire)) return true;
        try {
          return !original_probe || original_probe() ||
                 !composition_probe || composition_probe();
        } catch (...) {
          return true;
        }
      };
      const auto run_execution =
          [execution = std::move(execution), fanout_cancel]() {
            try {
              auto completed = ExecuteModelFamilySourceV1(execution);
              if (!completed.accepted || !completed.root_published ||
                  !completed.cleanup_complete || completed.cleanup_count != 1) {
                fanout_cancel->store(true, std::memory_order_release);
              }
              return completed;
            } catch (...) {
              fanout_cancel->store(true, std::memory_order_release);
              throw;
            }
          };
      if (request.engine_mga_inventory_guard_owned_by_caller) {
        try {
          executions[invocation] = run_execution();
        } catch (...) {
          fanout_cancel->store(true, std::memory_order_release);
          executions[invocation].diagnostic_id =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          executions[invocation].detail =
              "a caller-thread MGA provider execution failed";
        }
      } else {
        futures.push_back(std::async(std::launch::async, run_execution));
      }
      execution_ordinals.push_back(ordinal);
    }
    std::string terminal_non_cancellation_diagnostic;
    std::string terminal_non_cancellation_detail;
    std::string terminal_cancellation_diagnostic;
    std::string terminal_cancellation_detail;
    for (std::size_t wave_ordinal = 0; wave_ordinal < execution_count;
         ++wave_ordinal) {
      if (!request.engine_mga_inventory_guard_owned_by_caller) {
        try {
          executions[wave_ordinal] = futures[wave_ordinal].get();
        } catch (...) {
          fanout_cancel->store(true, std::memory_order_release);
          executions[wave_ordinal].diagnostic_id =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          executions[wave_ordinal].detail =
              "a parallel model-family leg future failed";
        }
      }
      auto& executed = executions[wave_ordinal];
      if (executed.execution_started) {
        result.started_leg_ordinals.push_back(execution_ordinals[wave_ordinal]);
      } else {
        result.unstarted_leg_ordinals.push_back(execution_ordinals[wave_ordinal]);
      }
      if ((executed.provider_entered &&
           result.provider_entry_count ==
               std::numeric_limits<std::uint64_t>::max()) ||
          (executed.data_access_observed &&
           result.observed_data_access_count ==
               std::numeric_limits<std::uint64_t>::max())) {
        return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                      "composition provider observation count overflowed");
      }
      result.provider_entry_count += executed.provider_entered ? 1 : 0;
      result.observed_data_access_count +=
          executed.data_access_observed ? 1 : 0;
      result.provider_cleanup_count += executed.cleanup_count;
      result.leg_terminal_diagnostic_ids.push_back(executed.diagnostic_id);
      if (executed.diagnostic_id == "SB_MODEL_EXECUTION_CANCELLED_V1") {
        result.cancelled_leg_ordinals.push_back(execution_ordinals[wave_ordinal]);
      } else if (!executed.accepted || !executed.root_published ||
                 !executed.cleanup_complete || executed.cleanup_count != 1) {
        result.failed_leg_ordinals.push_back(execution_ordinals[wave_ordinal]);
      } else {
        result.completed_leg_ordinals.push_back(execution_ordinals[wave_ordinal]);
      }
      if (!executed.accepted || !executed.root_published ||
          !executed.cleanup_complete || executed.cleanup_count != 1) {
        fanout_cancel->store(true, std::memory_order_release);
        const bool cancellation =
            executed.diagnostic_id == "SB_MODEL_EXECUTION_CANCELLED_V1";
        auto& diagnostic = cancellation
            ? terminal_cancellation_diagnostic
            : terminal_non_cancellation_diagnostic;
        auto& detail = cancellation
            ? terminal_cancellation_detail
            : terminal_non_cancellation_detail;
        if (diagnostic.empty()) {
          diagnostic = executed.diagnostic_id.empty()
                           ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                           : executed.diagnostic_id;
          detail = executed.detail;
        }
      }
      if (request.inject_failure_after_first_parallel_leg &&
          wave_ordinal == 0) {
        fanout_cancel->store(true, std::memory_order_release);
        if (terminal_non_cancellation_diagnostic.empty()) {
          terminal_non_cancellation_diagnostic =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          terminal_non_cancellation_detail =
              "named injected parallel-leg failure froze the composition";
        }
      }
    }
    const auto& terminal_diagnostic =
        terminal_non_cancellation_diagnostic.empty()
            ? terminal_cancellation_diagnostic
            : terminal_non_cancellation_diagnostic;
    const auto& terminal_detail =
        terminal_non_cancellation_diagnostic.empty()
            ? terminal_cancellation_detail
            : terminal_non_cancellation_detail;
    if (!terminal_diagnostic.empty()) {
      result.failure_frozen =
          terminal_diagnostic != "SB_MODEL_EXECUTION_CANCELLED_V1";
      result.cancellation_fanout_complete = true;
      receipt("COORD-019-V1", true);
      receipt("COORD-020-V1", result.failure_frozen);
      return refuse(terminal_diagnostic, terminal_detail);
    }
    for (std::size_t wave_ordinal = 0; wave_ordinal < execution_count;
         ++wave_ordinal) {
      const auto ordinal = execution_ordinals[wave_ordinal];
      auto& executed = executions[wave_ordinal];
      result.started_exchange_ordinals.push_back(ordinal);
      const auto& bounds = schedule_by_ordinal[ordinal]->leg;
      std::uint64_t batch_cells = 0;
      if (executed.output.batch.columns.size() > bounds.maximum_columns ||
          executed.output.batch.rows.size() > bounds.maximum_rows) {
        return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                      "family exchange exceeded its admitted row or column bound");
      }
      for (const auto& row : executed.output.batch.rows) {
        if (row.values.size() != executed.output.batch.columns.size() ||
            row.values.size() > bounds.maximum_columns ||
            std::numeric_limits<std::uint64_t>::max() - batch_cells <
                row.values.size()) {
          return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                        "family exchange row width or cell count is invalid");
        }
        batch_cells += row.values.size();
      }
      if (batch_cells > bounds.maximum_cells) {
        return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                      "family exchange exceeded its admitted cell bound");
      }
      if (executed.output.operation_ids != bounds.operation_ids ||
          executed.output.operation_id != bounds.operation_id ||
          executed.output.properties.property_uuid !=
              bounds.delivered_property_uuid) {
        return refuse("SB_MODEL_PROPERTY_UNSATISFIED_V1",
                      "family exchange operation or delivered property differed from admission");
      }
      if (executed.output.batch.columns.size() !=
          bounds.output_descriptor_uuids.size()) {
        return refuse(kModelTypedExchangeInvalid,
                      "family exchange descriptor UUID lineage cardinality changed");
      }
      for (std::size_t column = 0;
           column < executed.output.batch.columns.size(); ++column) {
        if (executed.output.batch.columns[column]
                .descriptor.descriptor_uuid.canonical !=
            bounds.output_descriptor_uuids[column]) {
          return refuse(kModelTypedExchangeInvalid,
                        "family exchange descriptor UUID lineage was substituted");
        }
      }
      std::deque<std::uint64_t> exchange_queue;
      bool paused = false;
      for (std::size_t row = 0; row < executed.output.batch.rows.size(); ++row) {
        exchange_queue.push_back(next_received_ordinal++);
        ++result.rows_received;
        if (std::numeric_limits<std::uint64_t>::max() - result.cells_received <
            executed.output.batch.rows[row].values.size()) {
          return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                        "composition cumulative cell counter overflowed");
        }
        result.cells_received +=
            executed.output.batch.rows[row].values.size();
        if (exchange_queue.size() >=
                request.backpressure_high_watermark_rows &&
            !paused) {
          try {
            request.legs[ordinal].pause_exchange();
          } catch (...) {
            return refuse("SB_MODEL_BACKPRESSURE_PROTOCOL_FAILED_V1",
                          "exchange pause callback failed");
          }
          paused = true;
          ++result.pause_count;
        }
        while (paused && exchange_queue.size() >
                             request.backpressure_low_watermark_rows) {
          received_row_ordinals.push_back(exchange_queue.front());
          exchange_queue.pop_front();
        }
        if (paused && exchange_queue.size() ==
                          request.backpressure_low_watermark_rows) {
          try {
            request.legs[ordinal].resume_exchange();
          } catch (...) {
            return refuse("SB_MODEL_BACKPRESSURE_PROTOCOL_FAILED_V1",
                          "exchange resume callback failed");
          }
          paused = false;
          ++result.resume_count;
        }
      }
      while (!exchange_queue.empty()) {
        received_row_ordinals.push_back(exchange_queue.front());
        exchange_queue.pop_front();
      }
      if (paused) {
        try {
          request.legs[ordinal].resume_exchange();
        } catch (...) {
          return refuse("SB_MODEL_BACKPRESSURE_PROTOCOL_FAILED_V1",
                        "final exchange resume callback failed");
        }
        ++result.resume_count;
      }
      auto [node, inserted] = node_outputs.try_emplace(
          schedule_by_ordinal[ordinal]->leg.physical_node_uuid,
          DescriptorBatch{});
      if (inserted) node->second.columns = executed.output.batch.columns;
      if (node->second.columns.size() != executed.output.batch.columns.size()) {
        return refuse(kModelTypedExchangeInvalid,
                      "correlated lateral invocation descriptor lineage changed");
      }
      for (std::size_t column = 0; column < node->second.columns.size();
           ++column) {
        if (node->second.columns[column].stable_name !=
                executed.output.batch.columns[column].stable_name ||
            node->second.columns[column].descriptor.descriptor_uuid.canonical !=
                executed.output.batch.columns[column]
                    .descriptor.descriptor_uuid.canonical) {
          return refuse(kModelTypedExchangeInvalid,
                        "correlated lateral invocation descriptor lineage changed");
        }
      }
      node->second.rows.insert(node->second.rows.end(),
                               std::make_move_iterator(
                                   executed.output.batch.rows.begin()),
                               std::make_move_iterator(
                                   executed.output.batch.rows.end()));
      if (cancelled()) {
        result.cancellation_fanout_complete = true;
        receipt("COORD-019-V1", true);
        return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "composition was cancelled after a family/exchange node");
      }
    }
    bool empty_dependency = false;
    for (const auto ordinal : wave) {
      empty_dependency =
          empty_dependency ||
          node_outputs.at(schedule_by_ordinal[ordinal]
                              ->leg.physical_node_uuid)
              .rows.empty();
    }
    if (empty_dependency && wave.size() == 1 &&
        result.launched_leg_ordinals.size() < request.legs.size()) {
      for (std::uint16_t ordinal = 0; ordinal < request.legs.size(); ++ordinal) {
        if (std::ranges::find(result.launched_leg_ordinals, ordinal) ==
            result.launched_leg_ordinals.end()) {
          result.unstarted_leg_ordinals.push_back(ordinal);
        }
      }
      receipt("COORD-018-V1", true);
      break;
    }
  }
  if (received_row_ordinals.size() != result.rows_received ||
      !std::ranges::equal(received_row_ordinals,
                          std::views::iota(std::uint64_t{0},
                                           result.rows_received))) {
    return refuse("SB_MODEL_BACKPRESSURE_PROTOCOL_FAILED_V1",
                  "bounded exchange lost, duplicated, or reordered a row");
  }
  result.backpressure_complete = true;
  receipt("COORD-013-V1", true);
  receipt("COORD-015-V1", true);
  receipt("COORD-016-V1", true);
  receipt("COORD-017-V1", true);
  receipt("COORD-018-V1", true);

  if (request.admitted_plan.spill_reservation_required) {
    TempSpillRequest spill;
    spill.route_kind = TempSpillRouteKind::kSort;
    spill.route_label = "rcp080.multimodel." + request.spill_operation_uuid;
    spill.spill_directory = spill_directory;
    spill.runtime_generation = request.spill_runtime_generation;
    spill.memory_quota_bytes = 1;
    spill.authority.engine_mga_snapshot_bound = true;
    spill.authority.transaction_inventory_authoritative = true;
    spill.authority.security_recheck_required = true;
    spill.authority.security_context_bound = true;
    spill.authority.exact_recheck_required = true;
    constexpr auto kMaximumSignedSpillOrdinal =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max());
    if ((result.rows_received != 0 &&
         result.rows_received - 1 > kMaximumSignedSpillOrdinal) ||
        result.rows_received > std::numeric_limits<std::size_t>::max() ||
        result.rows_received > spill.rows.max_size()) {
      return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                    "spill row ordinal exceeds the signed payload domain");
    }
    TempSpillResult spilled;
    try {
      spill.rows.reserve(static_cast<std::size_t>(result.rows_received));
      for (std::uint64_t ordinal = 0; ordinal < result.rows_received;
           ++ordinal) {
        spill.rows.push_back({"rcp080.row." + std::to_string(ordinal),
                              static_cast<std::int64_t>(ordinal), ordinal});
      }
      spilled = ExecuteBoundedTempSpillRoute(spill);
    } catch (const std::bad_alloc&) {
      return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                    "bounded spill allocation was refused");
    } catch (...) {
      return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                    "bounded spill execution threw");
    }
    temp_spill_artifact_cleaned = spilled.cleanup_proven;
    if (temp_spill_artifact_cleaned) ++result.spill_cleanup_count;
    if (!spilled.ok || !spilled.runtime_consumed || !spilled.spilled ||
        !spilled.cleanup_proven || !spilled.reopen_recovery_proven ||
        spilled.output_rows.size() != result.rows_received) {
      return refuse("SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                    spilled.diagnostic_code + ":" + spilled.fallback_reason);
    }
    result.spill_io_complete = true;
    result.spill_result_hash = spilled.result_hash;
    receipt("COORD-012-V1", true);
  }

  for (const auto& consumer : request.admitted_plan.relational_consumers) {
    const bool completed_lateral_consumer =
        lateral_first_consumer_executed &&
        consumer.input_physical_node_uuids.size() == 2 &&
        consumer.input_physical_node_uuids[0] ==
            schedule_by_ordinal[0]->leg.physical_node_uuid &&
        consumer.input_physical_node_uuids[1] ==
            schedule_by_ordinal[1]->leg.physical_node_uuid;
    if (completed_lateral_consumer) continue;
    const auto left = node_outputs.find(consumer.input_physical_node_uuids[0]);
    const auto right = node_outputs.find(consumer.input_physical_node_uuids[1]);
    if (left == node_outputs.end() || right == node_outputs.end()) {
      if (std::ranges::any_of(
              node_outputs, [](const auto& entry) {
                return entry.second.rows.empty();
              })) {
        result.root_output_batch = {};
        break;
      }
      result.failure_frozen = true;
      receipt("COORD-020-V1", true);
      return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                    "typed relational consumer input is unresolved");
    }
    ModelFamilyRelationalConsumerExecutionResultV1 consumed;
    try {
      result.started_relational_consumer_ids.push_back(
          consumer.physical_node_id);
      consumed = request.execute_relational_consumer(consumer, left->second,
                                                     right->second);
    } catch (...) {
      result.failure_frozen = true;
      receipt("COORD-020-V1", true);
      return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                    "typed relational consumer threw");
    }
    if (!consumed.ok) {
      result.failure_frozen = true;
      receipt("COORD-020-V1", true);
      return refuse(consumed.diagnostic_id.empty()
                        ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                        : consumed.diagnostic_id,
                    consumed.detail);
    }
    const auto descriptor_validation = ValidateCanonicalDescriptorBatch(
        consumed.output_batch, consumer.output_descriptor_ids);
    std::uint64_t consumer_cells = 0;
    std::uint64_t consumer_memory = sizeof(DescriptorBatch);
    bool bounded_consumer = descriptor_validation.ok &&
        consumed.executed_physical_node_id == consumer.physical_node_id &&
        consumed.causal_counter_id == consumer.causal_counter_id &&
        consumed.selected_implementation_uuid ==
            consumer.selected_implementation_uuid &&
        consumed.causal_counter_id > prior_consumer_causal_counter &&
        consumer_causal_counters.insert(consumed.causal_counter_id).second &&
        PhysicalMgaStatementContextEqual(consumed.mga_statement_context,
                                         consumer.mga_statement_context) &&
        consumed.security_receipt_uuid ==
            consumer.expected_security_receipt_uuid &&
        consumed.output_batch.columns.size() <= consumer.maximum_columns &&
        consumed.output_batch.rows.size() <= consumer.maximum_rows &&
        consumed.output_batch.columns.size() ==
            consumer.output_descriptor_uuids.size();
    for (std::size_t column = 0;
         bounded_consumer && column < consumed.output_batch.columns.size();
         ++column) {
      const auto& descriptor = consumed.output_batch.columns[column];
      bounded_consumer =
          descriptor.descriptor.descriptor_uuid.canonical ==
              consumer.output_descriptor_uuids[column] &&
          CheckedAsofAdd(sizeof(ExecutorColumnDescriptor), &consumer_memory) &&
          CheckedAsofAdd(descriptor.stable_name.size(), &consumer_memory) &&
          CheckedAsofAdd(descriptor.descriptor.descriptor_uuid.canonical.size(),
              &consumer_memory) &&
          CheckedAsofAdd(descriptor.descriptor.descriptor_kind.size(), &consumer_memory) &&
          CheckedAsofAdd(descriptor.descriptor.canonical_type_name.size(),
              &consumer_memory) &&
          CheckedAsofAdd(descriptor.descriptor.encoded_descriptor.size(),
              &consumer_memory);
    }
    for (const auto& row : consumed.output_batch.rows) {
      if (!bounded_consumer ||
          row.values.size() != consumed.output_batch.columns.size() ||
          std::numeric_limits<std::uint64_t>::max() - consumer_cells <
              row.values.size() ||
          !CheckedAsofAdd(sizeof(DescriptorTuple), &consumer_memory)) {
        bounded_consumer = false;
        break;
      }
      consumer_cells += row.values.size();
      for (const auto& value : row.values) {
        if (!CheckedAsofAdd(sizeof(internal_api::EngineTypedValue), &consumer_memory) ||
            !CheckedAsofAdd(value.descriptor.descriptor_uuid.canonical.size(),
                 &consumer_memory) ||
            !CheckedAsofAdd(value.descriptor.descriptor_kind.size(), &consumer_memory) ||
            !CheckedAsofAdd(value.descriptor.canonical_type_name.size(),
                 &consumer_memory) ||
            !CheckedAsofAdd(value.descriptor.encoded_descriptor.size(),
                 &consumer_memory) ||
            !CheckedAsofAdd(value.encoded_value.size(), &consumer_memory) ||
            !CheckedAsofAdd(value.binary_value.size(), &consumer_memory)) {
          bounded_consumer = false;
          break;
        }
      }
    }
    bounded_consumer = bounded_consumer &&
        consumer_cells <= consumer.maximum_cells &&
        consumer_memory <= consumer.memory_grant_bytes;
    if (!bounded_consumer) {
      result.failure_frozen = true;
      receipt("COORD-020-V1", true);
      return refuse(kModelTypedExchangeInvalid,
                    "typed relational consumer output identity, causal receipt, or bound was substituted");
    }
    prior_consumer_causal_counter = consumed.causal_counter_id;
    if (cancelled()) {
      result.cancellation_fanout_complete = true;
      receipt("COORD-019-V1", true);
      return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                    "composition was cancelled after a relational consumer");
    }
    node_outputs.emplace(consumer.physical_node_uuid,
                         std::move(consumed.output_batch));
  }

  const auto root = node_outputs.find(
      request.admitted_plan.relational_consumers.back().physical_node_uuid);
  if (root == node_outputs.end()) {
    result.root_output_batch = {};
  } else {
    result.root_output_batch = root->second;
  }
  if (cancelled()) {
    receipt("COORD-019-V1", true);
    result.cancellation_fanout_complete = true;
    return refuse("SB_MODEL_EXECUTION_CANCELLED_V1",
                  "composition was cancelled before root publication");
  }
  ModelFamilyCompositionPublicationStateV1 publication;
  try {
    publication = request.revalidate_publication_state();
  } catch (...) {
    return refuse("SB_MODEL_ROOT_PUBLICATION_REFUSED_V1",
                  "final publication authority revalidation failed");
  }
  const auto exact_vector_sizes = [&](const auto& values) {
    return values.size() == request.legs.size();
  };
  bool publication_current =
      publication.current_selected_plan_generation ==
          request.admitted_plan.selected_plan_generation &&
      publication.security_admitted &&
      PhysicalMgaStatementContextEqual(
          publication.current_mga_statement_context,
          request.current_mga_statement_context) &&
      exact_vector_sizes(publication.current_catalog_generations) &&
      exact_vector_sizes(publication.current_descriptor_generations) &&
      exact_vector_sizes(publication.current_security_generations) &&
      exact_vector_sizes(publication.current_policy_generations) &&
      exact_vector_sizes(publication.current_resource_generations) &&
      exact_vector_sizes(publication.current_provider_generations) &&
      exact_vector_sizes(publication.current_capability_generations) &&
      exact_vector_sizes(publication.current_catalog_snapshot_uuids) &&
      exact_vector_sizes(publication.current_descriptor_snapshot_uuids) &&
      exact_vector_sizes(publication.current_security_context_uuids) &&
      exact_vector_sizes(publication.current_policy_snapshot_uuids) &&
      exact_vector_sizes(publication.current_resource_contract_uuids) &&
      exact_vector_sizes(publication.current_provider_uuids) &&
      exact_vector_sizes(publication.current_capability_uuids);
  for (std::size_t ordinal = 0;
       publication_current && ordinal < request.legs.size(); ++ordinal) {
    const auto& leg = schedule_by_ordinal[ordinal]->leg;
    publication_current =
        publication.current_catalog_generations[ordinal] ==
            leg.current_catalog_generation &&
        publication.current_descriptor_generations[ordinal] ==
            leg.current_descriptor_generation &&
        publication.current_security_generations[ordinal] ==
            leg.current_security_generation &&
        publication.current_policy_generations[ordinal] ==
            leg.current_policy_generation &&
        publication.current_resource_generations[ordinal] ==
            leg.current_resource_generation &&
        publication.current_provider_generations[ordinal] ==
            leg.current_provider_generation &&
        publication.current_capability_generations[ordinal] ==
            leg.current_capability_generation &&
        publication.current_catalog_snapshot_uuids[ordinal] ==
            leg.current_catalog_snapshot_uuid &&
        publication.current_descriptor_snapshot_uuids[ordinal] ==
            leg.current_descriptor_snapshot_uuid &&
        publication.current_security_context_uuids[ordinal] ==
            leg.current_security_context_uuid &&
        publication.current_policy_snapshot_uuids[ordinal] ==
            leg.current_policy_snapshot_uuid &&
        publication.current_resource_contract_uuids[ordinal] ==
            leg.current_resource_contract_uuid &&
        publication.current_provider_uuids[ordinal] == leg.provider_uuid &&
        publication.current_capability_uuids[ordinal] == leg.capability_uuid &&
        PhysicalMgaStatementContextEqual(leg.mga_statement_context,
                                         request.current_mga_statement_context);
  }
  if (!publication_current) {
    return refuse("SB_MODEL_ROOT_PUBLICATION_REFUSED_V1",
                  "final generation, UUID snapshot, MGA, security, descriptor, provider, or capability identity drifted");
  }
  result.rows_published = result.root_output_batch.rows.size();
  result.root_publication_receipt_uuid = RuntimeDerivedUuid(
      request.admitted_plan.dependency_dag_receipt_uuid + "|root|" +
      std::to_string(result.rows_published));
  result.root_published = true;
  result.accepted = true;
  result.no_partial_root = true;
  receipt("COORD-019-V1", true);
  receipt("COORD-020-V1", true);
  receipt("COORD-022-V1", true);
  if (!finish_cleanup()) {
    result.accepted = false;
    result.root_published = false;
    result.root_output_batch = {};
    result.root_publication_receipt_uuid.clear();
    result.diagnostic_id = "SB_MODEL_CLEANUP_INCOMPLETE_V1";
    result.detail = "composition cleanup cardinality is incomplete";
    return result;
  }
  const std::uint64_t exact_started_cleanup_count =
      static_cast<std::uint64_t>(result.started_leg_ordinals.size()) +
      static_cast<std::uint64_t>(result.started_exchange_ordinals.size()) +
      static_cast<std::uint64_t>(
          result.started_relational_consumer_ids.size()) +
      (request.admitted_plan.spill_reservation_required ? 2 : 0);
  std::uint64_t admitted_cleanup_ceiling =
      request.admitted_plan.expected_cleanup_component_count;
  std::uint64_t repeated_lateral_cleanup_count = 0;
  const bool bounded_lateral_cleanup =
      result.right_evaluation_count == 0 ||
      (CheckedAsofMultiply(result.right_evaluation_count - 1, 3,
                           &repeated_lateral_cleanup_count) &&
       CheckedAsofAdd(repeated_lateral_cleanup_count,
                      &admitted_cleanup_ceiling));
  if (!bounded_lateral_cleanup ||
      result.total_cleanup_count != exact_started_cleanup_count ||
      result.total_cleanup_count > admitted_cleanup_ceiling) {
    result.accepted = false;
    result.root_published = false;
    result.root_output_batch = {};
    result.root_publication_receipt_uuid.clear();
    result.cleanup_complete = false;
    receipt("COORD-021-V1", false);
    result.diagnostic_id = "SB_MODEL_CLEANUP_INCOMPLETE_V1";
    result.detail = "composition cleanup count differs from admitted plan";
    return result;
  }
  result.diagnostic_id = "SB_EXECUTOR_OK";
  return result;
}

CanonicalTimeSeriesAsofJoinResultV1 ExecuteCanonicalTimeSeriesAsofJoinV1(
    const CanonicalTimeSeriesAsofJoinRequestV1& request) {
  enum class CancellationProbeState : std::uint8_t {
    kRunning = 0,
    kCancelled,
    kProbeFailed,
  };
  CanonicalTimeSeriesAsofJoinResultV1 result;
  auto cancellation_state = CancellationProbeState::kRunning;
  const auto refuse = [&](std::string code, std::string detail,
                          const std::size_t row = 0) {
    if (cancellation_state == CancellationProbeState::kProbeFailed) {
      code = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
      detail = "ASOF cancellation probe failed";
    }
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    result.diagnostic.row_index = row;
    result.output_batch = {};
    result.matched_right_ordinals.clear();
    return result;
  };
  const std::function<bool()> guarded_cancellation_requested = [&]() noexcept {
    if (cancellation_state != CancellationProbeState::kRunning) return true;
    try {
      if (!request.cancellation_requested ||
          !request.cancellation_requested()) {
        return false;
      }
      cancellation_state = CancellationProbeState::kCancelled;
    } catch (...) {
      cancellation_state = CancellationProbeState::kProbeFailed;
    }
    return true;
  };
  const auto cancelled = [&]() {
    return guarded_cancellation_requested();
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
  if (left_node->output_descriptor_ids.size() >
          std::numeric_limits<std::size_t>::max() -
              right_node->output_descriptor_ids.size()) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "ASOF output descriptor width overflowed");
  }
  const auto expected_output_width =
      left_node->output_descriptor_ids.size() +
      right_node->output_descriptor_ids.size();
  if (root->output_descriptor_ids.size() != expected_output_width ||
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
  if ((!left_time_series && !right_time_series) ||
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
              keys[ordinal].canonical_tags, guarded_cancellation_requested,
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
    if (cancellation_state != CancellationProbeState::kRunning) {
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
  if (right_count > static_cast<std::size_t>(
                        std::numeric_limits<std::int64_t>::max())) {
    return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                  "ASOF right cardinality exceeds the result ordinal width");
  }
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
