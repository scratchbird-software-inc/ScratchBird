// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "event_notification_router.hpp"

#include "../core/uuid/uuid.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::server {
namespace {

ParserEventRouterResult Ok(std::uint64_t count = 0) {
  ParserEventRouterResult result;
  result.ok = true;
  result.affected_count = count;
  return result;
}

ParserEventRouterResult Error(std::string code, std::string detail) {
  ParserEventRouterResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  return result;
}

EventRouterIdentityKey DeliveryKey(const EventRouterUuid& parser_channel_uuid,
                        const EventRouterUuid& subscription_uuid,
                        const EventRouterUuid& event_uuid) {
  return {parser_channel_uuid, subscription_uuid, event_uuid};
}

}  // namespace

EventRouterIdentityKey ParserEventNotificationRouter::SubscriptionKey(const EventRouterUuid& parser_channel_uuid,
                                                           const EventRouterUuid& subscription_uuid,
                                                           const EventRouterUuid& event_channel_uuid) {
  return {parser_channel_uuid, subscription_uuid, event_channel_uuid};
}

std::uint64_t ParserEventNotificationRouter::PayloadBytes(const ParserEventNotification& notification) {
  return static_cast<std::uint64_t>(notification.payload.size() + notification.event_uuid.bytes.size() +
                                    notification.event_channel_uuid.bytes.size() + notification.payload_descriptor_uuid.bytes.size() +
                                    notification.subscription_uuid.bytes.size());
}

std::uint64_t ParserEventNotificationRouter::QueueBytes(const Queue& queue) const {
  std::uint64_t total = 0;
  for (const auto& item : queue) { total += PayloadBytes(item); }
  return total;
}

ParserEventRouterResult ParserEventNotificationRouter::RegisterSubscription(ParserEventSubscription subscription) {
  if (subscription.parser_channel_uuid.is_nil()) { return Error("EVENT.SUBSCRIBE_DENIED", "parser_channel_uuid_required"); }
  if (subscription.session_uuid.is_nil()) { return Error("EVENT.SUBSCRIBE_DENIED", "session_uuid_required"); }
  if (subscription.event_channel_uuid.is_nil()) { return Error("EVENT.CHANNEL_NOT_FOUND", "event_channel_uuid_required"); }
  if (subscription.subscription_uuid.is_nil()) { return Error("EVENT.SUBSCRIBE_DENIED", "subscription_uuid_required"); }
  subscription.active = true;
  subscriptions_[SubscriptionKey(subscription.parser_channel_uuid, subscription.subscription_uuid, subscription.event_channel_uuid)] = std::move(subscription);
  return Ok(1);
}

