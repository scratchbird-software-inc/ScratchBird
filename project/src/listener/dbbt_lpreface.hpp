// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <vector>

#include "listener_config.hpp"

namespace scratchbird::listener {

struct DbbtKeyMaterial {
  std::vector<std::uint8_t> bytes;
  std::string source_name;
};

struct DbbtGateResult {
  bool ok{false};
  proto::MessageVectorSet messages;
};

DbbtGateResult LoadDbbtKeyMaterial(const ListenerConfig& config, DbbtKeyMaterial* out);
DbbtGateResult ValidateDbbtHexToken(const ListenerConfig& config,
                                    const std::string& token_hex,
                                    proto::DbbtReplayCache* replay_cache);
std::string MakeListenerDebugTagForParser(const ListenerConfig& config, const std::string& parser_worker_id);

} // namespace scratchbird::listener
