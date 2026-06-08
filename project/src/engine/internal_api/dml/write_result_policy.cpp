// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/write_result_policy.hpp"

#include "api_diagnostics.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

EngineTypedValue PolicyTextValue(std::string value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text;nullable=false";
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  return typed;
}

EngineTypedValue PolicyU64Value(EngineApiU64 value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "uint64";
  typed.descriptor.encoded_descriptor = "type=uint64;nullable=false";
  typed.encoded_value = std::to_string(value);
  typed.is_null = false;
  return typed;
}

bool IsPolicyKey(const std::string& key) {
  return key == "write_result_policy" ||
         key == "result_payload_policy" ||
         key == "odf_048.write_result_policy" ||
         key == "odf048.write_result_policy" ||
         key == "write.result_policy";
}

bool ParseOption(const std::string& option,
                 std::string* key,
                 std::string* value) {
  const auto equals = option.find('=');
  const auto colon = option.find(':');
  const auto separator =
      equals == std::string::npos
          ? colon
          : (colon == std::string::npos ? equals : std::min(equals, colon));
  if (separator == std::string::npos) {
    return false;
  }
  *key = option.substr(0, separator);
  *value = option.substr(separator + 1);
  return true;
}

EngineWriteResultPolicyResolution InvalidPolicy(
    const std::string& operation_id,
    const std::string& detail) {
  EngineWriteResultPolicyResolution result;
  result.ok = false;
  result.explicitly_supplied = true;
  result.diagnostic =
      MakeInvalidRequestDiagnostic(operation_id, "write_result_policy_" + detail);
  return result;
}

EngineWriteResultPolicyResolution PolicyFromValue(
    const std::string& operation_id,
    std::string value,
    std::string source_key) {
  EngineWriteResultPolicyResolution result;
  result.explicitly_supplied = true;
  result.source_key = std::move(source_key);
  value = LowerAscii(std::move(value));
  if (value == "full" || value == "default") {
    value = "full_payload";
  }
  result.policy_name = value;
  if (value == "full_payload") {
    result.policy = EngineWriteResultPolicy::full_payload;
  } else if (value == "return_none") {
    result.policy = EngineWriteResultPolicy::return_none;
  } else if (value == "ids_only") {
    result.policy = EngineWriteResultPolicy::ids_only;
  } else if (value == "summary_only") {
    result.policy = EngineWriteResultPolicy::summary_only;
  } else if (value == "changed_fields") {
    result.policy = EngineWriteResultPolicy::changed_fields;
  } else {
    return InvalidPolicy(operation_id, "unsupported:" + value);
  }
  return result;
}

bool IdentifierFieldName(const std::string& name) {
  return name == "id" ||
         name == "row_uuid" ||
         name == "object_uuid" ||
         name == "key_uuid" ||
         name == "document_uuid" ||
         name == "generation_uuid" ||
         name == "table_or_collection_uuid" ||
         (name.size() > 5 && name.substr(name.size() - 5) == "_uuid");
}

bool HasField(const EngineRowValue& row, const std::string& field_name) {
  for (const auto& [name, value] : row.fields) {
    (void)value;
    if (name == field_name) {
      return true;
    }
  }
  return false;
}

void AddTextField(EngineRowValue* row,
                  std::string name,
                  std::string value) {
  row->fields.push_back({std::move(name), PolicyTextValue(std::move(value))});
}

EngineResultShape IdsOnlyShape(const EngineResultShape& original) {
  EngineResultShape shaped;
  shaped.result_kind = "write_ids_only";
  for (const auto& row : original.rows) {
    EngineRowValue out;
    out.requested_row_uuid = row.requested_row_uuid;
    if (!row.requested_row_uuid.canonical.empty()) {
      AddTextField(&out, "row_uuid", row.requested_row_uuid.canonical);
    }
    for (const auto& [name, value] : row.fields) {
      if (IdentifierFieldName(name) && !HasField(out, name)) {
        out.fields.push_back({name, value});
      }
    }
    shaped.rows.push_back(std::move(out));
  }
  return shaped;
}

EngineResultShape SummaryOnlyShape(const EngineApiResult& result,
                                   EngineApiU64 original_row_count) {
  EngineResultShape shaped;
  shaped.result_kind = "write_summary_only";
  EngineRowValue row;
  row.fields.push_back({"rows_changed",
                        PolicyU64Value(result.dml_summary.rows_changed)});
  row.fields.push_back({"visible_rows_scanned",
                        PolicyU64Value(result.dml_summary.visible_rows_scanned)});
  row.fields.push_back({"index_probes",
                        PolicyU64Value(result.dml_summary.index_probes)});
  row.fields.push_back({"append_calls",
                        PolicyU64Value(result.dml_summary.append_calls)});
  row.fields.push_back({"file_opens",
                        PolicyU64Value(result.dml_summary.file_opens)});
  row.fields.push_back({"flushes",
                        PolicyU64Value(result.dml_summary.flushes)});
  row.fields.push_back({"page_reservations",
                        PolicyU64Value(result.dml_summary.page_reservations)});
  row.fields.push_back({"result_rows_before_policy",
                        PolicyU64Value(original_row_count)});
  shaped.rows.push_back(std::move(row));
  return shaped;
}

