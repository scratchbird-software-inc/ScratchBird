// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "behavior_support/api_behavior_store.hpp"

#include "uuid.hpp"
#include "api_behavior_record_codec.hpp"
#include <filesystem>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace scratchbird::engine::internal_api {
namespace {

bool StartsWith(const std::string& value, const std::string& prefix) { return value.rfind(prefix, 0) == 0; }

std::string ApiBehaviorEventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.api_events.v2";
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
  ApiBehaviorRecord checked;
  if(!DecodeApiBehaviorRecord({reinterpret_cast<const std::uint8_t*>(event.data()),event.size()},&checked))return MakeInvalidRequestDiagnostic("api_behavior.append_event","binary_record_invalid");
  std::error_code error;
  const bool native=std::filesystem::exists(ApiBehaviorEventPath(context),error);
  if(error||(!native&&std::filesystem::exists(context.database_path+".sb.api_events",error))||error)return MakeInvalidRequestDiagnostic("api_behavior.append_event","legacy_format_unsupported");
  std::ofstream out(ApiBehaviorEventPath(context), std::ios::binary | std::ios::app);
  if (!out) { return MakeInvalidRequestDiagnostic("api_behavior.append_event", "database_path_unwritable"); }
  out.write(event.data(),static_cast<std::streamsize>(event.size()));
  out.flush();
  if (!out) { return MakeInvalidRequestDiagnostic("api_behavior.append_event", "database_write_failed"); }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

std::string MakeApiBehaviorRecordEvent(const ApiBehaviorRecord& record) {
  std::string encoded;return EncodeApiBehaviorRecord(record,&encoded)?encoded:std::string{};
}

std::string ApiBehaviorPayloadFromRequest(const EngineApiRequest& request) {
  std::vector<std::string> payload;
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
  descriptor.encoded_descriptor = "object_kind=" + record.object_kind +
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
  record.target_database_uuid=request.target_database.uuid;
  record.target_schema_uuid=request.target_schema.uuid;
  record.target_object_uuid=request.target_object.uuid;
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
