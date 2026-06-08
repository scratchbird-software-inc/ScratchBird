// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_runtime_logging.hpp"

#include <sstream>
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
  std::ostringstream out;
  out << "SBLRLOG1"
      << "\tcluster_uuid=" << record.cluster_uuid
      << "\tnode_uuid=" << record.node_uuid
      << "\tdatabase_uuid=" << record.database_uuid
      << "\ttransaction_uuid=" << record.transaction_uuid
      << "\tlocal_transaction_id=" << record.local_transaction_id
      << "\tstatement_uuid=" << record.statement_uuid
      << "\tsession_uuid=" << record.session_uuid
      << "\tattachment_uuid=" << record.attachment_uuid
      << "\tuser_uuid=" << record.user_uuid
      << "\tcurrent_role_uuid=" << record.current_role_uuid
      << "\tparser_profile_uuid=" << record.parser_profile_uuid
      << "\tsecurity_snapshot_uuid=" << record.security_snapshot_uuid
      << "\tpackage_object_uuid=" << record.package_object_uuid
      << "\troutine_object_uuid=" << record.routine_object_uuid
      << "\tframe_uuid=" << record.frame_uuid
      << "\ttimestamp=" << record.timestamp
      << "\tmessage=" << record.message;
  return out.str();
}

SblrResult EmitSblrRuntimeLog(const SblrExecutionContext& context,
                              const SblrFrameStack& stack,
                              std::string message) {
  const auto record = MakeSblrRuntimeLogRecord(context, stack, std::move(message));
  SblrResult out = MakeSblrSuccess("sblr.runtime.log");
  SblrValue value;
  value.descriptor_id = "sblr_runtime_log_record";
  value.text_value = SerializeSblrRuntimeLogRecord(record);
  value.encoded_value = value.text_value;
  value.payload_kind = SblrValuePayloadKind::descriptor_payload;
  value.is_null = false;
  out.scalar_values.push_back(std::move(value));
  return out;
}

}  // namespace scratchbird::engine::sblr
