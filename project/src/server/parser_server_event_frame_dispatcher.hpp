// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "parser_server_event_ipc.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::server {

// SEARCH_KEY: EVN_IMPL_011_SBPS_EVENT_FRAME_DISPATCH
struct ParserServerEventFrame {
  ParserServerEventMessageType message_type = ParserServerEventMessageType::kEventSubscribeRequest;
  std::string request_uuid;
  ParserServerEventSession session;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct ParserServerEventOutboundFrame {
  ParserServerEventMessageType message_type = ParserServerEventMessageType::kEventSubscribeResult;
  std::string request_uuid;
  std::vector<std::pair<std::string, std::string>> fields;
  std::vector<ParserServerMessageVector> message_vector_set;
};

struct ParserServerEventDispatchResult {
  bool ok = false;
  std::string outcome;
  std::vector<ParserServerEventOutboundFrame> outbound_frames;
  std::vector<ParserServerMessageVector> message_vector_set;
};

class ParserServerEventFrameDispatcher {
 public:
  explicit ParserServerEventFrameDispatcher(ParserServerEventIpcRuntime* runtime);

  ParserServerEventDispatchResult DispatchParserFrame(const ParserServerEventFrame& frame);
  ParserServerEventDispatchResult PumpCommittedEvents(const PsEventDeliveryPumpRequest& request);

 private:
  ParserServerEventIpcRuntime* runtime_;

  ParserServerEventDispatchResult RuntimeUnavailable(const std::string& request_uuid) const;
};

}  // namespace scratchbird::server
