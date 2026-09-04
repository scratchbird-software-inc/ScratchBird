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

#include <span>

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
  // Specialized engine-owned mutation producers may bind one durable
  // publication journal to this statement savepoint. These hooks are never a
  // parser/public ABI surface and may not alter rows or authority.
  std::function<EngineApiDiagnostic()> before_mutation_publication;
  std::function<EngineApiDiagnostic(
      std::span<const std::string>, std::span<const std::string>,
      std::span<const std::string>)>
      before_row_publication;
  std::function<EngineApiDiagnostic(EngineApiU64, EngineApiU64,
                                    EngineApiU64)>
      before_statement_publication;
  std::function<EngineApiDiagnostic(EngineApiU64, EngineApiU64,
                                    EngineApiU64)>
      after_statement_publication;
  std::function<void()> after_statement_rollback;
};

struct EngineExecuteNativeBulkIngestResult : public EngineApiResult {
  EngineApiU64 accepted_rows = 0;
  EngineApiU64 inserted_rows = 0;
  EngineApiU64 rejected_rows = 0;
  std::vector<EngineUuid> row_uuids;
  std::vector<EngineUuid> row_version_uuids;
  std::vector<EngineUuid> row_image_uuids;
  bool delegated_to_import_execution = false;
};

EngineExecuteNativeBulkIngestResult EngineExecuteNativeBulkIngest(
    const EngineExecuteNativeBulkIngestRequest& request);

}  // namespace scratchbird::engine::internal_api
