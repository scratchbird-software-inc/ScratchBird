// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include "catalog_record_codec.hpp"
#include <string_view>

namespace scratchbird::engine::internal_api {
struct LocalMetricVisibilityDecision {
  bool read_allowed = false;
  bool sensitive_labels_allowed = false;
  std::string_view diagnostic = "METRIC.ACCESS_DENIED";
};
// Inputs must come from the caller's retained native snapshot and live
// security context. This evaluator neither acquires nor replaces those pins.
LocalMetricVisibilityDecision EvaluateLocalMetricVisibility(
    const EngineRequestContext& context,
    const core::catalog::CatalogMetadataVersion& descriptor,
    const core::catalog::CatalogMetadataVersion& policy) noexcept;
}  // namespace scratchbird::engine::internal_api
