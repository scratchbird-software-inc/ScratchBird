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
// SB_ENGINE_INTERNAL_API_DML_IMPORT_EXECUTION_BEHAVIOR
// SB_PID010_IMPORT_INSERT_BULK_INTEGRATION

#include "dml/import_execution_api.hpp"

#include <algorithm>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "api_diagnostics.hpp"
#include "dml/import_reject_model.hpp"
#include "dml/insert_physical_integration.hpp"
#include "dml/write_result_policy.hpp"
#include "observability/dml_summary_counters.hpp"

namespace scratchbird::engine::internal_api {
namespace {

constexpr EngineApiU64 kDefaultCopyAppendBatchRows = 1024;

struct CopyAppendBatchPolicy {
  bool enabled = true;
  EngineApiU64 max_rows = kDefaultCopyAppendBatchRows;
};

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

bool IsTruthyOptionValue(const std::string& value) {
  return value == "1" || value == "true" || value == "enabled" || value == "on";
}

bool IsFalsyOptionValue(const std::string& value) {
  return value == "0" || value == "false" || value == "disabled" || value == "off";
}

std::string ImportOptionValue(const EngineExecuteImportRowsRequest& request,
                              const std::string& key) {
  const std::string colon_prefix = key + ":";
  const std::string equals_prefix = key + "=";
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(colon_prefix, 0) == 0) {
      return option.substr(colon_prefix.size());
    }
    if (option.rfind(equals_prefix, 0) == 0) {
      return option.substr(equals_prefix.size());
    }
  }
  return {};
}

EngineApiU64 ParsePositiveU64(const std::string& value, EngineApiU64 fallback) {
  if (value.empty()) {
    return fallback;
  }
  EngineApiU64 parsed = 0;
  for (const unsigned char ch : value) {
    if (ch < '0' || ch > '9') {
      return fallback;
    }
    const EngineApiU64 digit = static_cast<EngineApiU64>(ch - '0');
    if (parsed > (std::numeric_limits<EngineApiU64>::max() - digit) / 10) {
      return fallback;
    }
    parsed = parsed * 10 + digit;
  }
  return parsed == 0 ? fallback : parsed;
}

CopyAppendBatchPolicy ResolveCopyAppendBatchPolicy(
    const EngineExecuteImportRowsRequest& request) {
  CopyAppendBatchPolicy policy;
  const std::string enabled = LowerAscii(ImportOptionValue(request, "copy_append_batching"));
  if (IsFalsyOptionValue(enabled)) {
    policy.enabled = false;
  } else if (IsTruthyOptionValue(enabled)) {
    policy.enabled = true;
  }
  policy.max_rows = ParsePositiveU64(
      ImportOptionValue(request, "copy_append_batch_rows"),
      kDefaultCopyAppendBatchRows);
  return policy;
}

EngineExecuteImportRowsResult ImportExecutionFailure(const std::string& detail) {
  EngineExecuteImportRowsResult result;
  result.ok = false;
  result.operation_id = "dml.execute_import_rows";
  result.diagnostics.push_back(MakeInvalidRequestDiagnostic("dml.execute_import_rows", detail));
  return result;
}

EngineExecuteImportRowsResult ImportExecutionFailureFromPlan(const EnginePlanImportRowsResult& plan) {
  EngineExecuteImportRowsResult result;
  result.ok = false;
  result.operation_id = "dml.execute_import_rows";
  result.diagnostics = plan.diagnostics;
  result.unsupported_features = plan.unsupported_features;
  result.evidence = plan.evidence;
  result.evidence.push_back({"import_execution_refused_by", "dml.plan_import_rows"});
  return result;
}

EngineExecuteImportRowsResult ImportExecutionFailureFromRejectModel(
    const EngineNormalizeImportRejectModelResult& reject_model) {
  EngineExecuteImportRowsResult result;
  result.ok = false;
  result.operation_id = "dml.execute_import_rows";
  result.diagnostics = reject_model.diagnostics;
  result.unsupported_features = reject_model.unsupported_features;
  result.evidence = reject_model.evidence;
  result.evidence.push_back({"import_execution_refused_by", "dml.normalize_import_reject_model"});
  return result;
}

EngineExecuteImportRowsResult ImportExecutionFailureFromCheckpointModel(
    const EngineNormalizeImportCheckpointResult& checkpoint_model) {
  EngineExecuteImportRowsResult result;
  result.ok = false;
  result.operation_id = "dml.execute_import_rows";
  result.diagnostics = checkpoint_model.diagnostics;
  result.unsupported_features = checkpoint_model.unsupported_features;
  result.evidence = checkpoint_model.evidence;
  result.evidence.push_back({"import_execution_refused_by", "dml.normalize_import_checkpoint_model"});
  return result;
}

