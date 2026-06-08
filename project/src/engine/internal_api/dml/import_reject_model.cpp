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
// SB_ENGINE_INTERNAL_API_DML_IMPORT_REJECT_MODEL_BEHAVIOR
// SB_PID008_IMPORT_REJECT_MODEL

#include "dml/import_reject_model.hpp"

#include <initializer_list>
#include <string>

#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {
namespace {

bool OneOf(const std::string& value, std::initializer_list<const char*> allowed) {
  for (const char* item : allowed) {
    if (value == item) {
      return true;
    }
  }
  return false;
}

EngineDescriptor TextDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "text";
  descriptor.encoded_descriptor = "type=text;nullable=false";
  return descriptor;
}

EngineDescriptor NullableTextDescriptor() {
  EngineDescriptor descriptor = TextDescriptor();
  descriptor.encoded_descriptor = "type=text;nullable=true";
  return descriptor;
}

EngineDescriptor UInt64Descriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "uint64";
  descriptor.encoded_descriptor = "type=uint64;nullable=false";
  return descriptor;
}

EngineDescriptor BoolDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "boolean";
  descriptor.encoded_descriptor = "type=boolean;nullable=false";
  return descriptor;
}

EngineImportErrorRowColumn Column(std::string name,
                                  EngineDescriptor descriptor,
                                  bool nullable,
                                  bool redacted) {
  EngineImportErrorRowColumn column;
  column.column_name = std::move(name);
  column.descriptor = std::move(descriptor);
  column.nullable = nullable;
  column.redacted = redacted;
  return column;
}

EngineNormalizeImportRejectModelResult RejectModelFailure(const std::string& detail) {
  EngineNormalizeImportRejectModelResult result;
  result.ok = false;
  result.operation_id = "dml.normalize_import_reject_model";
  result.diagnostics.push_back(MakeInvalidRequestDiagnostic("dml.normalize_import_reject_model", detail));
  return result;
}

bool RejectTargetPresent(const EngineObjectReference& target) {
  return !target.uuid.canonical.empty();
}

bool RejectLimitPresent(const EngineImportRejectPolicyEnvelope& policy) {
  return policy.reject_limit_rows > 0 || policy.reject_limit_percent > 0.0;
}

}  // namespace

EngineImportErrorRowSchema BuildDefaultImportErrorRowSchema(bool include_payload_reference_columns) {
  EngineImportErrorRowSchema schema;
  schema.schema_version = 1;
  schema.columns.push_back(Column("source_row_number", UInt64Descriptor(), false, false));
  schema.columns.push_back(Column("source_position", NullableTextDescriptor(), true, false));
  schema.columns.push_back(Column("target_table_uuid", TextDescriptor(), false, false));
  schema.columns.push_back(Column("target_column", NullableTextDescriptor(), true, false));
  schema.columns.push_back(Column("diagnostic_code", TextDescriptor(), false, false));
  schema.columns.push_back(Column("message_key", TextDescriptor(), false, false));
  schema.columns.push_back(Column("diagnostic_detail", NullableTextDescriptor(), true, true));
  schema.columns.push_back(Column("rejected_value_digest", NullableTextDescriptor(), true, false));
  schema.columns.push_back(Column("value_redacted", BoolDescriptor(), false, false));
  schema.columns.push_back(Column("policy_name", NullableTextDescriptor(), true, false));
  schema.columns.push_back(Column("audit_evidence_id", NullableTextDescriptor(), true, false));
  if (include_payload_reference_columns) {
    schema.columns.push_back(Column("payload_reference_uuid", NullableTextDescriptor(), true, false));
    schema.columns.push_back(Column("payload_encryption_profile", NullableTextDescriptor(), true, false));
  }
  return schema;
}

EngineNormalizeImportRejectModelResult EngineNormalizeImportRejectModel(
    const EngineNormalizeImportRejectModelRequest& request) {
  const auto& policy = request.reject_policy;

  if (request.context.local_transaction_id == 0) {
    return RejectModelFailure("local_transaction_id_required");
  }
  if (!request.localized_names.empty()) {
    return RejectModelFailure("localized_names_not_allowed_engine_boundary");
  }
  if (request.target_table.uuid.canonical.empty()) {
    return RejectModelFailure("target_table_uuid_required");
  }
  if (!OneOf(policy.reject_mode, {"fail_fast", "reject_row", "reject_table", "quarantine"})) {
    return RejectModelFailure("reject_mode_unsupported:" + policy.reject_mode);
  }
  if (!OneOf(policy.reject_payload_policy,
             {"diagnostic_only", "redacted_payload_reference", "encrypted_payload_reference"})) {
    return RejectModelFailure("reject_payload_policy_unsupported:" + policy.reject_payload_policy);
  }
  if (!OneOf(policy.resume_policy,
             {"fail_closed", "resume_from_checkpoint", "operator_review_required"})) {
    return RejectModelFailure("resume_policy_unsupported:" + policy.resume_policy);
  }
  if (policy.reject_limit_percent < 0.0 || policy.reject_limit_percent > 100.0) {
    return RejectModelFailure("reject_limit_percent_invalid");
  }

  const bool target_present = RejectTargetPresent(policy.reject_target);
  const bool limit_present = RejectLimitPresent(policy);
  bool target_required = false;

  if (policy.reject_mode == "fail_fast") {
    if (limit_present || target_present || policy.reject_payload_policy != "diagnostic_only") {
      return RejectModelFailure("fail_fast_requires_zero_limits_no_target_diagnostic_only");
    }
  } else if (policy.reject_mode == "reject_row") {
    if (!limit_present) {
      return RejectModelFailure("reject_row_requires_explicit_limit");
    }
  } else if (policy.reject_mode == "reject_table") {
    target_required = true;
    if (!target_present) {
      return RejectModelFailure("reject_table_requires_reject_target_uuid");
    }
    if (!limit_present) {
      return RejectModelFailure("reject_table_requires_explicit_limit");
    }
  } else if (policy.reject_mode == "quarantine") {
    target_required = true;
    if (!target_present) {
      return RejectModelFailure("quarantine_requires_reject_target_uuid");
    }
    if (policy.reject_payload_policy == "diagnostic_only") {
      return RejectModelFailure("quarantine_requires_payload_reference_policy");
    }
  }

  EngineNormalizeImportRejectModelResult result;
  result.ok = true;
  result.operation_id = "dml.normalize_import_reject_model";
  result.normalized_reject_mode = policy.reject_mode;
  result.normalized_payload_policy = policy.reject_payload_policy;
  result.normalized_resume_policy = policy.resume_policy;
  result.effective_reject_limit_rows = policy.reject_limit_rows;
  result.effective_reject_limit_percent = policy.reject_limit_percent;
  result.reject_target_required = target_required;
  result.reject_target_present = target_present;
  result.error_row_schema = BuildDefaultImportErrorRowSchema(
      request.include_payload_reference_columns || policy.reject_payload_policy != "diagnostic_only");
  result.primary_object = request.target_table;
  result.evidence.push_back({"import_reject_model", policy.reject_mode});
  result.evidence.push_back({"import_reject_payload_policy", policy.reject_payload_policy});
  result.evidence.push_back({"import_resume_policy", policy.resume_policy});
  result.evidence.push_back({"import_error_row_schema_version", "1"});
  result.evidence.push_back({"target_object_uuid", request.target_table.uuid.canonical});
  if (target_present) {
    result.evidence.push_back({"reject_target_uuid", policy.reject_target.uuid.canonical});
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
