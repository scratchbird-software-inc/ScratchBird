// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_DATATYPE_INDEX_OPTIMIZER_ADMISSION_API
// Descriptor-owned index/statistics/optimizer admission policy. Reference labels
// are display aliases only and are never optimizer authority.

struct EngineDatatypeIndexOptimizerAdmissionRequest {
  std::string type_group;
  EngineDescriptor descriptor;
  std::string support_path;
  std::string index_stats_status;
  std::string reference_label;
};

struct EngineDatatypeIndexOptimizerAdmissionResult {
  bool ok = false;
  bool index_admitted = false;
  bool statistics_admitted = false;
  bool optimizer_uses_canonical_descriptor = false;
  std::string diagnostic_detail;
  std::string canonical_descriptor_used;
};

EngineDatatypeIndexOptimizerAdmissionResult EvaluateDatatypeIndexOptimizerAdmission(
    const EngineDatatypeIndexOptimizerAdmissionRequest& request);

}  // namespace scratchbird::engine::internal_api
