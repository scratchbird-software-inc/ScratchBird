// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "behavior_support/api_behavior_store.hpp"

#include "local_transaction_store.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace scratchbird::engine::internal_api {
namespace {

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string HexDecode(const std::string& value) {
  std::string out;
  if ((value.size() % 2) != 0) { return out; }
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) { return {}; }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

std::uint64_t ParseU64(const std::string& value) {
  try { return static_cast<std::uint64_t>(std::stoull(value)); } catch (...) { return 0; }
}

bool ParseBool(const std::string& value) { return value == "1" || value == "true" || value == "TRUE"; }

bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

std::string ApiBehaviorEventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.api_events";
}

std::string JoinOptions(const std::vector<std::string>& options) {
  std::string out;
  for (const auto& option : options) {
    if (!out.empty()) { out.push_back(';'); }
    out += option;
  }
  return out;
}

bool MgaCreatorVisible(const scratchbird::transaction::mga::LocalTransactionInventory& inventory,
                       std::uint64_t creator_tx,
                       std::uint64_t observer_tx) {
  if (creator_tx == 0) { return true; }
  for (const auto& entry : inventory.entries) {
    if (!entry.identity.local_id.valid() || entry.identity.local_id.value != creator_tx) { continue; }
    using scratchbird::transaction::mga::TransactionState;
    if (entry.state == TransactionState::committed || entry.state == TransactionState::archived) {
      return true;
    }
    return creator_tx == observer_tx &&
           (entry.state == TransactionState::active ||
            entry.state == TransactionState::read_only_active ||
            entry.state == TransactionState::preparing ||
            entry.state == TransactionState::prepared);
  }
  return false;
}

}  // namespace

EngineApiDiagnostic ValidateApiBehaviorContext(const EngineRequestContext& context,
                                               const std::string& operation_id,
                                               bool require_transaction,
                                               bool require_database_path) {
  if (require_database_path && context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic(operation_id, "database_path_required");
  }
  if (require_transaction && context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required");
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

ApiBehaviorStoreResult LoadApiBehaviorState(const EngineRequestContext& context) {
  ApiBehaviorStoreResult result;
  const auto path_status = ValidateApiBehaviorContext(context, "api_behavior.load_state", false, true);
  if (path_status.error) { result.diagnostic = path_status; return result; }
  std::ifstream in(ApiBehaviorEventPath(context), std::ios::binary);
  if (!in) { in.open(context.database_path, std::ios::binary); }
  if (!in) { result.ok = true; return result; }
  const auto crud = LoadCrudState(context);
  const auto transaction_inventory =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(context.database_path);
  std::map<std::string, ApiBehaviorRecord> latest;
  std::string line;
  std::uint64_t sequence = 0;
  while (std::getline(in, line)) {
    ++sequence;
    if (line.rfind("SBCRUD1", 0) == 0) { continue; }
    const auto magic_pos = line.find(kApiBehaviorEventMagic);
    if (magic_pos == std::string::npos) { continue; }
    if (magic_pos != 0) { line = line.substr(magic_pos); }
    const auto parts = Split(line, '\t');
    if (parts.size() < 10 || parts[1] != std::string("RECORD")) { continue; }
    ApiBehaviorRecord record;
    record.event_sequence = sequence;
    record.creator_tx = ParseU64(parts[2]);
    record.operation_id = parts[3];
    record.object_uuid = parts[4];
    record.object_kind = parts[5];
    record.default_name = HexDecode(parts[6]);
    record.payload = HexDecode(parts[7]);
    record.state = parts[8];
    record.deleted = ParseBool(parts[9]);
    const bool crud_visible =
        crud.ok && CrudCreatorVisible(crud.state,
                                      record.creator_tx,
                                      record.event_sequence,
                                      context.local_transaction_id);
    const bool mga_visible =
        transaction_inventory.ok() &&
        MgaCreatorVisible(transaction_inventory.inventory,
                          record.creator_tx,
                          context.local_transaction_id);
    if (!crud_visible && !mga_visible) { continue; }
    latest[record.object_uuid] = std::move(record);
  }
  for (const auto& [uuid, record] : latest) {
    if (!record.deleted) { result.state.records.push_back(record); }
  }
  result.ok = true;
  return result;
}

EngineApiDiagnostic AppendApiBehaviorEvent(const EngineRequestContext& context, const std::string& event) {
  const auto path_status = ValidateApiBehaviorContext(context, "api_behavior.append_event", false, true);
  if (path_status.error) { return path_status; }
  std::ofstream out(ApiBehaviorEventPath(context), std::ios::binary | std::ios::app);
  if (!out) { return MakeInvalidRequestDiagnostic("api_behavior.append_event", "database_path_unwritable"); }
  out << event << '\n';
  out.flush();
  if (!out) { return MakeInvalidRequestDiagnostic("api_behavior.append_event", "database_write_failed"); }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::string MakeApiBehaviorRecordEvent(const ApiBehaviorRecord& record) {
  return std::string(kApiBehaviorEventMagic) + "\tRECORD\t" + std::to_string(record.creator_tx) + "\t" +
         record.operation_id + "\t" + record.object_uuid + "\t" + record.object_kind + "\t" +
         EncodeCrudText(record.default_name) + "\t" + EncodeCrudText(record.payload) + "\t" + record.state + "\t" +
         (record.deleted ? "1" : "0");
}

std::string ApiBehaviorPrimaryName(const EngineApiRequest& request, const std::string& fallback) {
  if (!request.localized_names.empty() && !request.localized_names.front().name.empty()) { return request.localized_names.front().name; }
  if (!request.target_object.uuid.canonical.empty()) { return request.target_object.uuid.canonical; }
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, "name:")) { return option.substr(5); }
  }
  return fallback;
}

