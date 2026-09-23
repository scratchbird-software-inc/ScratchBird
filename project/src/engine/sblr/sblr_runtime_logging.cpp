// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_runtime_logging.hpp"

#include "catalog/binary_catalog_metadata.hpp"
#include <utility>

namespace scratchbird::engine::sblr {

SblrRuntimeLogRecord MakeSblrRuntimeLogRecord(const SblrExecutionContext& context,
                                              const SblrFrameStack& stack,
                                              std::string message) {
  SblrRuntimeLogRecord record;
  record.cluster_uuid = context.cluster_uuid;
  record.node_uuid = context.node_uuid;
  record.database_uuid = context.database_uuid;
  record.transaction_uuid = context.transaction_uuid;
  record.local_transaction_id = context.local_transaction_id;
  record.statement_uuid = context.statement_uuid;
  record.session_uuid = context.session_uuid;
  record.attachment_uuid = context.attachment_uuid;
  record.user_uuid = context.user_uuid;
  record.current_role_uuid = context.current_role_uuid;
  record.parser_profile_uuid = context.parser_profile_uuid;
  record.security_snapshot_uuid = context.security_snapshot_uuid;
  record.timestamp = context.current_timestamp.empty() ? context.statement_timestamp : context.current_timestamp;
  if (!stack.frames.empty()) {
    record.package_object_uuid = stack.frames.back().package_object_uuid;
    record.routine_object_uuid = stack.frames.back().routine_object_uuid;
    record.frame_uuid = stack.frames.back().frame_uuid;
  }
  record.message = std::move(message);
  return record;
}

std::string SerializeSblrRuntimeLogRecord(const SblrRuntimeLogRecord& record) {
  internal_api::BinaryCatalogMetadata fields;
  fields.identities = {{"cluster_uuid", record.cluster_uuid},
      {"node_uuid", record.node_uuid},
      {"database_uuid", record.database_uuid},
      {"transaction_uuid", record.transaction_uuid},
      {"statement_uuid", record.statement_uuid},
      {"session_uuid", record.session_uuid},
      {"attachment_uuid", record.attachment_uuid},
      {"user_uuid", record.user_uuid},
      {"current_role_uuid", record.current_role_uuid},
      {"parser_profile_uuid", record.parser_profile_uuid},
      {"security_snapshot_uuid", record.security_snapshot_uuid},
      {"package_object_uuid", record.package_object_uuid},
      {"routine_object_uuid", record.routine_object_uuid},
      {"frame_uuid", record.frame_uuid}};
  fields.text = {{"local_transaction_id", std::to_string(record.local_transaction_id)},
                 {"timestamp", record.timestamp}, {"message", record.message}};
  std::string bytes;
  if (!internal_api::EncodeBinaryCatalogMetadata(fields, "sblr.runtime.log.v2", &bytes)) return {};
  return bytes;
}

bool DeserializeSblrRuntimeLogRecord(std::string_view bytes, SblrRuntimeLogRecord* record) {
  if (!record) return false;
  internal_api::BinaryCatalogMetadata fields;
  if (!internal_api::DecodeBinaryCatalogMetadata(bytes, "sblr.runtime.log.v2", &fields)) return false;
  SblrRuntimeLogRecord decoded;
  decoded.cluster_uuid = internal_api::BinaryCatalogUuid(fields, "cluster_uuid");
  decoded.node_uuid = internal_api::BinaryCatalogUuid(fields, "node_uuid");
  decoded.database_uuid = internal_api::BinaryCatalogUuid(fields, "database_uuid");
  decoded.transaction_uuid = internal_api::BinaryCatalogUuid(fields, "transaction_uuid");
  decoded.statement_uuid = internal_api::BinaryCatalogUuid(fields, "statement_uuid");
  decoded.session_uuid = internal_api::BinaryCatalogUuid(fields, "session_uuid");
  decoded.attachment_uuid = internal_api::BinaryCatalogUuid(fields, "attachment_uuid");
  decoded.user_uuid = internal_api::BinaryCatalogUuid(fields, "user_uuid");
  decoded.current_role_uuid = internal_api::BinaryCatalogUuid(fields, "current_role_uuid");
  decoded.parser_profile_uuid = internal_api::BinaryCatalogUuid(fields, "parser_profile_uuid");
  decoded.security_snapshot_uuid = internal_api::BinaryCatalogUuid(fields, "security_snapshot_uuid");
  decoded.package_object_uuid = internal_api::BinaryCatalogUuid(fields, "package_object_uuid");
  decoded.routine_object_uuid = internal_api::BinaryCatalogUuid(fields, "routine_object_uuid");
  decoded.frame_uuid = internal_api::BinaryCatalogUuid(fields, "frame_uuid");
  try { decoded.local_transaction_id = std::stoull(fields.text["local_transaction_id"]); }
  catch (...) { return false; }
  decoded.timestamp = fields.text["timestamp"];
  decoded.message = fields.text["message"];
  if (SerializeSblrRuntimeLogRecord(decoded) != bytes) return false;
  *record = std::move(decoded);
  return true;
}

SblrResult EmitSblrRuntimeLog(const SblrExecutionContext& context,
                              const SblrFrameStack& stack,
                              std::string message) {
  const auto record = MakeSblrRuntimeLogRecord(context, stack, std::move(message));
  SblrResult out = MakeSblrSuccess("sblr.runtime.log");
  SblrValue value;
  value.descriptor_id = "sblr_runtime_log_record";
  const auto bytes = SerializeSblrRuntimeLogRecord(record);
  if (bytes.empty()) {
    return MakeSblrFailure(SblrStatusCode::invalid_envelope, "sblr.runtime.log",
        MakeSblrDiagnostic("SB_SBLR_RUNTIME_LOG_INVALID_RECORD", "sblr.runtime.log.invalid_record",
                           "runtime log contains invalid native identity or oversized payload"));
  }
  value.binary_value.assign(bytes.begin(), bytes.end());
  value.payload_kind = SblrValuePayloadKind::binary;
  value.is_null = false;
  out.scalar_values.push_back(std::move(value));
  return out;
}

}  // namespace scratchbird::engine::sblr
