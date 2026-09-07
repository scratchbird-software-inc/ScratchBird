// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_runtime_memory_support.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_RUNTIME_MEMORY_SUPPORT_AUTHORITY
bool CheckedAdd(const std::uint64_t left, const std::uint64_t right,
                std::uint64_t* result) {
  if (result == nullptr ||
      right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

bool CheckedMultiply(const std::uint64_t left, const std::uint64_t right,
                     std::uint64_t* result) {
  if (result == nullptr ||
      (left != 0 &&
       right > std::numeric_limits<std::uint64_t>::max() / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

bool RuntimeMaterializedBatchMemoryBytes(
    const exec::DescriptorBatch& batch,
    std::uint64_t* bytes,
    const exec::DescriptorCancellationProbe cancellation_requested,
    const void* cancellation_context,
    bool* cancellation_observed,
    bool* cancellation_probe_failed) {
  if (bytes == nullptr) return false;
  if (cancellation_observed != nullptr) *cancellation_observed = false;
  if (cancellation_probe_failed != nullptr) *cancellation_probe_failed = false;
  const auto stopped = [&] {
    if (cancellation_requested == nullptr) return false;
    try {
      if (!cancellation_requested(cancellation_context)) return false;
      if (cancellation_observed != nullptr) *cancellation_observed = true;
      return true;
    } catch (...) {
      if (cancellation_probe_failed != nullptr) {
        *cancellation_probe_failed = true;
      }
      return true;
    }
  };
  *bytes = 1;
  for (const auto& row : batch.rows) {
    if (stopped()) return false;
    for (const auto& value : row.values) {
      if (stopped()) return false;
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.binary_value.size();
    }
  }
  return true;
}

bool RuntimeTypedValueMemoryBytes(const api::EngineTypedValue& value,
                                  std::uint64_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 1;
  const std::array<std::size_t, 6> payloads{
      value.descriptor.descriptor_uuid.canonical.size(),
      value.descriptor.descriptor_kind.size(),
      value.descriptor.canonical_type_name.size(),
      value.descriptor.encoded_descriptor.size(), value.encoded_value.size(),
      value.binary_value.size()};
  for (const auto payload : payloads) {
    if (payload > std::numeric_limits<std::uint64_t>::max() - *bytes) {
      return false;
    }
    *bytes += payload;
  }
  return true;
}

void PublishRuntimeMemoryObservation(
    exec::CanonicalPhysicalDispatchStepResult* step,
    const std::uint64_t current_memory_bytes,
    const std::uint64_t peak_memory_bytes) {
  if (step == nullptr) return;
  const auto observed = [](const std::uint64_t value) {
    return exec::CanonicalObservedUint64{
        exec::CanonicalRuntimeMetricState::kObserved, value};
  };
  const auto not_applicable = [] {
    return exec::CanonicalObservedUint64{
        exec::CanonicalRuntimeMetricState::kNotApplicable, 0};
  };
  auto& receipt = step->runtime_observation;
  receipt.abi_version = 1;
  receipt.operator_wait_ns = observed(0);
  receipt.current_memory_bytes = observed(current_memory_bytes);
  receipt.peak_memory_bytes = observed(peak_memory_bytes);
  receipt.decoded_bytes = not_applicable();
  receipt.bytes_read = not_applicable();
  receipt.bytes_written = not_applicable();
  receipt.pages_read = not_applicable();
  receipt.pages_written = not_applicable();
  receipt.spill_bytes_read = observed(0);
  receipt.spill_bytes_written = observed(0);
  receipt.visibility_recheck_count = not_applicable();
  receipt.security_recheck_count = not_applicable();
  receipt.storage_recheck_count = not_applicable();
  receipt.index_recheck_count = not_applicable();
  receipt.residual_recheck_count = not_applicable();
  receipt.compatibility_recheck_count = not_applicable();
  receipt.archive_bytes_read = not_applicable();
  receipt.cluster_bytes_sent = not_applicable();
  receipt.cluster_bytes_received = not_applicable();
  receipt.authority.engine_execution_observation = true;
  receipt.producer_receipt_complete = true;
}

bool AddBatchMemoryBytes(
    const exec::DescriptorBatch& batch,
    std::uint64_t* memory_bytes,
    const exec::DescriptorCancellationProbe cancellation_requested,
    const void* cancellation_context,
    bool* cancellation_observed,
    bool* cancellation_probe_failed) {
  if (memory_bytes == nullptr) return false;
  if (cancellation_observed != nullptr) *cancellation_observed = false;
  if (cancellation_probe_failed != nullptr) *cancellation_probe_failed = false;
  const auto stopped = [&] {
    if (cancellation_requested == nullptr) return false;
    try {
      if (!cancellation_requested(cancellation_context)) return false;
      if (cancellation_observed != nullptr) *cancellation_observed = true;
      return true;
    } catch (...) {
      if (cancellation_probe_failed != nullptr) {
        *cancellation_probe_failed = true;
      }
      return true;
    }
  };
  const auto add = [&](const std::uint64_t amount) {
    return CheckedAdd(*memory_bytes, amount, memory_bytes);
  };
  const auto add_array = [&](const std::size_t count,
                             const std::size_t element_size) {
    std::uint64_t amount = 0;
    return CheckedMultiply(count, element_size, &amount) && add(amount);
  };
  const auto add_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           add(static_cast<std::uint64_t>(value.capacity()) + 1);
  };
  const auto add_descriptor = [&](const api::EngineDescriptor& descriptor) {
    return add_string(descriptor.descriptor_uuid.canonical) &&
           add_string(descriptor.descriptor_kind) &&
           add_string(descriptor.canonical_type_name) &&
           add_string(descriptor.encoded_descriptor);
  };
  if (!add(sizeof(batch)) ||
      !add_array(batch.columns.capacity(),
                 sizeof(exec::ExecutorColumnDescriptor)) ||
      !add_array(batch.rows.capacity(), sizeof(exec::DescriptorTuple))) {
    return false;
  }
  for (const auto& column : batch.columns) {
    if (stopped() || !add_string(column.stable_name) ||
        !add_descriptor(column.descriptor)) {
      return false;
    }
  }
  for (const auto& row : batch.rows) {
    if (stopped() ||
        !add_array(row.values.capacity(), sizeof(api::EngineTypedValue))) {
      return false;
    }
    for (const auto& value : row.values) {
      if (stopped() || !add_descriptor(value.descriptor) ||
          !add_string(value.encoded_value) ||
          !add(static_cast<std::uint64_t>(value.binary_value.capacity()))) {
        return false;
      }
    }
  }
  return true;
}

bool BoundDescriptorBatchLiveMemoryBytes(const exec::DescriptorBatch& batch,
                                         std::uint64_t* memory_bytes) {
  if (memory_bytes == nullptr) return false;
  *memory_bytes = sizeof(batch);
  const auto add = [&](const std::uint64_t amount) {
    return CheckedAdd(*memory_bytes, amount, memory_bytes);
  };
  const auto add_array = [&](const std::size_t count,
                             const std::size_t element_size) {
    std::uint64_t allocation = 0;
    return CheckedMultiply(count, element_size, &allocation) && add(allocation);
  };
  const auto add_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::uint64_t>::max() &&
           add(static_cast<std::uint64_t>(value.capacity()) + 1);
  };
  const auto add_descriptor = [&](const api::EngineDescriptor& descriptor) {
    return add_string(descriptor.descriptor_uuid.canonical) &&
           add_string(descriptor.descriptor_kind) &&
           add_string(descriptor.canonical_type_name) &&
           add_string(descriptor.encoded_descriptor);
  };
  if (!add_array(batch.columns.capacity(),
                 sizeof(exec::ExecutorColumnDescriptor)) ||
      !add_array(batch.rows.capacity(), sizeof(exec::DescriptorTuple))) {
    return false;
  }
  for (const auto& column : batch.columns) {
    if (!add_string(column.stable_name) ||
        !add_descriptor(column.descriptor)) {
      return false;
    }
  }
  for (const auto& row : batch.rows) {
    if (!add_array(row.values.capacity(), sizeof(api::EngineTypedValue))) {
      return false;
    }
    for (const auto& value : row.values) {
      if (!add_descriptor(value.descriptor) ||
          !add_string(value.encoded_value) ||
          !add(static_cast<std::uint64_t>(value.binary_value.capacity()))) {
        return false;
      }
    }
  }
  return true;
}

bool QueryDistinctAuxiliaryMemoryBytes(const std::size_t row_count,
                                       const std::size_t column_count,
                                       std::uint64_t* memory_bytes) {
  if (memory_bytes == nullptr) return false;
  std::uint64_t coverage_bytes = 0;
  std::uint64_t representative_bytes = 0;
  return CheckedMultiply(column_count, sizeof(std::uint8_t), &coverage_bytes) &&
         CheckedMultiply(row_count, sizeof(std::size_t),
                         &representative_bytes) &&
         CheckedAdd(coverage_bytes, representative_bytes, memory_bytes);
}

bool QueryDistinctOutputMemoryBytes(
    const exec::DescriptorBatch& batch,
    const std::vector<exec::CanonicalDescriptorOrderTerm>& equality_terms,
    const std::size_t maximum_value_comparisons,
    std::uint64_t* memory_bytes,
    std::string* detail) {
  if (memory_bytes == nullptr || detail == nullptr ||
      equality_terms.size() != batch.columns.size() || equality_terms.empty() ||
      (maximum_value_comparisons == 0 && !batch.rows.empty())) {
    return false;
  }
  *memory_bytes = 1;
  detail->clear();
  std::vector<std::size_t> representatives;
  representatives.reserve(batch.rows.size());
  std::size_t comparison_count = 0;
  for (std::size_t row = 0; row < batch.rows.size(); ++row) {
    bool duplicate = false;
    for (const auto representative : representatives) {
      bool equal = true;
      for (const auto& term : equality_terms) {
        if (comparison_count >= maximum_value_comparisons) {
          *detail = "query DISTINCT memory projection exceeded its work bound";
          return false;
        }
        ++comparison_count;
        if (term.column >= batch.columns.size() ||
            term.column >= batch.rows[row].values.size() ||
            term.column >= batch.rows[representative].values.size()) {
          *detail = "query DISTINCT memory projection is outside its schema";
          return false;
        }
        const auto compared = exec::CompareCanonicalDescriptorOrderValues(
            batch.rows[row].values[term.column],
            batch.rows[representative].values[term.column], term);
        if (!compared.diagnostic.ok) {
          *detail = compared.diagnostic.detail;
          return false;
        }
        if (compared.comparison != 0) {
          equal = false;
          break;
        }
      }
      if (equal) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    representatives.push_back(row);
    for (const auto& value : batch.rows[row].values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *memory_bytes) {
        *detail = "query DISTINCT output payload accounting overflowed";
        return false;
      }
      *memory_bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *memory_bytes) {
        *detail = "query DISTINCT output payload accounting overflowed";
        return false;
      }
      *memory_bytes += value.binary_value.size();
    }
  }
  return true;
}

bool AddBatchProjectionMemoryBytes(
    const exec::DescriptorBatch& batch,
    const std::vector<std::size_t>& projected_columns,
    std::uint64_t* memory_bytes) {
  if (memory_bytes == nullptr) return false;
  for (const auto& row : batch.rows) {
    for (const auto source_column : projected_columns) {
      if (source_column >= row.values.size()) return false;
      const auto& value = row.values[source_column];
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *memory_bytes) {
        return false;
      }
      *memory_bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *memory_bytes) {
        return false;
      }
      *memory_bytes += value.binary_value.size();
    }
  }
  return true;
}

