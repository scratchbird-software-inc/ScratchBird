// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../core/platform/runtime_platform.hpp"

#include <cstdint>
#include <tuple>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace scratchbird::server {

using EventRouterUuid = scratchbird::core::platform::Uuid;
using EventRouterIdentityKey = std::tuple<EventRouterUuid, EventRouterUuid, EventRouterUuid>;

// SEARCH_KEY: EVN_IMPL_006_SERVER_SUBSCRIPTION_REGISTRY
struct ParserEventSubscription {
  EventRouterUuid subscription_uuid;
  EventRouterUuid parser_channel_uuid;
  EventRouterUuid session_uuid;
  EventRouterUuid principal_uuid;
  EventRouterUuid event_channel_uuid;
  EventRouterUuid rendering_profile_uuid;
  std::string delivery_profile = "ephemeral_session";
  std::uint64_t next_delivery_sequence = 1;
  bool active = true;
};

struct ParserEventNotification {
  EventRouterUuid subscription_uuid;
  EventRouterUuid session_uuid;
  EventRouterUuid principal_uuid;
  EventRouterUuid event_uuid;
  EventRouterUuid event_channel_uuid;
  EventRouterUuid payload_descriptor_uuid;
  std::string payload;
  EventRouterUuid message_vector_uuid;
  EventRouterUuid rendering_profile_uuid;
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
  ParserEventRouterResult UnregisterSubscription(const EventRouterUuid& parser_channel_uuid,
                                                 const EventRouterUuid& subscription_uuid,
                                                 const EventRouterUuid& event_channel_uuid);
  ParserEventRouterResult UnregisterSession(const EventRouterUuid& parser_channel_uuid,
                                            const EventRouterUuid& session_uuid);
  ParserEventRouterResult EnqueueCommittedEvent(const EventRouterUuid& event_channel_uuid,
                                                EventRouterUuid event_uuid,
                                                EventRouterUuid payload_descriptor_uuid,
                                                std::string payload,
                                                std::string redaction_state = "clean",
                                                const ParserEventQueuePolicy& policy = {});
  const ParserEventSubscription* FindSubscription(const EventRouterUuid& parser_channel_uuid,
                                                  const EventRouterUuid& subscription_uuid) const;
  std::vector<ParserEventNotification> DrainParserChannel(const EventRouterUuid& parser_channel_uuid,
                                                          std::uint64_t max_events);
  std::uint64_t ActiveSubscriptionCount() const;
  std::uint64_t QueuedEventCount(const EventRouterUuid& parser_channel_uuid) const;

 private:
  using SubscriptionMap = std::map<EventRouterIdentityKey, ParserEventSubscription>;
  using Queue = std::deque<ParserEventNotification>;

  static EventRouterIdentityKey SubscriptionKey(const EventRouterUuid& parser_channel_uuid,
                                     const EventRouterUuid& subscription_uuid,
                                     const EventRouterUuid& event_channel_uuid);
  static std::uint64_t PayloadBytes(const ParserEventNotification& notification);
  std::uint64_t QueueBytes(const Queue& queue) const;

  SubscriptionMap subscriptions_;
  std::map<EventRouterUuid, Queue> queues_by_parser_channel_;
  std::set<EventRouterIdentityKey> delivered_event_keys_;
};

}  // namespace scratchbird::server
