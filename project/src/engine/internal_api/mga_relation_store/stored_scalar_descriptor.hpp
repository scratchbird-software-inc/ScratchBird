// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "mga_relation_store/stored_integer_descriptor.hpp"
#include "mga_relation_store/stored_timestamp_descriptor.hpp"

namespace scratchbird::engine::internal_api {
// These stored descriptors have a distinct, receipt-bound scalar execution
// view. Copying their storage metadata and relabelling descriptor_kind is not
// an execution binding, even when their payload bytes need no conversion.
inline bool StoredScalarProjectionRequiredV1(const EngineDescriptor& source) {
  return source.canonical_type_name == "int32" ||
         source.canonical_type_name == "int64" ||
         source.canonical_type_name == "timestamp";
}
inline bool ProjectStoredScalarDescriptorV1(
    const EngineRequestContext& context, const EngineDescriptor& source,
    bool nullable, EngineDescriptor* output, std::string* detail) {
  if (source.canonical_type_name == "timestamp")
    return ProjectStoredHistoricalTimestampDescriptorV1(context, source, nullable, output, detail);
  return ProjectStoredIntegerDescriptorV1(context, source, nullable, output, detail);
}
}  // namespace scratchbird::engine::internal_api
