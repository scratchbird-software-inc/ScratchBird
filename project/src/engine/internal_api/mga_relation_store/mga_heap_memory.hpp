// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "mga_relation_store/mga_relation_descriptor.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace scratchbird::engine::internal_api {

// Conservative heap-reader accounting. Inline UUID bytes are covered by
// sizeof their enclosing record or map node, never by dynamic string charges.
// These estimates do not grant execution, visibility or memory reservations.
bool CheckedHeapReadMemoryAdd(std::uint64_t value, std::uint64_t* total);
bool CheckedHeapReadMemoryMultiply(std::uint64_t left, std::uint64_t right,
                                  std::uint64_t* product);
bool AccountHeapReadOwnedString(const std::string& value, std::uint64_t* total);
// Inline variant/UUID storage is charged by sizeof(EngineEvidenceReference).
bool AccountHeapReadEvidenceDynamicMemory(const EngineEvidenceReference& evidence,
                                         std::uint64_t* total);
bool AccountHeapReadRowDynamicMemoryBytes(const CrudRowVersionRecord& row,
                                         std::uint64_t* total);
bool AccountHeapReadEngineDescriptorMemory(const EngineDescriptor& descriptor,
                                          std::uint64_t* total);
std::optional<std::uint64_t> HeapReadRowVectorMemoryBytes(
    const std::vector<CrudRowVersionRecord>& rows);
std::optional<std::uint64_t> HeapReadVersionIndexProjectionMemoryBytes(
    const std::vector<CrudRowVersionRecord>& rows);
std::optional<std::uint64_t> HeapReadVisibilityMapProjectionMemoryBytes(
    const std::vector<CrudRowVersionRecord>& rows);
std::optional<std::uint64_t> HeapReadStringVectorMemoryBytes(
    const std::vector<std::string>& values);
std::optional<std::uint64_t> HeapReadStringCacheMemoryBytes(
    const std::unordered_map<std::string, std::string>& cache);
std::optional<std::uint64_t> HeapReadStorageDescriptorMemoryBytes(
    const MgaRelationStorageDescriptor& descriptor);
std::optional<std::uint64_t> HeapReadVisibilityMapMemoryBytes(
    const std::unordered_map<EngineUuid, std::size_t, EngineUuidHash>& rows);

}  // namespace scratchbird::engine::internal_api
