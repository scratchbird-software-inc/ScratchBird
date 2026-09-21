// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_heap_memory.hpp"
#include <limits>
#include <utility>

namespace scratchbird::engine::internal_api {

bool CheckedHeapReadMemoryAdd(const std::uint64_t value,
                              std::uint64_t* total) {
  if (total == nullptr ||
      value > std::numeric_limits<std::uint64_t>::max() - *total) {
    return false;
  }
  *total += value;
  return true;
}

bool CheckedHeapReadMemoryMultiply(const std::uint64_t left,
                                   const std::uint64_t right,
                                   std::uint64_t* product) {
  if (product == nullptr ||
      (right != 0 &&
       left > std::numeric_limits<std::uint64_t>::max() / right)) {
    return false;
  }
  *product = left * right;
  return true;
}

bool AccountHeapReadOwnedString(const std::string& value,
                                std::uint64_t* total) {
  return value.capacity() < std::numeric_limits<std::uint64_t>::max() &&
         CheckedHeapReadMemoryAdd(
             static_cast<std::uint64_t>(value.capacity()) + 1, total);
}

bool AccountHeapReadEvidenceDynamicMemory(const EngineEvidenceReference& evidence,
                                         std::uint64_t* total) {
  if (!total || evidence.evidence_id.valueless_by_exception()) return false;
  auto candidate = *total;
  if (!AccountHeapReadOwnedString(evidence.evidence_kind, &candidate)) return false;
  if (const auto* text = std::get_if<std::string>(&evidence.evidence_id);
      text && !AccountHeapReadOwnedString(*text, &candidate)) return false;
  *total = candidate;
  return true;
}

std::optional<std::uint64_t> HeapReadRowVectorMemoryBytes(
    const std::vector<CrudRowVersionRecord>& rows) {
  std::uint64_t bytes = sizeof(rows);
  std::uint64_t allocation_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.capacity()),
          sizeof(CrudRowVersionRecord), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& row : rows) {
    if (!CheckedHeapReadMemoryMultiply(
            static_cast<std::uint64_t>(row.values.capacity()),
            sizeof(std::pair<std::string, std::string>),
            &allocation_bytes) ||
        !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
      return std::nullopt;
    }
    for (const auto& [key, value] : row.values) {
      if (!AccountHeapReadOwnedString(key, &bytes) ||
          !AccountHeapReadOwnedString(value, &bytes)) {
        return std::nullopt;
      }
    }
  }
  return bytes;
}

bool AccountHeapReadRowDynamicMemoryBytes(
    const CrudRowVersionRecord& row, std::uint64_t* total) {
  std::uint64_t allocation_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(row.values.capacity()),
          sizeof(std::pair<std::string, std::string>),
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, total)) {
    return false;
  }
  for (const auto& [key, value] : row.values) {
    if (!AccountHeapReadOwnedString(key, total) ||
        !AccountHeapReadOwnedString(value, total)) {
      return false;
    }
  }
  return true;
}

std::optional<std::uint64_t> HeapReadVersionIndexProjectionMemoryBytes(
    const std::vector<CrudRowVersionRecord>& rows) {
  constexpr std::uint64_t kNodeOverhead = 4 * sizeof(void*);
  std::uint64_t bytes =
      sizeof(std::unordered_map<EngineUuid, const CrudRowVersionRecord*, EngineUuidHash>);
  std::uint64_t allocation_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.size()), 2 * sizeof(void*),
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.size()),
          sizeof(std::pair<const EngineUuid,
                           const CrudRowVersionRecord*>) +
              kNodeOverhead,
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  return bytes;
}

std::optional<std::uint64_t> HeapReadVisibilityMapProjectionMemoryBytes(
    const std::vector<CrudRowVersionRecord>& rows) {
  constexpr std::uint64_t kNodeOverhead = 4 * sizeof(void*);
  std::uint64_t bytes =
      sizeof(std::unordered_map<EngineUuid, std::size_t, EngineUuidHash>);
  std::uint64_t allocation_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.size()), 2 * sizeof(void*),
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.size()),
          sizeof(std::pair<const EngineUuid, std::size_t>) +
              kNodeOverhead,
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  return bytes;
}

std::optional<std::uint64_t> HeapReadStringVectorMemoryBytes(
    const std::vector<std::string>& values) {
  std::uint64_t bytes = sizeof(values);
  std::uint64_t allocation_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(values.capacity()),
          sizeof(std::string), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& value : values) {
    if (!AccountHeapReadOwnedString(value, &bytes)) return std::nullopt;
  }
  return bytes;
}

