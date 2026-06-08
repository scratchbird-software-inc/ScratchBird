// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// SB_ENGINE_INTERNAL_API_DML_IMPORT_EXECUTION
// SB_PID010_IMPORT_INSERT_BULK_INTEGRATION

#pragma once

#include <string>
#include <vector>

#include "api_types.hpp"
#include "dml/import_api.hpp"
#include "dml/import_resume_checkpoint.hpp"
#include "dml/insert_api.hpp"

namespace scratchbird::engine::internal_api {

struct EngineExecuteImportRowsRequest : public EngineApiRequest {
  EngineObjectReference target_table;
  EngineImportSourceEnvelope source;
  EngineImportFormatEnvelope format;
  std::vector<EngineImportColumnMapping> column_mappings;
  EngineImportPolicyEnvelope import_policy;
  EngineImportCheckpointPolicyEnvelope checkpoint_policy;
  std::vector<EngineRowValue> canonical_rows;
  EngineApiU64 estimated_row_count = 0;
  std::string duplicate_mode = "error";
  bool require_generated_row_uuid = true;
};

struct EngineExecuteImportRowsResult : public EngineApiResult {
  EngineApiU64 accepted_rows = 0;
  EngineApiU64 inserted_rows = 0;
  EngineApiU64 rejected_rows = 0;
  std::vector<EngineUuid> row_uuids;
  std::string normalized_source_kind;
  std::string normalized_format_family;
  std::string normalized_insert_mode;
  std::string normalized_reject_mode;
  std::string normalized_checkpoint_mode;
  bool delegated_to_insert_rows = false;
  bool checkpoint_model_normalized = false;
  bool reject_model_normalized = false;
};

EngineExecuteImportRowsResult EngineExecuteImportRows(const EngineExecuteImportRowsRequest& request);

}  // namespace scratchbird::engine::internal_api
