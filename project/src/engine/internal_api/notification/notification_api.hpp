// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: EVN_IMPL_001_CORE_EVENT_TYPES
inline constexpr const char* kEventNotificationRecordMagic = "SBEVN1";

struct EventQueuePolicyShape {
  std::string policy_uuid;
  std::string durability_profile = "ephemeral_session";
  std::uint64_t max_payload_bytes = 8192;
  std::uint64_t max_queued_events = 1024;
  std::uint64_t max_queued_bytes = 1048576;
  std::uint64_t retention_seconds = 0;
  std::string overflow_behavior = "backpressure_then_drop_oldest";
  std::string duplicate_policy = "none";
};

struct EventChannelShape {
  std::string channel_uuid;
  std::string channel_name;
  std::string payload_descriptor_uuid;
  std::string queue_policy_uuid;
  std::string state;
  std::string visibility = "normal";
  std::string redaction_policy = "none";
};

struct EventSubscriptionShape {
  std::string subscription_uuid;
  std::string session_uuid;
  std::string principal_uuid;
  std::string channel_uuid;
  std::string delivery_profile;
  std::string state;
};

struct EventPublicationShape {
  std::string event_uuid;
  std::string channel_uuid;
  std::string payload_descriptor_uuid;
  std::string payload;
  std::string redaction_state = "clean";
  std::string source_object_uuid;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t event_sequence = 0;
  std::string state;
};

struct EngineCreateEventChannelRequest : EngineApiRequest {};
struct EngineCreateEventChannelResult : EngineApiResult {
  EventChannelShape channel;
};
EngineCreateEventChannelResult EngineCreateEventChannel(const EngineCreateEventChannelRequest& request);

struct EngineAlterEventChannelRequest : EngineApiRequest {};
struct EngineAlterEventChannelResult : EngineApiResult {
  EventChannelShape channel;
};
EngineAlterEventChannelResult EngineAlterEventChannel(const EngineAlterEventChannelRequest& request);

struct EngineDropEventChannelRequest : EngineApiRequest {};
struct EngineDropEventChannelResult : EngineApiResult {
  EventChannelShape channel;
};
EngineDropEventChannelResult EngineDropEventChannel(const EngineDropEventChannelRequest& request);

struct EngineListenNotificationRequest : EngineApiRequest {};
struct EngineListenNotificationResult : EngineApiResult {
  EventSubscriptionShape subscription;
};
EngineListenNotificationResult EngineListenNotification(const EngineListenNotificationRequest& request);

struct EngineUnlistenNotificationRequest : EngineApiRequest {};
struct EngineUnlistenNotificationResult : EngineApiResult {
  std::uint64_t removed_count = 0;
};
EngineUnlistenNotificationResult EngineUnlistenNotification(const EngineUnlistenNotificationRequest& request);

struct EngineNotifyEventChannelRequest : EngineApiRequest {};
struct EngineNotifyEventChannelResult : EngineApiResult {
  EventPublicationShape publication;
};
EngineNotifyEventChannelResult EngineNotifyEventChannel(const EngineNotifyEventChannelRequest& request);

struct EngineListEventSubscriptionsRequest : EngineApiRequest {};
struct EngineListEventSubscriptionsResult : EngineApiResult {
  std::vector<EventSubscriptionShape> subscriptions;
};
EngineListEventSubscriptionsResult EngineListEventSubscriptions(const EngineListEventSubscriptionsRequest& request);

struct EnginePollEventDeliveryRequest : EngineApiRequest {};
struct EnginePollEventDeliveryResult : EngineApiResult {
  std::vector<EventPublicationShape> deliverable_events;
};
EnginePollEventDeliveryResult EnginePollEventDelivery(const EnginePollEventDeliveryRequest& request);

struct EngineAcknowledgeEventDeliveryRequest : EngineApiRequest {};
struct EngineAcknowledgeEventDeliveryResult : EngineApiResult {
  std::string acknowledgement_uuid;
};
EngineAcknowledgeEventDeliveryResult EngineAcknowledgeEventDelivery(const EngineAcknowledgeEventDeliveryRequest& request);

struct EngineUnlistenSessionNotificationsRequest : EngineApiRequest {};
struct EngineUnlistenSessionNotificationsResult : EngineApiResult {
  std::uint64_t removed_count = 0;
};
EngineUnlistenSessionNotificationsResult EngineUnlistenSessionNotifications(const EngineUnlistenSessionNotificationsRequest& request);

}  // namespace scratchbird::engine::internal_api