std::string JoinFieldNames(const std::vector<std::string>& fields) {
  std::ostringstream out;
  for (const auto& field : fields) {
    if (out.tellp() > 0) {
      out << ',';
    }
    out << field;
  }
  return out.str();
}

EngineResultShape ChangedFieldsShape(const EngineResultShape& original) {
  EngineResultShape shaped;
  shaped.result_kind = "write_changed_fields";
  for (const auto& row : original.rows) {
    EngineRowValue out;
    out.requested_row_uuid = row.requested_row_uuid;
    if (!row.requested_row_uuid.canonical.empty()) {
      AddTextField(&out, "row_uuid", row.requested_row_uuid.canonical);
    }
    std::vector<std::string> names;
    names.reserve(row.fields.size());
    for (const auto& [name, value] : row.fields) {
      names.push_back(name);
      out.fields.push_back({"changed." + name, value});
    }
    out.fields.push_back({"changed_field_count",
                          PolicyU64Value(static_cast<EngineApiU64>(names.size()))});
    AddTextField(&out, "changed_field_names", JoinFieldNames(names));
    shaped.rows.push_back(std::move(out));
  }
  return shaped;
}

}  // namespace

bool IsWriteResultPolicyOption(const std::string& option) {
  std::string key;
  std::string value;
  return ParseOption(option, &key, &value) && IsPolicyKey(key);
}

EngineWriteResultPolicyResolution ResolveWriteResultPolicyOptions(
    const std::vector<std::string>& option_envelopes,
    const std::string& operation_id) {
  EngineWriteResultPolicyResolution resolved;
  for (const auto& option : option_envelopes) {
    std::string key;
    std::string value;
    if (!ParseOption(option, &key, &value) || !IsPolicyKey(key)) {
      continue;
    }
    auto candidate = PolicyFromValue(operation_id, value, key);
    if (!candidate.ok) {
      return candidate;
    }
    if (!resolved.explicitly_supplied) {
      resolved = std::move(candidate);
      continue;
    }
    if (resolved.policy_name != candidate.policy_name) {
      return InvalidPolicy(operation_id,
                           "conflict:" + resolved.policy_name + ":" +
                               candidate.policy_name);
    }
  }
  return resolved;
}

EngineWriteResultPolicyResolution ResolveWriteResultPolicy(
    const EngineApiRequest& request,
    const std::string& operation_id) {
  return ResolveWriteResultPolicyOptions(request.option_envelopes, operation_id);
}

std::vector<std::string> StripWriteResultPolicyOptions(
    const std::vector<std::string>& option_envelopes) {
  std::vector<std::string> stripped;
  stripped.reserve(option_envelopes.size());
  for (const auto& option : option_envelopes) {
    if (!IsWriteResultPolicyOption(option)) {
      stripped.push_back(option);
    }
  }
  return stripped;
}

bool WriteResultPolicySuppressesPayloadRows(
    const EngineWriteResultPolicyResolution& policy) {
  return policy.explicitly_supplied &&
         (policy.policy == EngineWriteResultPolicy::return_none ||
          policy.policy == EngineWriteResultPolicy::summary_only);
}

void AddWriteResultPolicyRefusalEvidence(
    const EngineWriteResultPolicyResolution& policy,
    EngineApiResult* result) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back({"write_result_policy_refused", "true"});
  if (!policy.policy_name.empty()) {
    result->evidence.push_back({"write_result_policy", policy.policy_name});
  }
  if (!policy.source_key.empty()) {
    result->evidence.push_back({"write_result_policy_source", policy.source_key});
  }
}

void ApplyWriteResultPolicy(const EngineWriteResultPolicyResolution& policy,
                            EngineApiResult* result) {
  if (result == nullptr || !policy.ok || !policy.explicitly_supplied) {
    return;
  }
  const EngineApiU64 original_row_count =
      static_cast<EngineApiU64>(result->result_shape.rows.size());
  result->evidence.push_back({"write_result_policy", policy.policy_name});
  if (!policy.source_key.empty()) {
    result->evidence.push_back({"write_result_policy_source", policy.source_key});
  }
  result->evidence.push_back({"write_result_policy_applied", "true"});
  result->evidence.push_back({"write_result_policy_result_rows_before",
                              std::to_string(original_row_count)});
  switch (policy.policy) {
    case EngineWriteResultPolicy::full_payload:
      return;
    case EngineWriteResultPolicy::return_none:
      result->result_shape.result_kind = "write_return_none";
      result->result_shape.columns.clear();
      result->result_shape.rows.clear();
      return;
    case EngineWriteResultPolicy::ids_only:
      result->result_shape = IdsOnlyShape(result->result_shape);
      return;
    case EngineWriteResultPolicy::summary_only:
      result->result_shape = SummaryOnlyShape(*result, original_row_count);
      return;
    case EngineWriteResultPolicy::changed_fields:
      result->result_shape = ChangedFieldsShape(result->result_shape);
      return;
  }
}

}  // namespace scratchbird::engine::internal_api