ParserEventRouterResult ParserEventNotificationRouter::UnregisterSubscription(const EventRouterUuid& parser_channel_uuid,
                                                                               const EventRouterUuid& subscription_uuid,
                                                                               const EventRouterUuid& event_channel_uuid) {
  std::uint64_t removed = 0;
  for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
    const bool parser_matches = parser_channel_uuid.is_nil() || it->second.parser_channel_uuid == parser_channel_uuid;
    const bool sub_matches = subscription_uuid.is_nil() || it->second.subscription_uuid == subscription_uuid;
    const bool channel_matches = event_channel_uuid.is_nil() || it->second.event_channel_uuid == event_channel_uuid;
    if (parser_matches && sub_matches && channel_matches) {
      it = subscriptions_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return Ok(removed);
}

ParserEventRouterResult ParserEventNotificationRouter::UnregisterSession(const EventRouterUuid& parser_channel_uuid,
                                                                          const EventRouterUuid& session_uuid) {
  std::uint64_t removed = 0;
  for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
    if ((parser_channel_uuid.is_nil() || it->second.parser_channel_uuid == parser_channel_uuid) &&
        (session_uuid.is_nil() || it->second.session_uuid == session_uuid)) {
      it = subscriptions_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  if (!parser_channel_uuid.is_nil()) { queues_by_parser_channel_.erase(parser_channel_uuid); }
  return Ok(removed);
}

ParserEventRouterResult ParserEventNotificationRouter::EnqueueCommittedEvent(const EventRouterUuid& event_channel_uuid,
                                                                              EventRouterUuid event_uuid,
                                                                              EventRouterUuid payload_descriptor_uuid,
                                                                              std::string payload,
                                                                              std::string redaction_state,
                                                                              const ParserEventQueuePolicy& policy) {
  if (event_channel_uuid.is_nil()) { return Error("EVENT.CHANNEL_NOT_FOUND", "event_channel_uuid_required"); }
  if (event_uuid.is_nil()) { return Error("EVENT.PAYLOAD_INVALID", "event_uuid_required"); }
  std::uint64_t enqueued = 0;
  for (auto& [key, subscription] : subscriptions_) {
    if (!subscription.active || subscription.event_channel_uuid != event_channel_uuid) { continue; }
    const auto delivery_key = DeliveryKey(subscription.parser_channel_uuid, subscription.subscription_uuid, event_uuid);
    if (delivered_event_keys_.count(delivery_key) != 0) { continue; }
    ParserEventNotification notification;
    notification.subscription_uuid = subscription.subscription_uuid;
    notification.session_uuid = subscription.session_uuid;
    notification.principal_uuid = subscription.principal_uuid;
    notification.event_uuid = event_uuid;
    notification.event_channel_uuid = event_channel_uuid;
    notification.payload_descriptor_uuid = payload_descriptor_uuid;
    notification.payload = payload;
    notification.redaction_state = redaction_state.empty() ? "clean" : redaction_state;
    notification.delivery_sequence = subscription.next_delivery_sequence++;
    notification.rendering_profile_uuid = subscription.rendering_profile_uuid;
    const auto message_identity = scratchbird::core::uuid::IssueRuntimeIdentityV7();
    if (!message_identity) return Error("EVENT.IDENTITY_UNAVAILABLE", "message_vector_identity_unavailable");
    notification.message_vector_uuid = *message_identity;
    auto& queue = queues_by_parser_channel_[subscription.parser_channel_uuid];
    while (!queue.empty() && (queue.size() >= policy.max_queued_events || QueueBytes(queue) + PayloadBytes(notification) > policy.max_queued_bytes)) {
      if (policy.overflow_behavior == "reject_publish" || policy.overflow_behavior == "backpressure") {
        return Error("EVENT.BACKPRESSURE", "queue_limit_reached");
      }
      queue.pop_front();
    }
    queue.push_back(std::move(notification));
    delivered_event_keys_.insert(delivery_key);
    ++enqueued;
  }
  return Ok(enqueued);
}

const ParserEventSubscription* ParserEventNotificationRouter::FindSubscription(const EventRouterUuid& parser_channel_uuid,
                                                                               const EventRouterUuid& subscription_uuid) const {
  if (subscription_uuid.is_nil()) { return nullptr; }
  for (const auto& [key, subscription] : subscriptions_) {
    if ((parser_channel_uuid.is_nil() || subscription.parser_channel_uuid == parser_channel_uuid) &&
        subscription.subscription_uuid == subscription_uuid) {
      return &subscription;
    }
  }
  return nullptr;
}

std::vector<ParserEventNotification> ParserEventNotificationRouter::DrainParserChannel(const EventRouterUuid& parser_channel_uuid,
                                                                                         std::uint64_t max_events) {
  std::vector<ParserEventNotification> out;
  auto it = queues_by_parser_channel_.find(parser_channel_uuid);
  if (it == queues_by_parser_channel_.end()) { return out; }
  auto& queue = it->second;
  const auto limit = max_events == 0 ? queue.size() : std::min<std::uint64_t>(max_events, queue.size());
  for (std::uint64_t i = 0; i < limit; ++i) {
    out.push_back(std::move(queue.front()));
    queue.pop_front();
  }
  return out;
}

std::uint64_t ParserEventNotificationRouter::ActiveSubscriptionCount() const {
  return static_cast<std::uint64_t>(subscriptions_.size());
}

std::uint64_t ParserEventNotificationRouter::QueuedEventCount(const EventRouterUuid& parser_channel_uuid) const {
  const auto it = queues_by_parser_channel_.find(parser_channel_uuid);
  return it == queues_by_parser_channel_.end() ? 0 : static_cast<std::uint64_t>(it->second.size());
}

}  // namespace scratchbird::server
