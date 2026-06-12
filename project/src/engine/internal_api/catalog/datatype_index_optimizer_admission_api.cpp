// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/datatype_index_optimizer_admission_api.hpp"

#include "catalog/wire_driver_metadata_api.hpp"

namespace scratchbird::engine::internal_api {

EngineDatatypeIndexOptimizerAdmissionResult EvaluateDatatypeIndexOptimizerAdmission(
    const EngineDatatypeIndexOptimizerAdmissionRequest& request) {
  EngineDatatypeIndexOptimizerAdmissionResult result;
  const auto metadata = RenderWireDriverMetadata(request.descriptor, {}, request.reference_label);
  result.canonical_descriptor_used = request.descriptor.canonical_type_name.empty()
                                         ? metadata.base_canonical_type_name
                                         : request.descriptor.canonical_type_name;
  result.optimizer_uses_canonical_descriptor =
      !result.canonical_descriptor_used.empty() && result.canonical_descriptor_used != request.reference_label;

  if (request.index_stats_status == "implemented" || request.index_stats_status == "validated") {
    if (request.support_path == "opaque_preserve_render" || metadata.opaque_render_only) {
      result.diagnostic_detail = "opaque_index_statistics_denied";
      return result;
    }
    result.index_admitted = true;
    result.statistics_admitted = true;
    result.ok = result.optimizer_uses_canonical_descriptor;
    if (!result.ok) { result.diagnostic_detail = "optimizer_descriptor_required"; }
    return result;
  }

  if (request.index_stats_status == "decided") {
    result.ok = result.optimizer_uses_canonical_descriptor;
    result.diagnostic_detail = "index_statistics_policy_decided_not_executable";
    return result;
  }

  if (request.index_stats_status == "blocked") {
    result.ok = result.optimizer_uses_canonical_descriptor;
    result.diagnostic_detail = "index_statistics_blocked_by_required_subsystem";
    return result;
  }

  result.diagnostic_detail = "unknown_index_statistics_status";
  return result;
}

}  // namespace scratchbird::engine::internal_api