EngineExecuteImportRowsResult ImportExecutionFailureFromInsert(const EngineInsertRowsResult& insert_result) {
  EngineExecuteImportRowsResult result;
  result.ok = false;
  result.operation_id = "dml.execute_import_rows";
  result.diagnostics = insert_result.diagnostics;
  result.unsupported_features = insert_result.unsupported_features;
  result.evidence = insert_result.evidence;
  result.dml_summary = insert_result.dml_summary;
  result.evidence.push_back({"import_execution_refused_by", "dml.insert_rows"});
  result.accepted_rows = 0;
  result.inserted_rows = insert_result.inserted_count;
  result.row_uuids = insert_result.row_uuids;
  return result;
}

EngineExecuteImportRowsResult ImportExecutionFailureFromDirectPhysical(
    const dml::DirectPhysicalBulkAppendResult& direct_result,
    std::vector<EngineEvidenceReference> evidence) {
  EngineExecuteImportRowsResult result;
  result.ok = false;
  result.operation_id = "dml.execute_import_rows";
  result.diagnostics = direct_result.diagnostics;
  result.unsupported_features = direct_result.unsupported_features;
  result.evidence = std::move(evidence);
  result.evidence.insert(result.evidence.end(),
                         direct_result.evidence.begin(),
                         direct_result.evidence.end());
  result.dml_summary = direct_result.dml_summary;
  result.evidence.push_back({"import_execution_refused_by",
                             "dml.direct_physical_bulk_append"});
  result.accepted_rows = 0;
  result.inserted_rows = direct_result.inserted_rows;
  result.rejected_rows = direct_result.rejected_rows;
  result.row_uuids = direct_result.row_uuids;
  return result;
}

EngineExecuteImportRowsResult ImportExecutionFailureFromRejectTargetInsert(
    const EngineInsertRowsResult& insert_result,
    EngineApiU64 accepted_rows,
    EngineApiU64 rejected_rows,
    std::vector<EngineUuid> row_uuids,
    EngineResultShape result_shape,
    std::vector<EngineEvidenceReference> evidence) {
  EngineExecuteImportRowsResult result;
  result.ok = false;
  result.operation_id = "dml.execute_import_rows";
  result.diagnostics = insert_result.diagnostics;
  result.unsupported_features = insert_result.unsupported_features;
  result.accepted_rows = accepted_rows;
  result.inserted_rows = accepted_rows;
  result.rejected_rows = rejected_rows;
  result.row_uuids = std::move(row_uuids);
  result.result_shape = std::move(result_shape);
  result.evidence = std::move(evidence);
  result.evidence.insert(result.evidence.end(), insert_result.evidence.begin(), insert_result.evidence.end());
  result.evidence.push_back({"import_execution_refused_by", "reject_target_insert"});
  return result;
}

EngineExecuteImportRowsResult ImportExecutionFailureRejectLimitExceeded(
    EngineApiU64 accepted_rows,
    EngineApiU64 rejected_rows,
    std::vector<EngineUuid> row_uuids,
    EngineResultShape result_shape,
    std::vector<EngineEvidenceReference> evidence) {
  EngineExecuteImportRowsResult result;
  result.ok = false;
  result.operation_id = "dml.execute_import_rows";
  result.diagnostics.push_back(
      MakeInvalidRequestDiagnostic("dml.execute_import_rows", "reject_limit_exceeded"));
  result.accepted_rows = accepted_rows;
  result.inserted_rows = accepted_rows;
  result.rejected_rows = rejected_rows;
  result.row_uuids = std::move(row_uuids);
  result.result_shape = std::move(result_shape);
  result.evidence = std::move(evidence);
  result.evidence.push_back({"import_execution_refused_by", "reject_limit"});
  result.evidence.push_back({"import_reject_limit_exceeded", std::to_string(rejected_rows)});
  return result;
}

EnginePlanImportRowsRequest MakePlanRequest(const EngineExecuteImportRowsRequest& request) {
  EnginePlanImportRowsRequest plan;
  plan.context = request.context;
  plan.operation_id = "dml.plan_import_rows";
  plan.target_table = request.target_table;
  plan.source = request.source;
  plan.format = request.format;
  plan.column_mappings = request.column_mappings;
  plan.import_policy = request.import_policy;
  plan.option_envelopes = request.option_envelopes;
  plan.diagnostic_options = request.diagnostic_options;
  return plan;
}

