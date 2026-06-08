// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "dml/import_api.hpp"
#include "dml/import_resume_checkpoint.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DML_NATIVE_BULK_INGEST
struct EngineExecuteNativeBulkIngestRequest : public EngineApiRequest {
  EngineObjectReference target_table;
  std::vector<EngineRowValue> canonical_rows;
  EngineImportPolicyEnvelope import_policy;
  EngineImportCheckpointPolicyEnvelope checkpoint_policy;
  EngineApiU64 estimated_row_count = 0;
  std::string duplicate_mode = "error";
  bool require_generated_row_uuid = true;
  bool native_bulk_ingest_enabled = true;
};

struct EngineExecuteNativeBulkIngestResult : public EngineApiResult {
  EngineApiU64 accepted_rows = 0;
  EngineApiU64 inserted_rows = 0;
  EngineApiU64 rejected_rows = 0;
  std::vector<EngineUuid> row_uuids;
  bool delegated_to_import_execution = false;
};

EngineExecuteNativeBulkIngestResult EngineExecuteNativeBulkIngest(
    const EngineExecuteNativeBulkIngestRequest& request);

}  // namespace scratchbird::engine::internal_api
