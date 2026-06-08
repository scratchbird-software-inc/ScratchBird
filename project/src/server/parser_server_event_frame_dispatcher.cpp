// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parser_server_event_frame_dispatcher.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace scratchbird::server {
namespace {

std::string Field(const ParserServerEventFrame& frame, const std::string& key) {
  for (const auto& field : frame.fields) {
    if (field.first == key) { return field.second; }
  }
  return {};
}

bool FieldBool(const ParserServerEventFrame& frame, const std::string& key) {
  const auto value = Field(frame, key);
  return value == "1" || value == "true" || value == "yes" || value == "all";
}

std::uint64_t FieldU64(const ParserServerEventFrame& frame, const std::string& key, std::uint64_t fallback = 0) {
  try {
    const auto value = Field(frame, key);
    return value.empty() ? fallback : static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

void AddField(std::vector<std::pair<std::string, std::string>>* fields,
              const std::string& key,
              const std::string& value) {
  if (!value.empty()) { fields->push_back({key, value}); }
}

ParserServerMessageVector ErrorVector(std::string code, std::string key, std::string detail) {
  ParserServerMessageVector vector;
  vector.message_class = "error";
  vector.diagnostic_code = std::move(code);
  vector.safe_message_key = std::move(key);
  vector.detail = std::move(detail);
  vector.error = true;
  return vector;
}

ParserServerEventOutboundFrame MakeOutcomeFrame(ParserServerEventMessageType type,
                                                const std::string& request_uuid,
                                                const std::string& outcome,
                                                std::vector<ParserServerMessageVector> vectors) {
  ParserServerEventOutboundFrame frame;
  frame.message_type = type;
  frame.request_uuid = request_uuid;
  frame.message_vector_set = std::move(vectors);
  frame.fields.push_back({"request_uuid", request_uuid});
  frame.fields.push_back({"outcome", outcome});
  return frame;
}

ParserServerEventOutboundFrame MakeNotificationFrame(const PsEventNotificationFrame& notification) {
  ParserServerEventOutboundFrame frame;
  frame.message_type = ParserServerEventMessageType::kEventNotification;
  frame.fields.push_back({"parser_channel_uuid", notification.parser_channel_uuid});
  frame.fields.push_back({"subscription_uuid", notification.subscription_uuid});
  frame.fields.push_back({"event_uuid", notification.event_uuid});
  frame.fields.push_back({"delivery_sequence", std::to_string(notification.delivery_sequence)});
  frame.fields.insert(frame.fields.end(),
                      notification.notification_vector.fields.begin(),
                      notification.notification_vector.fields.end());
  frame.message_vector_set.push_back(notification.notification_vector);
  return frame;
}

ParserServerEventOutboundFrame MakeBackpressureFrame(const PsEventBackpressureFrame& backpressure) {
  ParserServerEventOutboundFrame frame;
  frame.message_type = ParserServerEventMessageType::kEventBackpressure;
  frame.fields.push_back({"parser_channel_uuid", backpressure.parser_channel_uuid});
  frame.fields.push_back({"subscription_uuid", backpressure.subscription_uuid});
  frame.fields.push_back({"queued_events", std::to_string(backpressure.queued_events)});
  frame.fields.push_back({"queued_bytes", std::to_string(backpressure.queued_bytes)});
  frame.fields.push_back({"overflow_behavior", backpressure.overflow_behavior});
  frame.message_vector_set.push_back(backpressure.message_vector);
  return frame;
}

}  // namespace

ParserServerEventFrameDispatcher::ParserServerEventFrameDispatcher(ParserServerEventIpcRuntime* runtime)
    : runtime_(runtime) {}

ParserServerEventDispatchResult ParserServerEventFrameDispatcher::RuntimeUnavailable(const std::string& request_uuid) const {
  ParserServerEventDispatchResult result;
  result.ok = false;
  result.outcome = "rejected";
  result.message_vector_set.push_back(ErrorVector("PARSER_SERVER_IPC.EVENT_RUNTIME_UNAVAILABLE",
                                                 "parser_server_ipc.event_runtime_unavailable",
                                                 "parser-server event runtime is not attached"));
  result.outbound_frames.push_back(MakeOutcomeFrame(ParserServerEventMessageType::kEventSubscribeResult,
                                                    request_uuid,
                                                    result.outcome,
                                                    result.message_vector_set));
  return result;
}

ParserServerEventDispatchResult ParserServerEventFrameDispatcher::DispatchParserFrame(const ParserServerEventFrame& frame) {
  if (runtime_ == nullptr) { return RuntimeUnavailable(frame.request_uuid); }

  ParserServerEventDispatchResult result;
  result.ok = false;
  result.outcome = "rejected";

  switch (frame.message_type) {
    case ParserServerEventMessageType::kEventSubscribeRequest: {
      PsEventSubscribeRequest request;
      request.request_uuid = frame.request_uuid;
      request.session = frame.session;
      request.channel_uuid = Field(frame, "channel_uuid");
      request.rendering_profile_uuid = Field(frame, "rendering_profile_uuid");
      request.delivery_profile = Field(frame, "delivery_profile").empty() ? "ephemeral_session" : Field(frame, "delivery_profile");
      request.policy_generation = FieldU64(frame, "policy_generation");
      const auto handled = runtime_->HandleSubscribe(request);
      auto response = MakeOutcomeFrame(ParserServerEventMessageType::kEventSubscribeResult,
                                       handled.request_uuid,
                                       handled.outcome,
                                       handled.message_vector_set);
      AddField(&response.fields, "subscription_uuid", handled.subscription_uuid);
      result.outcome = handled.outcome;
      result.ok = handled.outcome == "accepted";
      result.message_vector_set = handled.message_vector_set;
      result.outbound_frames.push_back(std::move(response));
      return result;
    }
    case ParserServerEventMessageType::kEventUnsubscribeRequest: {
      PsEventUnsubscribeRequest request;
      request.request_uuid = frame.request_uuid;
      request.session = frame.session;
      request.subscription_uuid = Field(frame, "subscription_uuid");
      request.channel_uuid = Field(frame, "channel_uuid");
      request.all_channels = FieldBool(frame, "all_channels");
      const auto handled = runtime_->HandleUnsubscribe(request);
      auto response = MakeOutcomeFrame(ParserServerEventMessageType::kEventUnsubscribeResult,
                                       handled.request_uuid,
                                       handled.outcome,
                                       handled.message_vector_set);
      response.fields.push_back({"removed_count", std::to_string(handled.removed_count)});
      result.outcome = handled.outcome;
      result.ok = handled.outcome == "accepted";
      result.message_vector_set = handled.message_vector_set;
      result.outbound_frames.push_back(std::move(response));
      return result;
    }
    case ParserServerEventMessageType::kEventAck: {
      PsEventAckRequest request;
      request.request_uuid = frame.request_uuid;
      request.session = frame.session;
      request.subscription_uuid = Field(frame, "subscription_uuid");
      request.event_uuid = Field(frame, "event_uuid");
      request.delivery_sequence = FieldU64(frame, "delivery_sequence");
      request.ack_state = Field(frame, "ack_state").empty() ? "acknowledged" : Field(frame, "ack_state");
      const auto handled = runtime_->HandleAck(request);
      auto response = MakeOutcomeFrame(ParserServerEventMessageType::kEventAck,
                                       handled.request_uuid,
                                       handled.outcome,
                                       handled.message_vector_set);
      AddField(&response.fields, "acknowledgement_uuid", handled.acknowledgement_uuid);
      result.outcome = handled.outcome;
      result.ok = handled.outcome == "accepted";
      result.message_vector_set = handled.message_vector_set;
      result.outbound_frames.push_back(std::move(response));
      return result;
    }
    case ParserServerEventMessageType::kEventSubscribeResult:
    case ParserServerEventMessageType::kEventUnsubscribeResult:
    case ParserServerEventMessageType::kEventNotification:
    case ParserServerEventMessageType::kEventBackpressure:
    case ParserServerEventMessageType::kEventSubscriptionInvalidate:
    case ParserServerEventMessageType::kEventChannelClosed:
      result.message_vector_set.push_back(ErrorVector("PARSER_SERVER_IPC.MESSAGE_DIRECTION_INVALID",
                                                     "parser_server_ipc.message_direction_invalid",
                                                     "parser attempted to send a server-owned event message"));
      result.outbound_frames.push_back(MakeOutcomeFrame(ParserServerEventMessageType::kEventSubscribeResult,
                                                        frame.request_uuid,
                                                        result.outcome,
                                                        result.message_vector_set));
      return result;
  }

  result.message_vector_set.push_back(ErrorVector("PARSER_SERVER_IPC.MESSAGE_TYPE_UNKNOWN",
                                                 "parser_server_ipc.message_type_unknown",
                                                 "unknown parser-server event message type"));
  result.outbound_frames.push_back(MakeOutcomeFrame(ParserServerEventMessageType::kEventSubscribeResult,
                                                    frame.request_uuid,
                                                    result.outcome,
                                                    result.message_vector_set));
  return result;
}

ParserServerEventDispatchResult ParserServerEventFrameDispatcher::PumpCommittedEvents(const PsEventDeliveryPumpRequest& request) {
  if (runtime_ == nullptr) { return RuntimeUnavailable(request.request_uuid); }
  const auto handled = runtime_->PumpCommittedEvents(request);
  ParserServerEventDispatchResult result;
  result.ok = handled.outcome == "accepted";
  result.outcome = handled.outcome;
  result.message_vector_set = handled.message_vector_set;
  for (const auto& notification : handled.notifications) {
    result.outbound_frames.push_back(MakeNotificationFrame(notification));
  }
  for (const auto& backpressure : handled.backpressure_frames) {
    result.outbound_frames.push_back(MakeBackpressureFrame(backpressure));
  }
  return result;
}

}  // namespace scratchbird::server