EngineNormalizeImportRejectModelRequest MakeRejectModelRequest(const EngineExecuteImportRowsRequest& request) {
  EngineNormalizeImportRejectModelRequest reject;
  reject.context = request.context;
  reject.operation_id = "dml.normalize_import_reject_model";
  reject.target_table = request.target_table;
  reject.reject_policy = request.import_policy;
  reject.include_payload_reference_columns = request.import_policy.reject_payload_policy != "diagnostic_only";
  reject.option_envelopes = request.option_envelopes;
  reject.diagnostic_options = request.diagnostic_options;
  return reject;
}

EngineNormalizeImportCheckpointRequest MakeCheckpointModelRequest(const EngineExecuteImportRowsRequest& request) {
  EngineNormalizeImportCheckpointRequest checkpoint;
  checkpoint.context = request.context;
  checkpoint.operation_id = "dml.normalize_import_checkpoint_model";
  checkpoint.target_table = request.target_table;
  checkpoint.checkpoint_policy = request.checkpoint_policy;
  checkpoint.source_fingerprint = request.source.source_fingerprint;
  checkpoint.source_position = request.source.source_position;
  checkpoint.option_envelopes = request.option_envelopes;
  checkpoint.diagnostic_options = request.diagnostic_options;
  return checkpoint;
}

std::span<const EngineRowValue> BorrowRows(const std::vector<EngineRowValue>& rows,
                                           std::size_t first,
                                           std::size_t count) {
  if (first >= rows.size()) {
    return {};
  }
  const std::size_t end = std::min(rows.size(), first + count);
  return std::span<const EngineRowValue>(rows.data() + first, end - first);
}

EngineInsertRowsRequest MakeInsertRequestForRows(const EngineExecuteImportRowsRequest& request,
                                                 const EnginePlanImportRowsResult& plan,
                                                 std::span<const EngineRowValue> rows) {
  EngineInsertRowsRequest insert;
  insert.context = request.context;
  insert.operation_id = "dml.insert_rows";
  insert.target_table = request.target_table;
  insert.borrowed_input_rows = rows;
  insert.require_generated_row_uuid = request.require_generated_row_uuid;
  insert.estimated_row_count = static_cast<EngineApiU64>(insert.borrowed_input_rows.size());
  insert.insert_mode = plan.normalized_insert_mode;
  insert.duplicate_mode = request.duplicate_mode;
  insert.strict_bulk_load_requested = request.import_policy.strict_bulk_load_requested;
  insert.reference_unique_checks_relaxed = false;
  insert.reference_foreign_key_checks_relaxed = false;
  insert.option_envelopes = StripWriteResultPolicyOptions(request.option_envelopes);
  insert.diagnostic_options = request.diagnostic_options;
  return insert;
}

EngineTypedValue RejectTextValue(std::string value, bool is_null = false) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = is_null ? "type=text;nullable=true" : "type=text;nullable=false";
  typed.encoded_value = std::move(value);
  typed.is_null = is_null;
  return typed;
}

EngineTypedValue RejectU64Value(EngineApiU64 value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "uint64";
  typed.descriptor.encoded_descriptor = "type=uint64;nullable=false";
  typed.encoded_value = std::to_string(value);
  return typed;
}

EngineTypedValue RejectBoolValue(bool value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "boolean";
  typed.descriptor.encoded_descriptor = "type=boolean;nullable=false";
  typed.encoded_value = value ? "true" : "false";
  return typed;
}

EngineRowValue MakeRejectDiagnosticRow(const EngineExecuteImportRowsRequest& request,
                                       EngineApiU64 source_row_number,
                                       const EngineApiDiagnostic& diagnostic,
                                       bool include_payload_reference_columns) {
  EngineRowValue row;
  row.requested_row_uuid.canonical = "import-reject-" + std::to_string(source_row_number);
  row.fields.push_back({"source_row_number", RejectU64Value(source_row_number)});
  row.fields.push_back({"source_position", RejectTextValue(request.source.source_position, request.source.source_position.empty())});
  row.fields.push_back({"target_table_uuid", RejectTextValue(request.target_table.uuid.canonical)});
  row.fields.push_back({"target_column", RejectTextValue({}, true)});
  row.fields.push_back({"diagnostic_code", RejectTextValue(diagnostic.code)});
  row.fields.push_back({"message_key", RejectTextValue(diagnostic.message_key)});
  row.fields.push_back({"diagnostic_detail", RejectTextValue(diagnostic.detail, diagnostic.detail.empty())});
  row.fields.push_back({"rejected_value_digest", RejectTextValue({}, true)});
  row.fields.push_back({"value_redacted", RejectBoolValue(true)});
  row.fields.push_back({"policy_name", RejectTextValue(request.import_policy.reject_mode)});
  row.fields.push_back({"audit_evidence_id", RejectTextValue("import_reject:" + std::to_string(source_row_number))});
  if (include_payload_reference_columns) {
    row.fields.push_back({"payload_reference_uuid", RejectTextValue(row.requested_row_uuid.canonical)});
    row.fields.push_back({"payload_encryption_profile", RejectTextValue(request.import_policy.reject_payload_policy)});
  }
  return row;
}

