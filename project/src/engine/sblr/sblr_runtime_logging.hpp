// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_runtime.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::sblr {

struct SblrRuntimeLogRecord {
  SblrUuid cluster_uuid;
  SblrUuid node_uuid;
  SblrUuid database_uuid;
  SblrUuid transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  SblrUuid statement_uuid;
  SblrUuid session_uuid;
  SblrUuid attachment_uuid;
  SblrUuid user_uuid;
  SblrUuid current_role_uuid;
  SblrUuid parser_profile_uuid;
  SblrUuid security_snapshot_uuid;
  SblrUuid package_object_uuid;
  SblrUuid routine_object_uuid;
  SblrUuid frame_uuid;
  std::string timestamp;
  std::string message;
};

SblrRuntimeLogRecord MakeSblrRuntimeLogRecord(const SblrExecutionContext& context,
                                              const SblrFrameStack& stack,
                                              std::string message);
// Opaque binary record; only client presentation may format UUID identities.
std::string SerializeSblrRuntimeLogRecord(const SblrRuntimeLogRecord& record);
bool DeserializeSblrRuntimeLogRecord(std::string_view bytes, SblrRuntimeLogRecord* record);
SblrResult EmitSblrRuntimeLog(const SblrExecutionContext& context,
                              const SblrFrameStack& stack,
                              std::string message);

}  // namespace scratchbird::engine::sblr