bool AddBatchRowRangeMemoryBytes(const exec::DescriptorBatch& batch,
                                 const std::size_t first_row,
                                 const std::size_t row_count,
                                 std::uint64_t* memory_bytes) {
  if (memory_bytes == nullptr || first_row > batch.rows.size() ||
      row_count > batch.rows.size() - first_row) {
    return false;
  }
  for (std::size_t row_index = first_row;
       row_index < first_row + row_count; ++row_index) {
    for (const auto& value : batch.rows[row_index].values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *memory_bytes) {
        return false;
      }
      *memory_bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *memory_bytes) {
        return false;
      }
      *memory_bytes += value.binary_value.size();
    }
  }
  return true;
}

bool LogicalBitVectorPayloadBytes(const std::size_t bit_count,
                                  std::uint64_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = static_cast<std::uint64_t>(bit_count / 8);
  if (bit_count % 8 != 0) return CheckedAdd(*bytes, 1, bytes);
  return true;
}

std::uint64_t CanonicalUnsignedDecimalWidth(std::uint64_t value) {
  std::uint64_t width = 1;
  while (value >= 10) {
    value /= 10;
    ++width;
  }
  return width;
}

std::optional<std::size_t> SelectedNodeAggregateMemoryBound(
    const exec::TypedPhysicalNodeDag& dag,
    const exec::PhysicalNodeRecord& node) {
  if (dag.memory_budget_bytes == 0 || node.memory_bytes_required == 0 ||
      node.memory_bytes_required > dag.memory_budget_bytes ||
      node.memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return std::nullopt;
  }
  const auto callback_limit =
      node.dispatcher_callback_memory_limit_bytes == 0
          ? node.memory_bytes_required
          : std::min(node.memory_bytes_required,
                     node.dispatcher_callback_memory_limit_bytes);
  if (callback_limit == 0 ||
      callback_limit > std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(callback_limit);
}

}  // namespace scratchbird::engine::sblr