std::optional<std::uint64_t> HeapReadStringCacheMemoryBytes(
    const std::unordered_map<std::string, std::string>& cache) {
  constexpr std::uint64_t kNodeOverhead = 4 * sizeof(void*);
  std::uint64_t bytes = sizeof(cache);
  std::uint64_t allocation_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(cache.bucket_count()), sizeof(void*),
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(cache.size()),
          sizeof(std::pair<const std::string, std::string>) + kNodeOverhead,
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& [key, value] : cache) {
    if (!AccountHeapReadOwnedString(key, &bytes) ||
        !AccountHeapReadOwnedString(value, &bytes)) {
      return std::nullopt;
    }
  }
  return bytes;
}


bool AccountHeapReadEngineDescriptorMemory(
    const EngineDescriptor& descriptor, std::uint64_t* total) {
  // Fixed UUID bytes are already included in the enclosing descriptor size.
  return AccountHeapReadOwnedString(descriptor.descriptor_kind, total) &&
         AccountHeapReadOwnedString(descriptor.canonical_type_name, total) &&
         AccountHeapReadOwnedString(descriptor.encoded_descriptor, total);
}

std::optional<std::uint64_t> HeapReadStorageDescriptorMemoryBytes(
    const MgaRelationStorageDescriptor& descriptor) {
  std::uint64_t bytes = sizeof(descriptor);
  std::uint64_t allocation_bytes = 0;
  if (!AccountHeapReadOwnedString(descriptor.relation_kind, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.storage_profile, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.row_identity_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.version_identity_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.mutation_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.visibility_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.cleanup_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.recovery_rule, &bytes) ||
      !AccountHeapReadOwnedString(descriptor.descriptor_status, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(descriptor.columns.capacity()),
          sizeof(MgaRelationColumnStorageDescriptor), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(descriptor.indexes.capacity()),
          sizeof(MgaRelationIndexStorageDescriptor), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(
              descriptor.required_evidence_kinds.capacity()),
          sizeof(std::string), &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  for (const auto& column : descriptor.columns) {
    if (!AccountHeapReadOwnedString(column.canonical_name_key, &bytes) ||
        !AccountHeapReadEngineDescriptorMemory(column.value_descriptor,
                                               &bytes) ||
        !AccountHeapReadOwnedString(column.storage_class, &bytes) ||
        !AccountHeapReadOwnedString(column.overflow_policy, &bytes)) {
      return std::nullopt;
    }
  }
  for (const auto& index : descriptor.indexes) {
    if (!AccountHeapReadOwnedString(index.family, &bytes) ||
        !AccountHeapReadOwnedString(index.profile, &bytes) ||
        !AccountHeapReadOwnedString(index.predicate_kind, &bytes) ||
        !AccountHeapReadOwnedString(index.predicate_column, &bytes) ||
        !AccountHeapReadOwnedString(index.predicate_value, &bytes) ||
        !AccountHeapReadOwnedString(index.residency_policy, &bytes) ||
        !CheckedHeapReadMemoryMultiply(
            static_cast<std::uint64_t>(index.key_envelopes.capacity()),
            sizeof(std::string), &allocation_bytes) ||
        !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
        !CheckedHeapReadMemoryMultiply(
            static_cast<std::uint64_t>(index.include_columns.capacity()),
            sizeof(std::string), &allocation_bytes) ||
        !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
      return std::nullopt;
    }
    for (const auto& value : index.key_envelopes) {
      if (!AccountHeapReadOwnedString(value, &bytes)) return std::nullopt;
    }
    for (const auto& value : index.include_columns) {
      if (!AccountHeapReadOwnedString(value, &bytes)) return std::nullopt;
    }
  }
  for (const auto& value : descriptor.required_evidence_kinds) {
    if (!AccountHeapReadOwnedString(value, &bytes)) return std::nullopt;
  }
  return bytes;
}


std::optional<std::uint64_t> HeapReadVisibilityMapMemoryBytes(
    const std::unordered_map<EngineUuid, std::size_t, EngineUuidHash>& rows) {
  constexpr std::uint64_t kNodeOverhead = 4 * sizeof(void*);
  std::uint64_t bytes = sizeof(rows);
  std::uint64_t allocation_bytes = 0;
  if (!CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.bucket_count()), sizeof(void*),
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes) ||
      !CheckedHeapReadMemoryMultiply(
          static_cast<std::uint64_t>(rows.size()),
          sizeof(std::pair<const EngineUuid, std::size_t>) + kNodeOverhead,
          &allocation_bytes) ||
      !CheckedHeapReadMemoryAdd(allocation_bytes, &bytes)) {
    return std::nullopt;
  }
  return bytes;
}


}  // namespace scratchbird::engine::internal_api
