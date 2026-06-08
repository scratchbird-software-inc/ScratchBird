// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/constraint_enforcement.hpp"

#include "api_diagnostics.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace scratchbird::engine::internal_api {
namespace {

constexpr const char* kOkCode = "SB_ENGINE_API_OK";
constexpr const char* kOkKey = "engine.api.ok";

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

std::string TrimAscii(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool IsNullValue(const std::string& value) {
  return value == "<NULL>";
}

bool HasField(const std::vector<std::pair<std::string, std::string>>& values,
              const std::string& field) {
  for (const auto& [name, ignored] : values) {
    (void)ignored;
    if (name == field) { return true; }
  }
  return false;
}

std::string FieldValue(const std::vector<std::pair<std::string, std::string>>& values,
                       const std::string& field) {
  for (const auto& [name, value] : values) {
    if (name == field) { return value; }
  }
  return {};
}

void UpsertField(std::vector<std::pair<std::string, std::string>>* values,
                 const std::string& field,
                 const std::string& value) {
  for (auto& [name, existing] : *values) {
    if (name == field) {
      existing = value;
      return;
    }
  }
  values->push_back({field, value});
}

std::map<std::string, std::string> DescriptorFields(const std::string& descriptor) {
  std::map<std::string, std::string> fields;
  for (const auto& raw_part : Split(descriptor, ';')) {
    const std::string part = TrimAscii(raw_part);
    if (part.empty()) { continue; }
    const auto pos = part.find('=');
    if (pos == std::string::npos) {
      fields[LowerAscii(part)] = "true";
      continue;
    }
    fields[LowerAscii(TrimAscii(part.substr(0, pos)))] = TrimAscii(part.substr(pos + 1));
  }
  return fields;
}

std::string DescriptorText(const std::map<std::string, std::string>& fields) {
  std::string descriptor;
  for (const auto& [key, value] : fields) {
    if (!descriptor.empty()) { descriptor.push_back(';'); }
    descriptor += key;
    if (!value.empty() && value != "true") {
      descriptor.push_back('=');
      descriptor += value;
    }
  }
  return descriptor;
}

std::string FieldOrEmpty(const std::map<std::string, std::string>& fields,
                         std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    const auto found = fields.find(key);
    if (found != fields.end()) { return found->second; }
  }
  return {};
}

bool BoolField(const std::map<std::string, std::string>& fields,
               std::initializer_list<const char*> keys,
               bool fallback = false) {
  const std::string value = LowerAscii(FieldOrEmpty(fields, keys));
  if (value.empty()) { return fallback; }
  return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool FalseField(const std::map<std::string, std::string>& fields,
                std::initializer_list<const char*> keys) {
  const std::string value = LowerAscii(FieldOrEmpty(fields, keys));
  return value == "0" || value == "false" || value == "no" || value == "off";
}

std::string ConstraintUuid(const std::map<std::string, std::string>& fields,
                           const CrudTableRecord& table,
                           const std::string& column_name,
                           const std::string& constraint_class) {
  const std::string explicit_uuid = FieldOrEmpty(fields, {"constraint_uuid", "uuid"});
  if (!explicit_uuid.empty()) { return explicit_uuid; }
  return "descriptor:" + table.table_uuid + ":" + column_name + ":" + constraint_class;
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic(kOkCode, kOkKey, {}, false);
}

std::string ConstraintDiagnosticDetail(const EngineRequestContext& context,
                                       const CrudTableRecord& table,
                                       const std::string& constraint_class,
                                       const std::string& constraint_uuid,
                                       const std::string& detail,
                                       const std::string& column_name,
                                       const std::string& key_descriptor_uuid = {},
                                       const std::string& support_uuid = {},
                                       const std::string& support_path = "table_scan") {
  std::vector<std::pair<std::string, std::string>> fields = {
      {"constraint_uuid", constraint_uuid},
      {"constraint_class", constraint_class},
      {"owner_object_uuid", table.table_uuid},
      {"key_descriptor_uuid", key_descriptor_uuid},
      {"support_uuid", support_uuid},
      {"transaction_uuid", context.transaction_uuid.canonical},
      {"operation_uuid", context.request_id},
      {"savepoint_uuid", ""},
      {"pending_check_uuid", ""},
      {"validation_run_uuid", ""},
      {"maintenance_operation_uuid", ""},
      {"validation_state", "unvalidated"},
      {"trust_state", "untrusted"},
      {"enforcement_timing", "immediate"},
      {"support_path_used", support_path},
      {"dependency_uuid", ""},
      {"donor_profile_uuid", context.donor_profile_uuid},
      {"column", column_name},
      {"detail", detail},
  };
  std::string encoded;
  for (const auto& [key, value] : fields) {
    if (!encoded.empty()) { encoded.push_back(';'); }
    encoded.append(key);
    encoded.push_back('=');
    encoded.append(value);
  }
  return encoded;
}

EngineApiDiagnostic ConstraintDiagnostic(const std::string& code,
                                         const std::string& message_key,
                                         const EngineRequestContext& context,
                                         const CrudTableRecord& table,
                                         const std::string& constraint_class,
                                         const std::string& constraint_uuid,
                                         const std::string& detail,
                                         const std::string& column_name,
                                         const std::string& key_descriptor_uuid = {},
                                         const std::string& support_uuid = {},
                                         const std::string& support_path = "table_scan") {
  return MakeEngineApiDiagnostic(
      code,
      message_key,
      ConstraintDiagnosticDetail(context,
                                 table,
                                 constraint_class,
                                 constraint_uuid,
                                 detail,
                                 column_name,
                                 key_descriptor_uuid,
                                 support_uuid,
                                 support_path),
      true);
}

bool TimingRequiresDeferredStore(const std::map<std::string, std::string>& fields) {
  const std::string timing = LowerAscii(FieldOrEmpty(fields, {"enforcement_timing", "timing"}));
  if (timing == "deferred" || timing == "transaction_end" || timing == "initially_deferred") {
    return true;
  }
  return BoolField(fields, {"deferrable", "initially_deferred"});
}

std::optional<EngineApiDiagnostic> ValidateImmediateTiming(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    const std::map<std::string, std::string>& fields,
    const std::string& column_name,
    const std::string& constraint_class) {
  if (!TimingRequiresDeferredStore(fields)) { return std::nullopt; }
  return ConstraintDiagnostic("CLI.NO_ENFORCEMENT_PATH",
                              "constraint.no_enforcement_path",
                              context,
                              table,
                              constraint_class,
                              ConstraintUuid(fields, table, column_name, constraint_class),
                              "deferred_constraint_pending_check_store_unavailable",
                              column_name,
                              {},
                              {},
                              "none");
}

std::optional<std::string> MaterializeDefault(const std::string& envelope) {
  if (envelope.empty()) { return std::nullopt; }
  if (StartsWith(envelope, "literal:")) { return envelope.substr(8); }
  if (StartsWith(envelope, "value:")) { return envelope.substr(6); }
  const std::string lower = LowerAscii(envelope);
  if (lower == "null" || envelope == "<NULL>") { return std::string("<NULL>"); }
  if (!StartsWith(lower, "sblr:") && !StartsWith(lower, "sblr_expression:") &&
      !StartsWith(lower, "generated:")) {
    return envelope;
  }
  return std::nullopt;
}

struct CheckResult {
  bool ok = false;
  bool unsupported = false;
  std::string detail;
};

bool NumberCompare(const std::string& left, const std::string& op, const std::string& right) {
  try {
    const long double l = std::stold(left);
    const long double r = std::stold(right);
    if (op == "gt") { return l > r; }
    if (op == "gte") { return l >= r; }
    if (op == "lt") { return l < r; }
    if (op == "lte") { return l <= r; }
    if (op == "eq") { return l == r; }
    if (op == "ne") { return l != r; }
  } catch (...) {
    return false;
  }
  return false;
}

CheckResult EvaluateCheckEnvelope(const std::string& envelope, const std::string& value) {
  if (envelope.empty()) { return {true, false, {}}; }
  if (StartsWith(envelope, "sblr_predicate:")) {
    return EvaluateCheckEnvelope(envelope.substr(15), value);
  }
  if (envelope == "not_null") {
    return {!IsNullValue(value), false, "check_not_null_failed"};
  }
  if (envelope == "not_empty") {
    return {!value.empty() && !IsNullValue(value), false, "check_not_empty_failed"};
  }
  const auto pos = envelope.find(':');
  if (pos == std::string::npos) {
    return {false, true, "check_constraint_requires_sblr_executor"};
  }
  const std::string op = envelope.substr(0, pos);
  const std::string rhs = envelope.substr(pos + 1);
  if (op == "eq") { return {value == rhs, false, "check_eq_failed"}; }
  if (op == "ne") { return {value != rhs, false, "check_ne_failed"}; }
  if (op == "gt" || op == "gte" || op == "lt" || op == "lte") {
    return {NumberCompare(value, op, rhs), false, "check_" + op + "_failed"};
  }
  if (op == "length_gt" || op == "length_gte" || op == "length_lt" || op == "length_lte") {
    std::size_t rhs_length = 0;
    try {
      rhs_length = static_cast<std::size_t>(std::stoull(rhs));
    } catch (...) {
      return {false, false, "check_length_rhs_invalid"};
    }
    const std::size_t length = value.size();
    if (op == "length_gt") { return {length > rhs_length, false, "check_length_gt_failed"}; }
    if (op == "length_gte") { return {length >= rhs_length, false, "check_length_gte_failed"}; }
    if (op == "length_lt") { return {length < rhs_length, false, "check_length_lt_failed"}; }
    if (op == "length_lte") { return {length <= rhs_length, false, "check_length_lte_failed"}; }
  }
  return {false, true, "check_constraint_requires_sblr_executor"};
}

std::vector<std::string> KeyColumnsForIndex(const CrudIndexRecord& index) {
  std::vector<std::string> columns;
  for (const auto& envelope : index.key_envelopes) {
    if (envelope.empty() || envelope == "unique" || envelope == "primary_key" ||
        StartsWith(envelope, "include:") || StartsWith(envelope, "where_eq:")) {
      continue;
    }
    if (StartsWith(envelope, "identity:")) {
      columns.push_back(envelope.substr(9));
    } else {
      columns.push_back(envelope);
    }
  }
  if (columns.empty() && !index.column_name.empty()) { columns.push_back(index.column_name); }
  return columns;
}

bool IndexCoversColumn(const CrudIndexRecord& index, const std::string& column_name) {
  const auto columns = KeyColumnsForIndex(index);
  return columns.size() == 1 && columns.front() == column_name;
}

std::optional<CrudIndexRecord> FindVisibleUniqueIndexForColumn(const CrudState& state,
                                                               const std::string& table_uuid,
                                                               const std::string& column_name,
                                                               std::uint64_t observer_tx) {
  for (const auto& index : VisibleCrudIndexesForTable(state, table_uuid, observer_tx)) {
    if (index.unique && IndexCoversColumn(index, column_name)) { return index; }
  }
  return std::nullopt;
}

bool AnyNullKey(const std::vector<std::string>& keys) {
  for (const auto& key : keys) {
    if (key.find("<NULL>") != std::string::npos) { return true; }
  }
  return false;
}

std::string TableColumnCacheKey(const std::string& table_uuid,
                                const std::string& column_name) {
  return table_uuid + "\n" + column_name;
}

std::string UniquePreflightProofKey(const CrudIndexRecord& index,
                                    const std::string& row_uuid,
                                    const std::string& key) {
  return index.index_uuid + "\n" + row_uuid + "\n" + key;
}

std::string ContextScopedCacheKey(const EngineRequestContext& context,
                                  const std::string& identity) {
  return std::to_string(context.local_transaction_id) + "\n" +
         std::to_string(context.snapshot_visible_through_local_transaction_id) + "\n" +
         std::to_string(context.catalog_generation_id) + "\n" +
         std::to_string(context.security_epoch) + "\n" +
         std::to_string(context.resource_epoch) + "\n" +
         std::to_string(context.name_resolution_epoch) + "\n" +
         context.database_uuid.canonical + "\n" +
         context.principal_uuid.canonical + "\n" + identity;
}

std::string TraceTagFingerprint(const EngineRequestContext& context) {
  std::vector<std::string> tags = context.trace_tags;
  std::sort(tags.begin(), tags.end());
  std::string encoded;
  for (const auto& tag : tags) {
    if (!encoded.empty()) { encoded.push_back(','); }
    encoded += tag;
  }
  return encoded;
}

ConstraintDmlProofContext MakeProofContext(const EngineRequestContext& context) {
  ConstraintDmlProofContext proof_context;
  proof_context.database_uuid = context.database_uuid.canonical;
  proof_context.transaction_uuid = context.transaction_uuid.canonical;
  proof_context.principal_uuid = context.principal_uuid.canonical;
  proof_context.isolation_level = context.transaction_isolation_level;
  proof_context.trace_tag_fingerprint = TraceTagFingerprint(context);
  proof_context.local_transaction_id = context.local_transaction_id;
  proof_context.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  proof_context.catalog_generation_id = context.catalog_generation_id;
  proof_context.security_epoch = context.security_epoch;
  proof_context.resource_epoch = context.resource_epoch;
  proof_context.name_resolution_epoch = context.name_resolution_epoch;
  proof_context.security_context_present = context.security_context_present;
  return proof_context;
}

bool SameProofContext(const ConstraintDmlProofContext& left,
                      const ConstraintDmlProofContext& right) {
  return left.database_uuid == right.database_uuid &&
         left.transaction_uuid == right.transaction_uuid &&
         left.principal_uuid == right.principal_uuid &&
         left.isolation_level == right.isolation_level &&
         left.trace_tag_fingerprint == right.trace_tag_fingerprint &&
         left.local_transaction_id == right.local_transaction_id &&
         left.snapshot_visible_through_local_transaction_id ==
             right.snapshot_visible_through_local_transaction_id &&
         left.catalog_generation_id == right.catalog_generation_id &&
         left.security_epoch == right.security_epoch &&
         left.resource_epoch == right.resource_epoch &&
         left.name_resolution_epoch == right.name_resolution_epoch &&
         left.security_context_present == right.security_context_present;
}

std::string ProofBody(const std::string& proof_kind,
                      const std::string& proof_identity) {
  return proof_kind + "\n" + proof_identity;
}

std::string ProofFullKey(const std::string& proof_body,
                         const ConstraintDmlProofContext& context) {
  return proof_body + "\nctx\n" + context.database_uuid + "\n" +
         context.transaction_uuid + "\n" + context.principal_uuid + "\n" +
         context.isolation_level + "\n" + context.trace_tag_fingerprint + "\n" +
         std::to_string(context.local_transaction_id) + "\n" +
         std::to_string(context.snapshot_visible_through_local_transaction_id) + "\n" +
         std::to_string(context.catalog_generation_id) + "\n" +
         std::to_string(context.security_epoch) + "\n" +
         std::to_string(context.resource_epoch) + "\n" +
         std::to_string(context.name_resolution_epoch) + "\n" +
         (context.security_context_present ? "security_present" : "security_absent");
}

std::string FirstProofContextMismatch(const ConstraintDmlProofContext& stored,
                                      const ConstraintDmlProofContext& current) {
  if (stored.catalog_generation_id != current.catalog_generation_id) {
    return "catalog_epoch_mismatch";
  }
  if (stored.snapshot_visible_through_local_transaction_id !=
      current.snapshot_visible_through_local_transaction_id ||
      stored.local_transaction_id != current.local_transaction_id ||
      stored.transaction_uuid != current.transaction_uuid ||
      stored.isolation_level != current.isolation_level) {
    return "visibility_epoch_mismatch";
  }
  if (stored.security_epoch != current.security_epoch ||
      stored.security_context_present != current.security_context_present ||
      stored.principal_uuid != current.principal_uuid ||
      stored.trace_tag_fingerprint != current.trace_tag_fingerprint) {
    return "security_epoch_mismatch";
  }
  if (stored.resource_epoch != current.resource_epoch) {
    return "resource_epoch_mismatch";
  }
  if (stored.name_resolution_epoch != current.name_resolution_epoch) {
    return "name_resolution_epoch_mismatch";
  }
  if (stored.database_uuid != current.database_uuid) {
    return "database_mismatch";
  }
  return "context_mismatch";
}

std::string ProofEvidenceId(const std::string& proof_kind,
                            const std::string& proof_identity) {
  const auto newline = proof_identity.find('\n');
  const std::string compact_identity =
      newline == std::string::npos ? proof_identity : proof_identity.substr(0, newline);
  return proof_kind + ":" + compact_identity;
}

bool HasIndexBackedUniquePreflightProof(
    const ConstraintDmlValidationCache* cache,
    const EngineRequestContext& context,
    const CrudIndexRecord& index,
    const std::string& row_uuid,
    const std::vector<std::string>& keys,
    std::vector<EngineEvidenceReference>* evidence = nullptr) {
  if (cache == nullptr) {
    return false;
  }
  for (const auto& key : keys) {
    if (!FindConstraintDmlProofPayload(cache,
                                       context,
                                       "unique_preflight",
                                       UniquePreflightProofKey(index, row_uuid, key),
                                       evidence)
             .has_value()) {
      return false;
    }
  }
  return true;
}

const std::vector<CrudRowVersionRecord>& CachedVisibleRowsForTable(
    ConstraintDmlValidationCache* cache,
    const CrudState& state,
    const std::string& table_uuid,
    const EngineRequestContext& context) {
  if (cache == nullptr) {
    static thread_local std::vector<CrudRowVersionRecord> uncached_rows;
    uncached_rows = VisibleCrudRowsForContext(state, table_uuid, context);
    return uncached_rows;
  }
  const std::string cache_key = ContextScopedCacheKey(context, table_uuid);
  if (cache->visible_rows_built_for_table_uuid.insert(cache_key).second) {
    cache->visible_rows_by_table_uuid[cache_key] =
        VisibleCrudRowsForContext(state, table_uuid, context);
  }
  return cache->visible_rows_by_table_uuid[cache_key];
}

const std::map<std::string, std::set<std::string>>& CachedUniqueKeyRowsForIndex(
    ConstraintDmlValidationCache* cache,
    const CrudState& state,
    const CrudTableRecord& table,
    const CrudIndexRecord& index,
    const EngineRequestContext& context) {
  if (cache == nullptr) {
    static thread_local std::map<std::string, std::set<std::string>> uncached_keys;
    uncached_keys.clear();
    for (const auto& row : VisibleCrudRowsForContext(state, table.table_uuid, context)) {
      for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
        uncached_keys[key].insert(row.row_uuid);
      }
    }
    return uncached_keys;
  }
  const std::string cache_key = ContextScopedCacheKey(context, index.index_uuid);
  if (cache->unique_key_rows_built_for_index_uuid.insert(cache_key).second) {
    auto& keys = cache->unique_key_rows_by_index_uuid[cache_key];
    for (const auto& row : CachedVisibleRowsForTable(cache, state, table.table_uuid, context)) {
      for (const auto& key : CrudIndexKeysForValues(index, row.values)) {
        keys[key].insert(row.row_uuid);
      }
    }
  }
  return cache->unique_key_rows_by_index_uuid[cache_key];
}

const std::set<std::string>& CachedColumnValues(
    ConstraintDmlValidationCache* cache,
    const CrudState& state,
    const std::string& table_uuid,
    const std::string& column_name,
    const EngineRequestContext& context) {
  const std::string key =
      ContextScopedCacheKey(context, TableColumnCacheKey(table_uuid, column_name));
  if (cache == nullptr) {
    static thread_local std::set<std::string> uncached_values;
    uncached_values.clear();
    for (const auto& row : VisibleCrudRowsForContext(state, table_uuid, context)) {
      uncached_values.insert(FieldValue(row.values, column_name));
    }
    return uncached_values;
  }
  if (cache->column_values_built_for_table_column.insert(key).second) {
    auto& values = cache->column_values_by_table_column[key];
    for (const auto& row : CachedVisibleRowsForTable(cache, state, table_uuid, context)) {
      values.insert(FieldValue(row.values, column_name));
    }
  }
  return cache->column_values_by_table_column[key];
}

std::optional<EngineApiDiagnostic> ValidateUniqueIndexNoDuplicate(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudTableRecord& table,
    const CrudIndexRecord& index,
    const std::string& row_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::map<std::string, std::string>& fields,
    const std::string& column_name,
    const std::string& constraint_class,
    bool nulls_distinct,
    ConstraintDmlValidationCache* cache) {
  const auto keys = CrudIndexKeysForValues(index, values);
  if (keys.empty()) { return std::nullopt; }
  if (nulls_distinct && AnyNullKey(keys)) { return std::nullopt; }
  if (HasIndexBackedUniquePreflightProof(cache, context, index, row_uuid, keys)) {
    return std::nullopt;
  }
  const auto& existing = CachedUniqueKeyRowsForIndex(cache, state, table, index, context);
  for (const auto& key : keys) {
    const auto found = existing.find(key);
    if (found == existing.end()) { continue; }
    for (const auto& existing_row_uuid : found->second) {
      if (existing_row_uuid == row_uuid) { continue; }
      const std::string code = constraint_class == "primary_key"
                                   ? "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION"
                                   : "CLI.CONSTRAINT_UNIQUE_VIOLATION";
      const std::string message_key = constraint_class == "primary_key"
                                          ? "constraint.primary_key.violation"
                                          : "constraint.unique.violation";
      return ConstraintDiagnostic(code,
                                  message_key,
                                  context,
                                  table,
                                  constraint_class,
                                  ConstraintUuid(fields, table, column_name, constraint_class),
                                  "duplicate_key",
                                  column_name,
                                  "key:" + index.index_uuid,
                                  index.index_uuid,
                                  "support_structure");
    }
  }
  return std::nullopt;
}

struct ForeignKeyReference {
  std::string parent_table_uuid;
  std::string parent_column;
};

std::optional<ForeignKeyReference> ParseForeignKeyReference(
    const std::map<std::string, std::string>& fields) {
  ForeignKeyReference reference;
  reference.parent_table_uuid = FieldOrEmpty(fields, {"referenced_table_uuid", "foreign_table_uuid", "foreign_table"});
  reference.parent_column = FieldOrEmpty(fields, {"referenced_column", "foreign_column", "parent_column"});
  if (!reference.parent_table_uuid.empty() && !reference.parent_column.empty()) { return reference; }
  const std::string envelope = FieldOrEmpty(fields, {"foreign_key", "references", "fk"});
  if (envelope.empty()) { return std::nullopt; }
  const auto colon = envelope.find(':');
  const auto dot = envelope.rfind('.');
  const auto open = envelope.find('(');
  const auto close = envelope.rfind(')');
  if (colon != std::string::npos) {
    reference.parent_table_uuid = envelope.substr(0, colon);
    reference.parent_column = envelope.substr(colon + 1);
  } else if (dot != std::string::npos) {
    reference.parent_table_uuid = envelope.substr(0, dot);
    reference.parent_column = envelope.substr(dot + 1);
  } else if (open != std::string::npos && close == envelope.size() - 1 && close > open + 1) {
    reference.parent_table_uuid = envelope.substr(0, open);
    reference.parent_column = envelope.substr(open + 1, close - open - 1);
  }
  if (reference.parent_table_uuid.empty() || reference.parent_column.empty()) { return std::nullopt; }
  return reference;
}

bool DescriptorDeclaresForeignKey(const std::map<std::string, std::string>& fields) {
  return !FieldOrEmpty(fields,
                       {"foreign_key",
                        "references",
                        "fk",
                        "referenced_table_uuid",
                        "foreign_table_uuid",
                        "foreign_table",
                        "referenced_column",
                        "foreign_column",
                        "parent_column"})
              .empty();
}

std::optional<CrudTableRecord> VisibleTableByUuid(const CrudState& state,
                                                  const std::string& table_uuid,
                                                  std::uint64_t observer_tx) {
  return FindVisibleCrudTable(state, table_uuid, observer_tx);
}

std::optional<EngineApiDiagnostic> ValidateForeignKeyReference(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudTableRecord& table,
    const std::map<std::string, std::string>& fields,
    const std::string& column_name,
    const std::string& value,
    ConstraintDmlValidationCache* cache,
    std::vector<EngineEvidenceReference>* evidence) {
  const auto reference = ParseForeignKeyReference(fields);
  if (!reference.has_value()) { return std::nullopt; }
  if (IsNullValue(value)) { return std::nullopt; }
  const std::string constraint_uuid = ConstraintUuid(fields, table, column_name, "foreign_key");
  const auto parent = VisibleTableByUuid(state,
                                         reference->parent_table_uuid,
                                         context.local_transaction_id);
  if (!parent) {
    return ConstraintDiagnostic("CLI.CONSTRAINT_DESCRIPTOR_INVALID",
                                "constraint.descriptor.invalid",
                                context,
                                table,
                                "foreign_key",
                                constraint_uuid,
                                "referenced_table_not_visible",
                                column_name);
  }
  const auto parent_index = FindVisibleUniqueIndexForColumn(state,
                                                            parent->table_uuid,
                                                            reference->parent_column,
                                                            context.local_transaction_id);
  if (!parent_index) {
    return ConstraintDiagnostic("CLI.SUPPORT_STRUCTURE_UNAVAILABLE",
                                "constraint.support_structure.unavailable",
                                context,
                                table,
                                "foreign_key",
                                constraint_uuid,
                                "referenced_candidate_key_backing_index_missing",
                                column_name,
                                {},
                                {},
                                "none");
  }
  const std::string proof_identity =
      constraint_uuid + "\n" + parent_index->index_uuid + "\n" + value;
  if (FindConstraintDmlProofPayload(cache,
                                    context,
                                    "foreign_key_parent_exists",
                                    proof_identity,
                                    evidence)
          .has_value()) {
    return std::nullopt;
  }
  const auto& parent_values =
      CachedColumnValues(cache, state, parent->table_uuid, reference->parent_column, context);
  if (parent_values.count(value) != 0) {
    StoreConstraintDmlProof(cache,
                            context,
                            "foreign_key_parent_exists",
                            proof_identity,
                            "visible_parent",
                            evidence);
    return std::nullopt;
  }
  return ConstraintDiagnostic("CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION",
                              "constraint.foreign_key.violation",
                              context,
                              table,
                              "foreign_key",
                              constraint_uuid,
                              "referenced_parent_key_missing",
                              column_name,
                              "key:" + parent_index->index_uuid,
                              parent_index->index_uuid,
                              "table_scan");
}

bool DescriptorHasExclusion(const std::map<std::string, std::string>& fields) {
  return BoolField(fields, {"exclusion", "exclusion_constraint"}) ||
         !FieldOrEmpty(fields, {"exclusion_operator", "exclusion_family"}).empty();
}

struct ExclusionInterval {
  bool valid = false;
  long double lower = 0;
  long double upper = 0;
};

ExclusionInterval ParseExclusionInterval(std::string value) {
  for (char& ch : value) {
    if (ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == ':') { ch = ','; }
  }
  const auto parts = Split(value, ',');
  if (parts.size() < 2) { return {}; }
  try {
    ExclusionInterval interval;
    interval.lower = std::stold(TrimAscii(parts[0]));
    interval.upper = std::stold(TrimAscii(parts[1]));
    if (interval.upper < interval.lower) { std::swap(interval.lower, interval.upper); }
    interval.valid = true;
    return interval;
  } catch (...) {
    return {};
  }
}

bool ExclusionValuesConflict(const std::string& left, const std::string& right) {
  if (IsNullValue(left) || IsNullValue(right)) { return false; }
  const auto left_interval = ParseExclusionInterval(left);
  const auto right_interval = ParseExclusionInterval(right);
  if (left_interval.valid && right_interval.valid) {
    return left_interval.lower < right_interval.upper &&
           right_interval.lower < left_interval.upper;
  }
  return left == right;
}

std::vector<std::pair<std::string, std::map<std::string, std::string>>> ConstraintColumns(
    const CrudTableRecord& table) {
  std::vector<std::pair<std::string, std::map<std::string, std::string>>> result;
  for (const auto& [column_name, descriptor] : table.columns) {
    result.push_back({column_name, DescriptorFields(descriptor)});
  }
  return result;
}

bool IsKeyColumnReferencedByChildren(const std::map<std::string, std::string>& child_fields,
                                     const CrudTableRecord& parent_table,
                                     const std::string& parent_column) {
  const auto reference = ParseForeignKeyReference(child_fields);
  return reference.has_value() &&
         reference->parent_table_uuid == parent_table.table_uuid &&
         reference->parent_column == parent_column;
}

std::optional<EngineApiDiagnostic> ValidateChildReferencesForParentValue(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudTableRecord& parent_table,
    const std::string& parent_column,
    const std::string& parent_value,
    const std::string& action_kind,
    ConstraintDmlValidationCache* cache = nullptr) {
  for (const auto& child_table : state.tables) {
    if (!CrudCreatorVisible(state, child_table.creator_tx, child_table.event_sequence, context.local_transaction_id)) {
      continue;
    }
    for (const auto& [child_column, child_fields] : ConstraintColumns(child_table)) {
      if (!IsKeyColumnReferencedByChildren(child_fields, parent_table, parent_column)) { continue; }
      const std::string action = LowerAscii(FieldOrEmpty(
          child_fields,
          {action_kind == "delete" ? "on_delete" : "on_update", "referential_action"}));
      if (!action.empty() && action != "restrict" && action != "no_action") {
        return ConstraintDiagnostic("CLI.NO_ENFORCEMENT_PATH",
                                    "constraint.no_enforcement_path",
                                    context,
                                    child_table,
                                    "foreign_key",
                                    ConstraintUuid(child_fields, child_table, child_column, "foreign_key"),
                                    "referential_action_requires_engine_action_executor:" + action,
                                    child_column,
                                    {},
                                    {},
                                    "none");
      }
      const auto& child_values =
          CachedColumnValues(cache, state, child_table.table_uuid, child_column, context);
      if (child_values.count(parent_value) != 0) {
        return ConstraintDiagnostic("CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION",
                                    "constraint.foreign_key.violation",
                                    context,
                                    child_table,
                                    "foreign_key",
                                    ConstraintUuid(child_fields, child_table, child_column, "foreign_key"),
                                    "referenced_parent_key_" + action_kind + "_restricted",
                                    child_column);
      }
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> FindConstraintDmlProofPayload(
    const ConstraintDmlValidationCache* cache,
    const EngineRequestContext& context,
    const std::string& proof_kind,
    const std::string& proof_identity,
    std::vector<EngineEvidenceReference>* evidence) {
  if (cache == nullptr) {
    if (evidence != nullptr) {
      evidence->push_back({"constraint_proof_refusal", proof_kind + ":cache_unavailable"});
    }
    return std::nullopt;
  }
  const std::string body = ProofBody(proof_kind, proof_identity);
  const auto current_context = MakeProofContext(context);
  bool saw_body = false;
  std::string mismatch = "context_mismatch";
  for (const auto& [stored_body, stored_context] : cache->validation_proofs) {
    if (stored_body != body) { continue; }
    saw_body = true;
    if (SameProofContext(stored_context, current_context)) {
      if (evidence != nullptr) {
        evidence->push_back({"constraint_proof_hit",
                             ProofEvidenceId(proof_kind, proof_identity)});
      }
      const auto payload = cache->validation_proof_payloads.find(
          ProofFullKey(body, stored_context));
      if (payload != cache->validation_proof_payloads.end()) {
        return payload->second;
      }
      return std::string{};
    }
    mismatch = FirstProofContextMismatch(stored_context, current_context);
  }
  if (saw_body && evidence != nullptr) {
    evidence->push_back({"constraint_proof_refusal", proof_kind + ":" + mismatch});
  }
  return std::nullopt;
}

void StoreConstraintDmlProof(
    ConstraintDmlValidationCache* cache,
    const EngineRequestContext& context,
    const std::string& proof_kind,
    const std::string& proof_identity,
    const std::string& payload,
    std::vector<EngineEvidenceReference>* evidence) {
  if (cache == nullptr) {
    return;
  }
  const std::string body = ProofBody(proof_kind, proof_identity);
  const auto proof_context = MakeProofContext(context);
  for (const auto& [stored_body, stored_context] : cache->validation_proofs) {
    if (stored_body == body && SameProofContext(stored_context, proof_context)) {
      return;
    }
  }
  cache->validation_proofs.push_back({body, proof_context});
  cache->validation_proof_payloads[ProofFullKey(body, proof_context)] = payload;
  if (evidence != nullptr) {
    evidence->push_back({"constraint_proof_store",
                         ProofEvidenceId(proof_kind, proof_identity)});
  }
}

void RecordIndexBackedUniquePreflightProof(
    ConstraintDmlValidationCache* cache,
    const EngineRequestContext& context,
    const CrudIndexRecord& index,
    const std::string& row_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    std::vector<EngineEvidenceReference>* evidence) {
  if (cache == nullptr) {
    return;
  }
  for (const auto& key : CrudIndexKeysForValues(index, values)) {
    const std::string proof_identity = UniquePreflightProofKey(index, row_uuid, key);
    cache->index_backed_unique_preflight_proofs.insert(proof_identity);
    StoreConstraintDmlProof(cache,
                            context,
                            "unique_preflight",
                            proof_identity,
                            "index_backed",
                            evidence);
  }
}

ConstraintDmlValidationResult ApplyConstraintDefaultsForInsert(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    const std::vector<std::pair<std::string, std::string>>& input_values) {
  ConstraintDmlValidationResult result;
  result.values = input_values;
  for (const auto& [column_name, fields] : ConstraintColumns(table)) {
    const bool present = HasField(result.values, column_name);
    const bool requested_default = present && FieldValue(result.values, column_name) == "<DEFAULT>";
    if (present && !requested_default) { continue; }
    const std::string default_envelope =
        FieldOrEmpty(fields, {"default_expression", "default_constraint", "default_value", "default"});
    if (default_envelope.empty()) {
      if (!requested_default) { continue; }
      result.diagnostic = ConstraintDiagnostic("CLI.NO_ENFORCEMENT_PATH",
                                               "constraint.no_enforcement_path",
                                               context,
                                               table,
                                               "default_constraint",
                                               ConstraintUuid(fields, table, column_name, "default_constraint"),
                                               "default_requested_without_descriptor",
                                               column_name,
                                               {},
                                               {},
                                               "none");
      return result;
    }
    if (const auto timing = ValidateImmediateTiming(context, table, fields, column_name, "default_constraint")) {
      result.diagnostic = *timing;
      return result;
    }
    const auto materialized = MaterializeDefault(default_envelope);
    if (!materialized.has_value()) {
      result.diagnostic = ConstraintDiagnostic("CLI.NO_ENFORCEMENT_PATH",
                                               "constraint.no_enforcement_path",
                                               context,
                                               table,
                                               "default_constraint",
                                               ConstraintUuid(fields, table, column_name, "default_constraint"),
                                               "default_expression_requires_sblr_executor",
                                               column_name,
                                               {},
                                               {},
                                               "none");
      return result;
    }
    UpsertField(&result.values, column_name, *materialized);
    result.evidence.push_back({"constraint_default", column_name});
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  return result;
}

ConstraintDmlValidationResult ValidateImmediateRowConstraints(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudTableRecord& table,
    const std::string& row_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& mutation_kind,
    ConstraintDmlValidationCache* cache) {
  return ValidateImmediateRowConstraintsWithOptions(
      context,
      state,
      table,
      row_uuid,
      values,
      mutation_kind,
      ConstraintDmlValidationOptions{},
      cache);
}

ConstraintDmlValidationResult ValidateImmediateRowConstraintsWithOptions(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudTableRecord& table,
    const std::string& row_uuid,
    const std::vector<std::pair<std::string, std::string>>& values,
    const std::string& mutation_kind,
    const ConstraintDmlValidationOptions& options,
    ConstraintDmlValidationCache* cache) {
  ConstraintDmlValidationResult result;
  result.values = values;
  for (const auto& [column_name, fields] : ConstraintColumns(table)) {
    (void)mutation_kind;
    const bool not_null = BoolField(fields, {"not_null", "required"}) ||
                          BoolField(fields, {"primary_key", "pk"}) ||
                          FalseField(fields, {"nullable"});
    const bool present = HasField(values, column_name);
    const std::string value = present ? FieldValue(values, column_name) : std::string("<NULL>");
    const bool deferred_timing = TimingRequiresDeferredStore(fields);
    auto record_deferred = [&result, &fields, &table, &column_name](const std::string& constraint_class) {
      result.evidence.push_back({"constraint_deferred_pending_check",
                                 ConstraintUuid(fields, table, column_name, constraint_class)});
    };
    if (not_null) {
      if (deferred_timing) {
        record_deferred("not_null_constraint");
      } else {
        if (!present || IsNullValue(value)) {
          result.diagnostic = ConstraintDiagnostic("CLI.CONSTRAINT_NOT_NULL_VIOLATION",
                                                   "constraint.not_null.violation",
                                                   context,
                                                   table,
                                                   "not_null_constraint",
                                                   ConstraintUuid(fields, table, column_name, "not_null_constraint"),
                                                   "null_value_forbidden",
                                                   column_name);
          return result;
        }
        const std::string proof_identity =
            ConstraintUuid(fields, table, column_name, "not_null_constraint") + "\n" +
            table.table_uuid + "\n" + column_name + "\nnon_null_value_present";
        if (!FindConstraintDmlProofPayload(cache,
                                           context,
                                           "not_null_descriptor",
                                           proof_identity,
                                           &result.evidence)
                 .has_value()) {
          StoreConstraintDmlProof(cache,
                                  context,
                                  "not_null_descriptor",
                                  proof_identity,
                                  "non_null",
                                  &result.evidence);
        }
        result.evidence.push_back({"constraint_not_null", column_name});
      }
    }

    const std::string check_envelope = FieldOrEmpty(fields, {"check_constraint", "check", "predicate_sblr_ref"});
    if (!check_envelope.empty()) {
      if (deferred_timing) {
        record_deferred("check_constraint");
      } else {
        const std::string unknown_policy = LowerAscii(FieldOrEmpty(fields, {"check_unknown_policy", "unknown_policy"}));
        if (IsNullValue(value) && unknown_policy != "fail") {
          const std::string proof_identity =
              ConstraintUuid(fields, table, column_name, "check_constraint") + "\n" +
              table.table_uuid + "\n" + column_name + "\n" + check_envelope + "\n" +
              unknown_policy + "\n<NULL>";
          if (!FindConstraintDmlProofPayload(cache,
                                             context,
                                             "check_predicate",
                                             proof_identity,
                                             &result.evidence)
                   .has_value()) {
            StoreConstraintDmlProof(cache,
                                    context,
                                    "check_predicate",
                                    proof_identity,
                                    "unknown_passed",
                                    &result.evidence);
          }
          result.evidence.push_back({"constraint_check_unknown_passed", column_name});
        } else {
          const std::string proof_identity =
              ConstraintUuid(fields, table, column_name, "check_constraint") + "\n" +
              table.table_uuid + "\n" + column_name + "\n" + check_envelope + "\n" +
              unknown_policy + "\n" + value;
          if (FindConstraintDmlProofPayload(cache,
                                            context,
                                            "check_predicate",
                                            proof_identity,
                                            &result.evidence)
                  .has_value()) {
            result.evidence.push_back({"constraint_check", column_name});
          } else {
            const auto check = EvaluateCheckEnvelope(check_envelope, value);
            if (check.unsupported) {
              result.diagnostic = ConstraintDiagnostic("CLI.NO_ENFORCEMENT_PATH",
                                                       "constraint.no_enforcement_path",
                                                       context,
                                                       table,
                                                       "check_constraint",
                                                       ConstraintUuid(fields, table, column_name, "check_constraint"),
                                                       check.detail,
                                                       column_name,
                                                       {},
                                                       {},
                                                       "none");
              return result;
            }
            if (!check.ok) {
              result.diagnostic = ConstraintDiagnostic("CLI.CONSTRAINT_CHECK_VIOLATION",
                                                       "constraint.check.violation",
                                                       context,
                                                       table,
                                                       "check_constraint",
                                                       ConstraintUuid(fields, table, column_name, "check_constraint"),
                                                       check.detail,
                                                       column_name);
              return result;
            }
            StoreConstraintDmlProof(cache,
                                    context,
                                    "check_predicate",
                                    proof_identity,
                                    "passed",
                                    &result.evidence);
            result.evidence.push_back({"constraint_check", column_name});
          }
        }
      }
    }

    const bool primary_key = BoolField(fields, {"primary_key", "pk"});
    const bool unique_key = primary_key || BoolField(fields, {"unique", "unique_key"});
    if (unique_key && options.validate_unique_constraints) {
      const std::string constraint_class = primary_key ? "primary_key" : "unique_key";
      if (deferred_timing) {
        record_deferred(constraint_class);
      } else {
        const auto support_index = FindVisibleUniqueIndexForColumn(state,
                                                                   table.table_uuid,
                                                                   column_name,
                                                                   context.local_transaction_id);
        if (!support_index) {
          result.diagnostic = ConstraintDiagnostic("CLI.SUPPORT_STRUCTURE_UNAVAILABLE",
                                                   "constraint.support_structure.unavailable",
                                                   context,
                                                   table,
                                                   constraint_class,
                                                   ConstraintUuid(fields, table, column_name, constraint_class),
                                                   "required_unique_backing_index_missing",
                                                   column_name,
                                                   {},
                                                   {},
                                                   "none");
          return result;
        }
        const std::string null_policy = LowerAscii(FieldOrEmpty(fields, {"null_policy", "unique_null_policy"}));
        const bool nulls_distinct = !primary_key && (null_policy.empty() || null_policy == "nulls_distinct");
        const auto descriptor_keys = CrudIndexKeysForValues(*support_index, values);
        const bool preflight_proven =
            !descriptor_keys.empty() &&
            !(nulls_distinct && AnyNullKey(descriptor_keys)) &&
            HasIndexBackedUniquePreflightProof(cache,
                                               context,
                                               *support_index,
                                               row_uuid,
                                               descriptor_keys,
                                               &result.evidence);
        if (const auto duplicate = ValidateUniqueIndexNoDuplicate(context,
                                                                  state,
                                                                  table,
                                                                  *support_index,
                                                                  row_uuid,
                                                                  values,
                                                                  fields,
                                                                  column_name,
                                                                  constraint_class,
                                                                  nulls_distinct,
                                                                  cache)) {
          result.diagnostic = *duplicate;
          return result;
        }
        if (preflight_proven) {
          result.evidence.push_back({"constraint_key_unique_preflight",
                                     support_index->index_uuid});
        }
        result.evidence.push_back({"constraint_key_support", support_index->index_uuid});
      }
    }

    if (DescriptorDeclaresForeignKey(fields) &&
        options.validate_foreign_key_constraints) {
      if (deferred_timing) {
        record_deferred("foreign_key");
      } else {
        if (!ParseForeignKeyReference(fields)) {
          result.diagnostic = ConstraintDiagnostic("CLI.CONSTRAINT_DESCRIPTOR_INVALID",
                                                   "constraint.descriptor.invalid",
                                                   context,
                                                   table,
                                                   "foreign_key",
                                                   ConstraintUuid(fields, table, column_name, "foreign_key"),
                                                   "referenced_key_descriptor_missing",
                                                   column_name,
                                                   {},
                                                   {},
                                                   "none");
          return result;
        }
        if (const auto diagnostic =
                ValidateForeignKeyReference(context,
                                            state,
                                            table,
                                            fields,
                                            column_name,
                                            value,
                                            cache,
                                            &result.evidence)) {
          result.diagnostic = *diagnostic;
          return result;
        }
        result.evidence.push_back({"constraint_foreign_key", column_name});
      }
    }

    if (DescriptorHasExclusion(fields)) {
      if (deferred_timing) {
        record_deferred("exclusion_constraint");
      } else {
        for (const auto& row : VisibleCrudRowsForContext(state, table.table_uuid, context)) {
          if (row.row_uuid == row_uuid) { continue; }
          if (ExclusionValuesConflict(value, FieldValue(row.values, column_name))) {
            result.diagnostic = ConstraintDiagnostic("CLI.CONSTRAINT_EXCLUSION_VIOLATION",
                                                     "constraint.exclusion.violation",
                                                     context,
                                                     table,
                                                     "exclusion_constraint",
                                                     ConstraintUuid(fields, table, column_name, "exclusion_constraint"),
                                                     "exclusion_value_conflict",
                                                     column_name,
                                                     {},
                                                     {},
                                                     "table_scan");
            return result;
          }
        }
        result.evidence.push_back({"constraint_exclusion", column_name});
      }
    }
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  if (!result.evidence.empty()) {
    result.evidence.push_back({"constraint_enforcement_timing", "immediate"});
    result.evidence.push_back({"constraint_visibility_authority", "mga_relation_store"});
  }
  return result;
}

EngineApiDiagnostic ValidateImmediateDeleteConstraints(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudTableRecord& table,
    const CrudRowVersionRecord& deleted_row) {
  for (const auto& [column_name, fields] : ConstraintColumns(table)) {
    const bool candidate_key = BoolField(fields, {"primary_key", "pk"}) ||
                               BoolField(fields, {"unique", "unique_key"});
    if (!candidate_key) { continue; }
    const std::string value = FieldValue(deleted_row.values, column_name);
    if (value.empty() || IsNullValue(value)) { continue; }
    if (const auto diagnostic = ValidateChildReferencesForParentValue(context,
                                                                      state,
                                                                      table,
                                                                      column_name,
                                                                      value,
                                                                      "delete")) {
      return *diagnostic;
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateImmediateParentKeyUpdateConstraints(
    const EngineRequestContext& context,
    const CrudState& state,
    const CrudTableRecord& table,
    const CrudRowVersionRecord& old_row,
    const std::vector<std::pair<std::string, std::string>>& new_values) {
  for (const auto& [column_name, fields] : ConstraintColumns(table)) {
    const bool candidate_key = BoolField(fields, {"primary_key", "pk"}) ||
                               BoolField(fields, {"unique", "unique_key"});
    if (!candidate_key) { continue; }
    const std::string old_value = FieldValue(old_row.values, column_name);
    const std::string new_value = FieldValue(new_values, column_name);
    if (old_value == new_value || old_value.empty() || IsNullValue(old_value)) { continue; }
    if (const auto diagnostic = ValidateChildReferencesForParentValue(context,
                                                                      state,
                                                                      table,
                                                                      column_name,
                                                                      old_value,
                                                                      "update")) {
      return *diagnostic;
    }
  }
  return OkDiagnostic();
}

EngineApiDiagnostic ValidateDeferredTransactionConstraints(const EngineRequestContext& context) {
  const auto loaded = LoadMgaRelationStoreState(context);
  if (!loaded.ok) { return loaded.diagnostic; }
  const CrudState state = BuildCrudCompatibilityStateFromMga(loaded.state);
  for (const auto& source_table : state.tables) {
    if (!CrudCreatorVisible(state,
                            source_table.creator_tx,
                            source_table.event_sequence,
                            context.local_transaction_id)) {
      continue;
    }
    CrudTableRecord commit_table = source_table;
    for (auto& [column_name, descriptor] : commit_table.columns) {
      (void)column_name;
      auto fields = DescriptorFields(descriptor);
      if (!TimingRequiresDeferredStore(fields)) { continue; }
      fields.erase("deferrable");
      fields.erase("initially_deferred");
      fields.erase("enforcement_timing");
      fields.erase("timing");
      descriptor = DescriptorText(fields);
    }
    for (const auto& row : VisibleCrudRowsForContext(state, commit_table.table_uuid, context)) {
      if (row.creator_tx != context.local_transaction_id) { continue; }
      const auto validation = ValidateImmediateRowConstraints(context,
                                                              state,
                                                              commit_table,
                                                              row.row_uuid,
                                                              row.values,
                                                              "commit");
      if (!validation.ok) { return validation.diagnostic; }
    }
  }
  return OkDiagnostic();
}

}  // namespace scratchbird::engine::internal_api