void AppendRows(EngineResultShape* destination, const EngineResultShape& source) {
  if (destination->result_kind.empty()) {
    destination->result_kind = source.result_kind.empty() ? "import_rows" : source.result_kind;
  }
  destination->rows.insert(destination->rows.end(), source.rows.begin(), source.rows.end());
}

void AppendImportNormalizationEvidence(std::vector<EngineEvidenceReference>* evidence,
                                       const EnginePlanImportRowsResult& plan,
                                       const EngineNormalizeImportRejectModelResult& reject_model,
                                       const EngineNormalizeImportCheckpointResult& checkpoint_model) {
  evidence->insert(evidence->end(), plan.evidence.begin(), plan.evidence.end());
  evidence->insert(evidence->end(), reject_model.evidence.begin(), reject_model.evidence.end());
  evidence->insert(evidence->end(), checkpoint_model.evidence.begin(), checkpoint_model.evidence.end());
}

EngineApiU64 EffectiveRejectLimit(const EngineExecuteImportRowsRequest& request,
                                  const EngineNormalizeImportRejectModelResult& reject_model) {
  if (reject_model.effective_reject_limit_rows != 0) {
    return reject_model.effective_reject_limit_rows;
  }
  if (reject_model.effective_reject_limit_percent > 0.0) {
    const auto computed = static_cast<EngineApiU64>(
        (static_cast<double>(request.canonical_rows.size()) *
         reject_model.effective_reject_limit_percent) /
        100.0);
    return std::max<EngineApiU64>(1, computed);
  }
  return 0;
}

bool RejectTargetMaterializationRequired(const EngineExecuteImportRowsRequest& request) {
  return request.import_policy.reject_mode == "reject_table" ||
         request.import_policy.reject_mode == "quarantine" ||
         !request.import_policy.reject_target.uuid.canonical.empty();
}

bool ContainsText(const std::string& value, const std::string& needle) {
  return value.find(needle) != std::string::npos;
}

