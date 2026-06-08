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
// SB_ENGINE_INTERNAL_API_DML_IMPORT_REJECT_MODEL
// SB_PID008_IMPORT_REJECT_MODEL

#pragma once

#include <string>
#include <vector>

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

struct EngineImportRejectPolicyEnvelope {
  std::string reject_mode = "fail_fast";
  EngineApiU64 reject_limit_rows = 0;
  double reject_limit_percent = 0.0;
  EngineObjectReference reject_target;
  std::string reject_payload_policy = "diagnostic_only";
  std::string resume_policy = "fail_closed";
};

struct EngineImportErrorRowColumn {
  std::string column_name;
  EngineDescriptor descriptor;
  bool nullable = false;
  bool redacted = false;
};

struct EngineImportErrorRowSchema {
  EngineApiU64 schema_version = 1;
  std::vector<EngineImportErrorRowColumn> columns;
};

struct EngineImportRowDiagnosticEnvelope {
  EngineApiU64 source_row_number = 0;
  std::string source_position;
  std::string diagnostic_code;
  std::string message_key;
  std::string field_name;
  std::string rejected_value_digest;
  bool value_redacted = true;
};

struct EngineNormalizeImportRejectModelRequest : public EngineApiRequest {
  EngineObjectReference target_table;
  EngineImportRejectPolicyEnvelope reject_policy;
  bool include_payload_reference_columns = false;
};

struct EngineNormalizeImportRejectModelResult : public EngineApiResult {
  std::string normalized_reject_mode;
  std::string normalized_payload_policy;
  std::string normalized_resume_policy;
  EngineApiU64 effective_reject_limit_rows = 0;
  double effective_reject_limit_percent = 0.0;
  bool reject_target_required = false;
  bool reject_target_present = false;
  bool row_diagnostics_required = true;
  bool audit_evidence_required = true;
  EngineImportErrorRowSchema error_row_schema;
};

EngineImportErrorRowSchema BuildDefaultImportErrorRowSchema(bool include_payload_reference_columns);
EngineNormalizeImportRejectModelResult EngineNormalizeImportRejectModel(
    const EngineNormalizeImportRejectModelRequest& request);

}  // namespace scratchbird::engine::internal_api