std::string ApiBehaviorPayloadFromRequest(const EngineApiRequest& request) {
  std::vector<std::string> payload;
  if (!request.target_database.uuid.canonical.empty()) { payload.push_back("database=" + request.target_database.uuid.canonical); }
  if (!request.target_schema.uuid.canonical.empty()) { payload.push_back("schema=" + request.target_schema.uuid.canonical); }
  if (!request.target_object.uuid.canonical.empty()) { payload.push_back("target=" + request.target_object.uuid.canonical); }
  if (!request.localized_names.empty()) {
    payload.push_back("localized_name_count=" + std::to_string(request.localized_names.size()));
    for (const auto& localized_name : request.localized_names) {
      payload.push_back("localized_name=" + localized_name.language_tag + "," + localized_name.name_class + "," +
                        localized_name.path + "," + localized_name.name + "," +
                        (localized_name.default_name ? "default" : "alias"));
    }
  }
  if (!request.predicate.predicate_kind.empty()) { payload.push_back("predicate=" + request.predicate.predicate_kind + ":" + request.predicate.canonical_predicate_envelope); }
  if (!request.projection.canonical_projection_envelopes.empty()) { payload.push_back("projection_count=" + std::to_string(request.projection.canonical_projection_envelopes.size())); }
  if (!request.descriptors.empty()) { payload.push_back("descriptor=" + request.descriptors.front().canonical_type_name); }
  if (!request.columns.empty()) { payload.push_back("column_count=" + std::to_string(request.columns.size())); }
  if (!request.indexes.empty()) { payload.push_back("index_count=" + std::to_string(request.indexes.size())); }
  if (!request.constraints.empty()) { payload.push_back("constraint_count=" + std::to_string(request.constraints.size())); }
  if (!request.rows.empty()) { payload.push_back("rows=" + std::to_string(request.rows.size())); }
  const auto options = JoinOptions(request.option_envelopes);
  if (!options.empty()) { payload.push_back("options=" + options); }
  return JoinOptions(payload);
}

std::string ApiBehaviorObjectUuid(const EngineApiRequest& request, const std::string& kind) {
  if (!request.target_object.uuid.canonical.empty()) { return request.target_object.uuid.canonical; }
  if (!request.related_objects.empty() && !request.related_objects.front().uuid.canonical.empty()) { return request.related_objects.front().uuid.canonical; }
  return GenerateCrudEngineUuid(kind == "database" ? "database" : (kind == "schema" ? "schema" : "object"));
}

EngineTypedValue ApiBehaviorValue(std::string value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  return typed;
}