bool StartsWithText(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool IsRejectableInsertDiagnostic(const EngineApiDiagnostic& diagnostic) {
  return ContainsText(diagnostic.detail, "unique_index_duplicate") ||
         ContainsText(diagnostic.detail, "domain.validate_value") ||
         StartsWithText(diagnostic.code, "CLI.CONSTRAINT_") ||
         StartsWithText(diagnostic.message_key, "constraint.");
}

void AddCopyAppendBatchEvidence(const CopyAppendBatchPolicy& policy,
                                EngineApiU64 actual_batch_rows,
                                EngineApiU64 batch_count,
                                EngineApiU64 singleton_fallback_batches,
                                std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back({"copy_append_batching", policy.enabled ? "enabled" : "disabled"});
  evidence->push_back({"copy_append_batch_rows", std::to_string(actual_batch_rows)});
  if (policy.enabled && actual_batch_rows != policy.max_rows) {
    evidence->push_back({"copy_append_batch_policy_rows", std::to_string(policy.max_rows)});
  }
  evidence->push_back({"copy_append_batch_count", std::to_string(batch_count)});
  if (singleton_fallback_batches != 0) {
    evidence->push_back({"copy_append_singleton_fallback_batches",
                         std::to_string(singleton_fallback_batches)});
  }
}

void AddImportRowWindowEvidence(EngineApiU64 window_count,
                                std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back({"import_row_window_route", "borrowed_span"});
  evidence->push_back({"import_row_vector_copies", "0"});
  evidence->push_back({"import_row_window_count", std::to_string(window_count)});
}

void AddImportExecutionCompletionEvidence(std::vector<EngineEvidenceReference>* evidence) {
  evidence->push_back({"import_execution_entrypoint", "dml.execute_import_rows"});
  evidence->push_back({"import_plan_consumed", "true"});
  evidence->push_back({"import_planning_only", "false"});
  evidence->push_back({"import_execution_row_persistence_claimed", "true"});
  evidence->push_back({"import_execution_row_execution_completed", "true"});
  evidence->push_back({"parser_finality_authority", "false"});
  evidence->push_back({"reference_finality_authority", "false"});
}

EngineInsertRowsRequest MakeRejectTargetInsertRequest(const EngineExecuteImportRowsRequest& request,
                                                      EngineRowValue reject_row) {
  EngineInsertRowsRequest insert;
  insert.context = request.context;
  insert.operation_id = "dml.insert_rows";
  insert.target_table = request.import_policy.reject_target;
  insert.input_rows.push_back(std::move(reject_row));
  insert.require_generated_row_uuid = true;
  insert.estimated_row_count = 1;
  insert.insert_mode = "singleton";
  insert.duplicate_mode = "error";
  insert.strict_bulk_load_requested = false;
  insert.reference_unique_checks_relaxed = false;
  insert.reference_foreign_key_checks_relaxed = false;
  insert.option_envelopes = StripWriteResultPolicyOptions(request.option_envelopes);
  insert.diagnostic_options = request.diagnostic_options;
  return insert;
}

bool DirectPhysicalLaneEnabled(const EngineExecuteImportRowsRequest& request) {
  const std::string direct = LowerAscii(ImportOptionValue(request, "direct_physical_lane"));
  if (IsFalsyOptionValue(direct)) {
    return false;
  }
  const std::string copy_direct =
      LowerAscii(ImportOptionValue(request, "copy_direct_physical_lane"));
  if (IsFalsyOptionValue(copy_direct)) {
    return false;
  }
  return true;
}

dml::DirectPhysicalBulkAppendRequest MakeDirectPhysicalRequestForRows(
    const EngineExecuteImportRowsRequest& request,
    std::span<const EngineRowValue> rows) {
  dml::DirectPhysicalBulkAppendRequest direct;
  direct.context = request.context;
  direct.target_table = request.target_table;
  direct.borrowed_input_rows = rows;
  direct.option_envelopes = StripWriteResultPolicyOptions(request.option_envelopes);
  direct.diagnostic_options = request.diagnostic_options;
  direct.estimated_row_count = static_cast<EngineApiU64>(rows.size());
  direct.lane_operation = "copy_import";
  direct.duplicate_mode = request.duplicate_mode;
  direct.require_generated_row_uuid = request.require_generated_row_uuid;
  direct.strict_bulk_load_requested = request.import_policy.strict_bulk_load_requested;
  direct.direct_lane_enabled = DirectPhysicalLaneEnabled(request);
  return direct;
}

EngineExecuteImportRowsResult ExecuteImportRowsDirectPhysical(
    const EngineExecuteImportRowsRequest& request,
    const EnginePlanImportRowsResult& plan,
    const EngineNormalizeImportRejectModelResult& reject_model,
    const EngineNormalizeImportCheckpointResult& checkpoint_model,
    const CopyAppendBatchPolicy& copy_append_policy) {
  std::vector<EngineEvidenceReference> evidence;
  AppendImportNormalizationEvidence(&evidence, plan, reject_model, checkpoint_model);

  std::vector<EngineUuid> row_uuids;
  EngineResultShape result_shape;
  EngineDmlSummaryCounters summary;
  EngineApiU64 inserted_rows = 0;
  EngineApiU64 row_window_count = 0;
  EngineApiU64 batch_count = 0;
  const EngineApiU64 effective_batch_rows =
      copy_append_policy.enabled
          ? static_cast<EngineApiU64>(request.canonical_rows.size())
          : 1;

  for (std::size_t row_index = 0; row_index < request.canonical_rows.size();) {
    const std::size_t row_count = static_cast<std::size_t>(
        std::min<EngineApiU64>(
            static_cast<EngineApiU64>(request.canonical_rows.size() - row_index),
            std::max<EngineApiU64>(1, effective_batch_rows)));
    ++row_window_count;
    ++batch_count;
    const auto direct = dml::ExecuteDirectPhysicalBulkAppend(
        MakeDirectPhysicalRequestForRows(
            request,
            BorrowRows(request.canonical_rows, row_index, row_count)));
    if (!direct.ok) {
      return ImportExecutionFailureFromDirectPhysical(direct, std::move(evidence));
    }
    inserted_rows += direct.inserted_rows;
    row_uuids.insert(row_uuids.end(),
                     direct.row_uuids.begin(),
                     direct.row_uuids.end());
    AppendRows(&result_shape, direct.result_shape);
    evidence.insert(evidence.end(), direct.evidence.begin(), direct.evidence.end());
    AddDmlSummaryCounters(&summary, direct.dml_summary);
    row_index += row_count;
  }

  EngineExecuteImportRowsResult result;
  result.ok = true;
  result.operation_id = "dml.execute_import_rows";
  result.accepted_rows = static_cast<EngineApiU64>(request.canonical_rows.size());
  result.inserted_rows = inserted_rows;
  result.rejected_rows = 0;
  result.row_uuids = std::move(row_uuids);
  result.result_shape = std::move(result_shape);
  result.normalized_source_kind = plan.normalized_source_kind;
  result.normalized_format_family = plan.normalized_format_family;
  result.normalized_insert_mode = plan.normalized_insert_mode;
  result.normalized_reject_mode = reject_model.normalized_reject_mode;
  result.normalized_checkpoint_mode = checkpoint_model.normalized_checkpoint_mode;
  result.delegated_to_insert_rows = false;
  result.checkpoint_model_normalized = true;
  result.reject_model_normalized = true;
  result.primary_object = request.target_table;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.dml_summary = std::move(summary);
  result.dml_summary.rows_changed = result.inserted_rows;
  result.evidence = std::move(evidence);
  AddImportRowWindowEvidence(row_window_count, &result.evidence);
  AddImportExecutionCompletionEvidence(&result.evidence);
  AddCopyAppendBatchEvidence(copy_append_policy,
                             effective_batch_rows,
                             batch_count,
                             0,
                             &result.evidence);
  result.evidence.push_back({"import_execution", "direct_physical"});
  result.evidence.push_back({"import_execution_delegate", "none"});
  result.evidence.push_back({"import_canonical_rows", std::to_string(result.accepted_rows)});
  result.evidence.push_back({"import_inserted_rows", std::to_string(result.inserted_rows)});
  AddDmlSummaryEvidence(&result);
  return result;
}

struct RejectBisectionState {
  EngineResultShape* result_shape = nullptr;
  std::vector<EngineUuid>* row_uuids = nullptr;
  std::vector<EngineEvidenceReference>* evidence = nullptr;
  EngineApiU64* accepted_rows = nullptr;
  EngineApiU64* rejected_rows = nullptr;
  EngineApiU64* row_window_count = nullptr;
  EngineApiU64* split_count = nullptr;
  EngineApiU64* terminal_singleton_count = nullptr;
  EngineApiU64* batch_attempt_count = nullptr;
  EngineDmlSummaryCounters* dml_summary = nullptr;
  EngineApiU64 reject_limit = 0;
  bool include_payload_reference_columns = false;
};

struct RejectBisectionResult {
  bool ok = true;
  EngineExecuteImportRowsResult failure;
};

RejectBisectionResult ExecuteRejectWindowByBisection(
    const EngineExecuteImportRowsRequest& request,
    const EnginePlanImportRowsResult& plan,
    std::size_t row_index,
    std::size_t row_count,
    RejectBisectionState* state) {
  ++(*state->row_window_count);
  ++(*state->batch_attempt_count);
  const EngineInsertRowsResult inserted = EngineInsertRows(MakeInsertRequestForRows(
      request, plan, BorrowRows(request.canonical_rows, row_index, row_count)));
  if (inserted.ok) {
    *state->accepted_rows += static_cast<EngineApiU64>(row_count);
    state->row_uuids->insert(state->row_uuids->end(),
                             inserted.row_uuids.begin(),
                             inserted.row_uuids.end());
    AppendRows(state->result_shape, inserted.result_shape);
    state->evidence->insert(state->evidence->end(),
                            inserted.evidence.begin(),
                            inserted.evidence.end());
    AddDmlSummaryCounters(state->dml_summary, inserted.dml_summary);
    return {};
  }

  if (!inserted.diagnostics.empty() &&
      !IsRejectableInsertDiagnostic(inserted.diagnostics.front())) {
    RejectBisectionResult result;
    result.ok = false;
    result.failure = ImportExecutionFailureFromInsert(inserted);
    return result;
  }

  if (row_count > 1) {
    ++(*state->split_count);
    const std::size_t left_count = row_count / 2;
    const std::size_t right_count = row_count - left_count;
    auto left = ExecuteRejectWindowByBisection(
        request, plan, row_index, left_count, state);
    if (!left.ok) {
      return left;
    }
    return ExecuteRejectWindowByBisection(
        request, plan, row_index + left_count, right_count, state);
  }

  ++(*state->terminal_singleton_count);
  ++(*state->rejected_rows);
  if (state->reject_limit != 0 && *state->rejected_rows > state->reject_limit) {
    RejectBisectionResult result;
    result.ok = false;
    result.failure = ImportExecutionFailureRejectLimitExceeded(
        *state->accepted_rows,
        *state->rejected_rows,
        std::move(*state->row_uuids),
        std::move(*state->result_shape),
        std::move(*state->evidence));
    return result;
  }

  const EngineApiDiagnostic diagnostic = inserted.diagnostics.empty()
                                             ? MakeInvalidRequestDiagnostic("dml.insert_rows", "row_rejected")
                                             : inserted.diagnostics.front();
  EngineRowValue reject_row = MakeRejectDiagnosticRow(
      request,
      static_cast<EngineApiU64>(row_index + 1),
      diagnostic,
      state->include_payload_reference_columns);
  const EngineRowValue result_reject_row = reject_row;
  if (RejectTargetMaterializationRequired(request)) {
    const EngineInsertRowsResult reject_inserted =
        EngineInsertRows(MakeRejectTargetInsertRequest(request, std::move(reject_row)));
    if (!reject_inserted.ok) {
      RejectBisectionResult result;
      result.ok = false;
      result.failure = ImportExecutionFailureFromRejectTargetInsert(
          reject_inserted,
          *state->accepted_rows,
          *state->rejected_rows,
          std::move(*state->row_uuids),
          std::move(*state->result_shape),
          std::move(*state->evidence));
      return result;
    }
    state->evidence->insert(state->evidence->end(),
                            reject_inserted.evidence.begin(),
                            reject_inserted.evidence.end());
    AddDmlSummaryCounters(state->dml_summary, reject_inserted.dml_summary);
    state->evidence->push_back({"import_reject_materialization", "reject_target"});
  } else {
    state->evidence->push_back({"import_reject_materialization", "result_shape"});
  }
  if (state->result_shape->result_kind.empty()) {
    state->result_shape->result_kind = "import_rows";
  }
  state->result_shape->rows.push_back(result_reject_row);
  return {};
}

}  // namespace

