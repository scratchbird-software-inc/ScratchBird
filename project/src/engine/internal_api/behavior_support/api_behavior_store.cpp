// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "behavior_support/api_behavior_store.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace scratchbird::engine::internal_api {
namespace {

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

}  // namespace

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

std::string ApiBehaviorPayloadFromRequest(const EngineApiRequest& request) {
  std::vector<std::string> payload;
  if (!request.target_database.uuid.is_nil()) { payload.push_back("database=" + request.target_database.uuid); }
  if (!request.target_schema.uuid.is_nil()) { payload.push_back("schema=" + request.target_schema.uuid); }
  if (!request.target_object.uuid.is_nil()) { payload.push_back("target=" + request.target_object.uuid); }
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

EngineDescriptor ApiBehaviorDescriptor(const ApiBehaviorRecord& record) {
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid = record.object_uuid;
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
