// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/common.hpp"

#include <string>

namespace scratchbird::parser::sbsql {

struct AuthRelayRequest {
  std::string provider_id;
  std::string payload;
  std::uint32_t retry_ordinal{0};
};

struct AuthRelayResult {
  bool accepted{false};
  bool continuation{false};
  SessionContext session;
  MessageVectorSet messages;
};

AuthRelayResult FailClosedAuthRelay(const AuthRelayRequest& request, const ParserConfig& config);
AuthRelayResult ProbeAuthRelay(const AuthRelayRequest& request, const ParserConfig& config);

} // namespace scratchbird::parser::sbsql
