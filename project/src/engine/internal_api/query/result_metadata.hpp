// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "wire/typed_result_transport_codec.hpp"

#include <optional>
#include <memory>

namespace scratchbird::engine::internal_api {

// Owned query output schema, independent of the number of materialized rows.
// The registry descriptor identifies the datatype codec. A bound expression or
// column descriptor is a different identity and must not replace that tuple.
struct EngineQueryResultColumnV1 {
  wire::TypedResultColumnDescriptor transport;
  wire::TypedResultUuid bound_descriptor_uuid{};
  std::optional<wire::TypedResultUuid> collation_uuid;
  std::optional<std::string> timezone_profile_id;
  std::optional<std::uint32_t> width;
  std::optional<std::uint32_t> precision;
  std::optional<std::uint32_t> scale;
};

struct EngineQueryResultMetadataV1 {
  wire::TypedResultUuid statement_receipt_uuid{};
  wire::TypedResultUuid statement_snapshot_uuid{};
  wire::TypedResultUuid datatype_catalog_snapshot_uuid{};
  std::uint64_t datatype_catalog_generation = 0;
  std::uint64_t datatype_registry_generation = 0;
  std::vector<EngineQueryResultColumnV1> columns;
};

struct EngineQueryResultValuesV1 {
  std::shared_ptr<const EngineQueryResultMetadataV1> metadata;
  // Whole-result ordinals; the packet producer rebases each staged batch to0.
  std::vector<wire::TypedResultRow> rows;
};

}  // namespace scratchbird::engine::internal_api
