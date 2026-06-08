// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace scratchbird::server {

// SEARCH_KEY: EVN_IMPL_006_SERVER_SUBSCRIPTION_REGISTRY
struct ParserEventSubscription {
  std::string subscription_uuid;
  std::string parser_channel_uuid;
  std::string session_uuid;
  std::string principal_uuid;
  std::string event_channel_uuid;
  std::string rendering_profile_uuid;
  std::string delivery_profile = "ephemeral_session";
  std::uint64_t next_delivery_sequence = 1;
  bool active = true;
};

struct ParserEventNotification {
  std::string subscription_uuid;
  std::string session_uuid;
  std::string principal_uuid;
  std::string event_uuid;
  std::string event_channel_uuid;
  std::string payload_descriptor_uuid;
  std::string payload;
  std::string message_vector_uuid;
  std::string rendering_profile_uuid;
  std::uint64_t delivery_sequence = 0;
  std::string redaction_state = "clean";
};

struct ParserEventQueuePolicy {
  std::uint64_t max_queued_events = 1024;
  std::uint64_t max_queued_bytes = 1048576;
  std::string overflow_behavior = "backpressure_then_drop_oldest";
};

struct ParserEventRouterResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::uint64_t affected_count = 0;
};

class ParserEventNotificationRouter {
 public:
  ParserEventNotificationRouter() = default;

  ParserEventRouterResult RegisterSubscription(ParserEventSubscription subscription);
  ParserEventRouterResult UnregisterSubscription(const std::string& parser_channel_uuid,
                                                 const std::string& subscription_uuid,
                                                 const std::string& event_channel_uuid);
  ParserEventRouterResult UnregisterSession(const std::string& parser_channel_uuid,
                                            const std::string& session_uuid);
  ParserEventRouterResult EnqueueCommittedEvent(const std::string& event_channel_uuid,
                                                std::string event_uuid,
                                                std::string payload_descriptor_uuid,
                                                std::string payload,
                                                std::string redaction_state = "clean",
                                                const ParserEventQueuePolicy& policy = {});
  const ParserEventSubscription* FindSubscription(const std::string& parser_channel_uuid,
                                                  const std::string& subscription_uuid) const;
  std::vector<ParserEventNotification> DrainParserChannel(const std::string& parser_channel_uuid,
                                                          std::uint64_t max_events);
  std::uint64_t ActiveSubscriptionCount() const;
  std::uint64_t QueuedEventCount(const std::string& parser_channel_uuid) const;

 private:
  using SubscriptionMap = std::map<std::string, ParserEventSubscription>;
  using Queue = std::deque<ParserEventNotification>;

  static std::string SubscriptionKey(const std::string& parser_channel_uuid,
                                     const std::string& subscription_uuid,
                                     const std::string& event_channel_uuid);
  static std::uint64_t PayloadBytes(const ParserEventNotification& notification);
  std::uint64_t QueueBytes(const Queue& queue) const;

  SubscriptionMap subscriptions_;
  std::map<std::string, Queue> queues_by_parser_channel_;
  std::set<std::string> delivered_event_keys_;
};

}  // namespace scratchbird::server
