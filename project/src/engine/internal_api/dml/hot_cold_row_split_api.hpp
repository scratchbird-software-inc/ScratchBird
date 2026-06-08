// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "hot_cold_row_split.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_HOT_COLD_ROW_SPLIT
struct EngineHotColdFieldPolicy {
  std::string field_name;
  bool metadata = false;
  bool indexed = false;
  bool frequently_filtered = false;
  bool rare_projection = false;
  bool force_hot = false;
  bool force_cold = false;
};

struct EngineDmlHotColdSplitRequest {
  EngineRequestContext context;
  EngineRowValue row;
  EngineUuid filespace_uuid;
  EngineUuid owner_object_uuid;
  std::vector<EngineHotColdFieldPolicy> field_policy;
  scratchbird::storage::page::LargePayloadStore* large_payload_store = nullptr;
  EngineApiU64 cold_threshold_bytes = 4096;
  bool engine_storage_admission_authorized = false;
};

struct EngineDmlHotColdSplitResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  scratchbird::storage::page::HotColdRowHead hot_head;
  std::string serialized_hot_head;
  std::vector<std::pair<std::string, std::string>> storage_values;
  std::vector<EngineEvidenceReference> evidence;
};

struct EngineDmlHotColdMaterializeRequest {
  EngineRequestContext context;
  scratchbird::storage::page::HotColdRowHead hot_head;
  std::vector<std::string> cold_field_names;
  scratchbird::storage::page::LargePayloadStore* large_payload_store = nullptr;
  bool engine_storage_admission_authorized = false;
  bool use_cache = true;
  bool prefetch_on_miss = false;
};

struct EngineDmlHotColdMaterializeResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<std::pair<std::string, std::string>> cold_values;
  std::vector<EngineEvidenceReference> evidence;
};

struct EngineDmlHotColdUpdateRequest {
  EngineDmlHotColdSplitRequest replacement;
  scratchbird::storage::page::HotColdRowHead previous_hot_head;
};

struct EngineDmlHotColdUpdateResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  scratchbird::storage::page::HotColdRowHead hot_head;
  std::vector<scratchbird::storage::page::LargePayloadDescriptor> retired_descriptors;
  std::vector<EngineEvidenceReference> evidence;
};

EngineDmlHotColdSplitResult EngineDmlSplitHotColdRow(
    const EngineDmlHotColdSplitRequest& request);
EngineDmlHotColdMaterializeResult EngineDmlMaterializeColdFields(
    const EngineDmlHotColdMaterializeRequest& request);
EngineDmlHotColdUpdateResult EngineDmlUpdateHotColdRow(
    const EngineDmlHotColdUpdateRequest& request);

}  // namespace scratchbird::engine::internal_api
