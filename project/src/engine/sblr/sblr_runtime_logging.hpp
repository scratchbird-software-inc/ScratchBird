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
  std::string cluster_uuid;
  std::string node_uuid;
  std::string database_uuid;
  std::string transaction_uuid;
  std::uint64_t local_transaction_id = 0;
  std::string statement_uuid;
  std::string session_uuid;
  std::string attachment_uuid;
  std::string user_uuid;
  std::string current_role_uuid;
  std::string parser_profile_uuid;
  std::string security_snapshot_uuid;
  std::string package_object_uuid;
  std::string routine_object_uuid;
  std::string frame_uuid;
  std::string timestamp;
  std::string message;
};

SblrRuntimeLogRecord MakeSblrRuntimeLogRecord(const SblrExecutionContext& context,
                                              const SblrFrameStack& stack,
                                              std::string message);
std::string SerializeSblrRuntimeLogRecord(const SblrRuntimeLogRecord& record);
SblrResult EmitSblrRuntimeLog(const SblrExecutionContext& context,
                              const SblrFrameStack& stack,
                              std::string message);

}  // namespace scratchbird::engine::sblr