EngineRowValue ApiBehaviorRow(std::vector<std::pair<std::string, std::string>> fields) {
  EngineRowValue row;
  row.requested_row_uuid.canonical = GenerateCrudEngineUuid("row");
  for (auto& field : fields) { row.fields.push_back({std::move(field.first), ApiBehaviorValue(std::move(field.second))}); }
  return row;
}

void AddApiBehaviorRow(EngineApiResult* result, std::vector<std::pair<std::string, std::string>> fields) {
  result->result_shape.result_kind = "api_behavior_rows";
  result->result_shape.rows.push_back(ApiBehaviorRow(std::move(fields)));
}

void AddApiBehaviorEvidence(EngineApiResult* result, std::string kind, std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

namespace {

EngineTypedValue DdlPublicationValue(const std::string& value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = value;
  typed.is_null = false;
  return typed;
}

std::string RowFieldValue(const EngineRowValue& row, const std::string& field_name) {
  for (const auto& field : row.fields) {
    if (field.first == field_name) { return field.second.encoded_value; }
  }
  return {};
}

void UpsertRowField(EngineRowValue* row, std::string field_name, const std::string& value) {
  if (row == nullptr) { return; }
  for (auto& field : row->fields) {
    if (field.first == field_name) {
      field.second = DdlPublicationValue(value);
      return;
    }
  }
  row->fields.push_back({std::move(field_name), DdlPublicationValue(value)});
}

EngineRowValue* FindPublicationRow(EngineApiResult* result,
                                   const std::string& object_kind,
                                   const std::string& object_uuid) {
  if (result == nullptr) { return nullptr; }
  for (auto& row : result->result_shape.rows) {
    if (RowFieldValue(row, "object_uuid") == object_uuid &&
        (object_kind.empty() || RowFieldValue(row, "object_kind") == object_kind)) {
      return &row;
    }
  }
  return nullptr;
}

}  // namespace

void AddDdlPublicationResult(EngineApiResult* result,
                             const std::string& operation_id,
                             const std::string& object_kind,
                             const std::string& object_uuid,
                             const std::string& catalog_row_uuid,
                             const std::string& invalidation_scope) {
  if (result == nullptr || !result->ok || object_uuid.empty()) { return; }
  const std::string effective_kind =
      object_kind.empty() ? result->primary_object.object_kind : object_kind;
  const std::string effective_catalog_row_uuid =
      !catalog_row_uuid.empty() ? catalog_row_uuid : result->catalog_row_uuid.canonical;
  const std::string effective_operation =
      !operation_id.empty() ? operation_id : result->operation_id;
  const std::string effective_invalidation_scope =
      !invalidation_scope.empty() ? invalidation_scope : effective_kind;
  const std::string publish_packet_id =
      effective_operation + ":" + effective_kind + ":" + object_uuid;
  constexpr const char* kPublishOrder =
      "validate.prebuild.persist.name_registry.result_shape.invalidate";

  result->result_shape.rows.reserve(result->result_shape.rows.size() + 1);
  EngineRowValue* row = FindPublicationRow(result, effective_kind, object_uuid);
  if (row == nullptr) {
    AddApiBehaviorRow(result, {{"object_uuid", object_uuid},
                               {"object_kind", effective_kind},
                               {"state", "published"}});
    row = &result->result_shape.rows.back();
  }

  UpsertRowField(row, "ddl_operation_id", effective_operation);
  UpsertRowField(row, "ddl_result_object_uuid", object_uuid);
  UpsertRowField(row, "ddl_result_object_kind", effective_kind);
  UpsertRowField(row, "ddl_result_catalog_row_uuid", effective_catalog_row_uuid);
  UpsertRowField(row, "ddl_publish_packet_id", publish_packet_id);
  UpsertRowField(row, "ddl_publish_order", kPublishOrder);
  UpsertRowField(row, "ddl_final_publish_short_section", "true");
  UpsertRowField(row, "ddl_partial_state_visible", "false");
  UpsertRowField(row, "ddl_result_buffer_owner", "engine");
  UpsertRowField(row, "ddl_parser_sql_authority", "false");
  UpsertRowField(row, "ddl_mga_finality_authority", "durable_transaction_inventory");

  AddApiBehaviorEvidence(result, "ddl_publish_packet", publish_packet_id);
  AddApiBehaviorEvidence(result, "ddl_publish_packet_order", kPublishOrder);
  AddApiBehaviorEvidence(result, "ddl_final_publish_short_section", "true");
  AddApiBehaviorEvidence(result, "ddl_partial_state_visible", "false");
  AddApiBehaviorEvidence(result, "ddl_uuid_returned", object_uuid);
  AddApiBehaviorEvidence(result, "ddl_catalog_row_uuid", effective_catalog_row_uuid);
  AddApiBehaviorEvidence(result, "ddl_result_buffer_owner", "engine");
  AddApiBehaviorEvidence(result, "ddl_result_buffer_reserved", "true");
  AddApiBehaviorEvidence(result, "ddl_dependency_invalidation_scope", effective_invalidation_scope);
  AddApiBehaviorEvidence(result, "ddl_parser_sql_authority", "false");
  AddApiBehaviorEvidence(result, "ddl_mga_finality_authority", "durable_transaction_inventory");
}

std::vector<ApiBehaviorRecord> VisibleApiBehaviorRecords(const EngineRequestContext& context,
                                                         const std::string& object_kind,
                                                         std::uint64_t observer_tx) {
  EngineRequestContext effective = context;
  if (effective.local_transaction_id == 0) { effective.local_transaction_id = observer_tx; }
  const auto loaded = LoadApiBehaviorState(effective);
  std::vector<ApiBehaviorRecord> records;
  if (!loaded.ok) { return records; }
  for (const auto& record : loaded.state.records) {
    if (object_kind.empty() || record.object_kind == object_kind) { records.push_back(record); }
  }
  return records;
}

std::optional<ApiBehaviorRecord> FindVisibleApiBehaviorRecord(const EngineRequestContext& context,
                                                              const std::string& object_uuid,
                                                              std::uint64_t observer_tx) {
  for (const auto& record : VisibleApiBehaviorRecords(context, {}, observer_tx)) {
    if (record.object_uuid == object_uuid) { return record; }
  }
  return std::nullopt;
}

EngineDescriptor ApiBehaviorDescriptor(const ApiBehaviorRecord& record) {
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = record.object_uuid;
  descriptor.descriptor_kind = record.object_kind;
  descriptor.canonical_type_name = record.default_name.empty() ? record.object_kind : record.default_name;
  descriptor.encoded_descriptor = "object_uuid=" + record.object_uuid + ";object_kind=" + record.object_kind +
                                  ";state=" + record.state + ";payload=" + record.payload;
  return descriptor;
}

ApiBehaviorPersistedRecord PersistApiBehaviorRecord(const EngineApiRequest& request,
                                                    const std::string& operation_id,
                                                    const std::string& object_kind,
                                                    bool require_transaction,
                                                    std::string explicit_state,
                                                    bool deleted) {
  return PersistApiBehaviorRecordWithPayload(request,
                                            operation_id,
                                            object_kind,
                                            require_transaction,
                                            std::move(explicit_state),
                                            deleted,
                                            ApiBehaviorPayloadFromRequest(request));
}

ApiBehaviorPersistedRecord PersistApiBehaviorRecordWithPayload(const EngineApiRequest& request,
                                                               const std::string& operation_id,
                                                               const std::string& object_kind,
                                                               bool require_transaction,
                                                               std::string explicit_state,
                                                               bool deleted,
                                                               std::string payload_override) {
  ApiBehaviorPersistedRecord result;
  const auto context_status = ValidateApiBehaviorContext(request.context, operation_id, require_transaction, true);
  if (context_status.error) { result.diagnostic = context_status; return result; }
  ApiBehaviorRecord record;
  record.creator_tx = require_transaction ? request.context.local_transaction_id : request.context.local_transaction_id;
  record.operation_id = operation_id;
  record.object_uuid = ApiBehaviorObjectUuid(request, object_kind);
  record.object_kind = object_kind;
  record.default_name = ApiBehaviorPrimaryName(request, "unnamed_" + object_kind);
  record.payload = std::move(payload_override);
  record.state = std::move(explicit_state);
  record.deleted = deleted;
  const auto appended = AppendApiBehaviorEvent(request.context, MakeApiBehaviorRecordEvent(record));
  if (appended.error) { result.diagnostic = appended; return result; }
  result.record = std::move(record);
  result.ok = true;
  return result;
}

}  // namespace scratchbird::engine::internal_api
