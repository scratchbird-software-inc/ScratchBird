// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include <functional>
#include <span>
#include <string>
#include <vector>
namespace scratchbird::engine::internal_api::dml {
struct DirectPhysicalBulkAppendRequest {
  EngineRequestContext context;
  EngineObjectReference target_table;
  std::span<const EngineRowValue> borrowed_input_rows;
  const EngineNativeRowPacketFrame* native_row_packet = nullptr;
  std::vector<std::string> owned_shared_row_field_order;
  std::span<const std::string> shared_row_field_order;
  std::vector<std::string> option_envelopes;
  std::vector<std::string> diagnostic_options;
  EngineApiU64 estimated_row_count = 0;
  std::string lane_operation = "copy_import";
  std::string duplicate_mode = "error";
  bool require_generated_row_uuid = true;
  bool strict_bulk_load_requested = false;
  bool direct_lane_enabled = true;
  // Specialized MGA producers may durably bind the exact row/version/image
  // identities after the normal allocators run but before any row bytes are
  // appended. A diagnostic aborts the whole caller-owned statement savepoint.
  std::function<EngineApiDiagnostic(
      std::span<const EngineUuid>, std::span<const EngineUuid>,
      std::span<const EngineUuid>)>
      before_row_publication;
};

struct DirectPhysicalBulkAppendResult : EngineApiResult {
  EngineApiU64 accepted_rows = 0;
  EngineApiU64 inserted_rows = 0;
  EngineApiU64 rejected_rows = 0;
  std::vector<EngineUuid> row_uuids;
  std::vector<EngineUuid> row_version_uuids;
  std::vector<EngineUuid> row_image_uuids;
  bool direct_lane_selected = false;
};

DirectPhysicalBulkAppendResult ExecuteDirectPhysicalBulkAppend(
    const DirectPhysicalBulkAppendRequest& request);

} // namespace scratchbird::engine::internal_api::dml