EngineExecuteImportRowsResult EngineExecuteImportRows(const EngineExecuteImportRowsRequest& request) {
  if (request.context.local_transaction_id == 0) {
    return ImportExecutionFailure("local_transaction_id_required");
  }
  if (!request.localized_names.empty()) {
    return ImportExecutionFailure("localized_names_not_allowed_engine_boundary");
  }
  if (request.target_table.uuid.canonical.empty()) {
    return ImportExecutionFailure("target_table_uuid_required");
  }
  if (request.canonical_rows.empty()) {
    return ImportExecutionFailure("canonical_rows_required");
  }
  const auto write_result_policy =
      ResolveWriteResultPolicy(request, "dml.execute_import_rows");
  if (!write_result_policy.ok) {
    auto failure = ImportExecutionFailure("write_result_policy_refused");
    failure.diagnostics.clear();
    failure.diagnostics.push_back(write_result_policy.diagnostic);
    AddWriteResultPolicyRefusalEvidence(write_result_policy, &failure);
    return failure;
  }
  const CopyAppendBatchPolicy copy_append_policy =
      ResolveCopyAppendBatchPolicy(request);

  const EnginePlanImportRowsResult plan = EnginePlanImportRows(MakePlanRequest(request));
  if (!plan.ok) {
    return ImportExecutionFailureFromPlan(plan);
  }

  const EngineNormalizeImportRejectModelResult reject_model =
      EngineNormalizeImportRejectModel(MakeRejectModelRequest(request));
  if (!reject_model.ok) {
    return ImportExecutionFailureFromRejectModel(reject_model);
  }

  const EngineNormalizeImportCheckpointResult checkpoint_model =
      EngineNormalizeImportCheckpointModel(MakeCheckpointModelRequest(request));
  if (!checkpoint_model.ok) {
    return ImportExecutionFailureFromCheckpointModel(checkpoint_model);
  }

  if (reject_model.normalized_reject_mode != "fail_fast") {
    std::vector<EngineEvidenceReference> evidence;
    AppendImportNormalizationEvidence(&evidence, plan, reject_model, checkpoint_model);
    EngineResultShape result_shape;
    std::vector<EngineUuid> row_uuids;
    EngineApiU64 accepted_rows = 0;
    EngineApiU64 rejected_rows = 0;
    EngineApiU64 row_window_count = 0;
    EngineApiU64 bisection_split_count = 0;
    EngineApiU64 bisection_terminal_singleton_count = 0;
    EngineApiU64 bisection_batch_attempt_count = 0;
    EngineDmlSummaryCounters dml_summary;
    const EngineApiU64 reject_limit = EffectiveRejectLimit(request, reject_model);
    const bool include_payload_reference_columns =
        reject_model.error_row_schema.columns.size() >
        BuildDefaultImportErrorRowSchema(false).columns.size();

    for (std::size_t row_index = 0; row_index < request.canonical_rows.size();) {
      const EngineApiU64 effective_batch_rows =
          copy_append_policy.enabled
              ? std::max<EngineApiU64>(1, copy_append_policy.max_rows)
              : 1;
      const std::size_t row_count = static_cast<std::size_t>(
          std::min<EngineApiU64>(
              static_cast<EngineApiU64>(request.canonical_rows.size() - row_index),
              effective_batch_rows));
      const EngineApiU64 attempts_before = bisection_batch_attempt_count;
      const EngineApiU64 splits_before = bisection_split_count;
      const EngineApiU64 terminal_singletons_before = bisection_terminal_singleton_count;
      RejectBisectionState bisection_state;
      bisection_state.result_shape = &result_shape;
      bisection_state.row_uuids = &row_uuids;
      bisection_state.evidence = &evidence;
      bisection_state.accepted_rows = &accepted_rows;
      bisection_state.rejected_rows = &rejected_rows;
      bisection_state.row_window_count = &row_window_count;
      bisection_state.split_count = &bisection_split_count;
      bisection_state.terminal_singleton_count = &bisection_terminal_singleton_count;
      bisection_state.batch_attempt_count = &bisection_batch_attempt_count;
      bisection_state.dml_summary = &dml_summary;
      bisection_state.reject_limit = reject_limit;
      bisection_state.include_payload_reference_columns = include_payload_reference_columns;

      auto bisection = ExecuteRejectWindowByBisection(
          request, plan, row_index, row_count, &bisection_state);
      if (!bisection.ok) {
        return bisection.failure;
      }
      const EngineApiU64 batch_attempts =
          bisection_batch_attempt_count - attempts_before;
      const EngineApiU64 split_delta = bisection_split_count - splits_before;
      const EngineApiU64 terminal_singleton_delta =
          bisection_terminal_singleton_count - terminal_singletons_before;
      const EngineApiU64 singleton_fallback_batches =
          (split_delta != 0 || terminal_singleton_delta != 0) ? 1 : 0;
      AddCopyAppendBatchEvidence(copy_append_policy,
                                 static_cast<EngineApiU64>(row_count),
                                 batch_attempts,
                                 singleton_fallback_batches,
                                 &evidence);
      row_index += row_count;
    }

    EngineExecuteImportRowsResult result;
    result.ok = true;
    result.operation_id = "dml.execute_import_rows";
    result.accepted_rows = accepted_rows;
    result.inserted_rows = accepted_rows;
    result.rejected_rows = rejected_rows;
    result.row_uuids = std::move(row_uuids);
    result.result_shape = std::move(result_shape);
    result.normalized_source_kind = plan.normalized_source_kind;
    result.normalized_format_family = plan.normalized_format_family;
    result.normalized_insert_mode = plan.normalized_insert_mode;
    result.normalized_reject_mode = reject_model.normalized_reject_mode;
    result.normalized_checkpoint_mode = checkpoint_model.normalized_checkpoint_mode;
    result.delegated_to_insert_rows = true;
    result.checkpoint_model_normalized = true;
    result.reject_model_normalized = true;
    result.primary_object = request.target_table;
    result.local_transaction_id = request.context.local_transaction_id;
    result.transaction_uuid = request.context.transaction_uuid;
    result.dml_summary = std::move(dml_summary);
    result.dml_summary.rows_changed = result.inserted_rows;
    result.evidence = std::move(evidence);
    result.evidence.push_back({"import_execution", "delegated_to_dml.insert_rows"});
    AddImportExecutionCompletionEvidence(&result.evidence);
    result.evidence.push_back({"import_canonical_rows", std::to_string(request.canonical_rows.size())});
    result.evidence.push_back({"import_inserted_rows", std::to_string(result.inserted_rows)});
    result.evidence.push_back({"import_rejected_rows", std::to_string(result.rejected_rows)});
    result.evidence.push_back({"copy_append_reject_fallback", "bisection"});
    result.evidence.push_back({"copy_append_bisection_split_count",
                               std::to_string(bisection_split_count)});
    result.evidence.push_back({"copy_append_bisection_terminal_singleton_count",
                               std::to_string(bisection_terminal_singleton_count)});
    result.evidence.push_back({"copy_append_bisection_batch_attempt_count",
                               std::to_string(bisection_batch_attempt_count)});
    AddImportRowWindowEvidence(row_window_count, &result.evidence);
    AddDmlSummaryEvidence(&result);
    ApplyWriteResultPolicy(write_result_policy, &result);
    return result;
  }

  auto result = ExecuteImportRowsDirectPhysical(request,
                                                plan,
                                                reject_model,
                                                checkpoint_model,
                                                copy_append_policy);
  if (result.ok) {
    ApplyWriteResultPolicy(write_result_policy, &result);
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
