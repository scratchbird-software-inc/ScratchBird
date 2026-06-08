// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parser_server_event_ipc.hpp"

#include "notification/notification_api.hpp"
#include "sbps.hpp"
#include "session_registry.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::server {

namespace {

namespace api = scratchbird::engine::internal_api;

std::string ValueOr(const std::string& value, const std::string& fallback) {
  return value.empty() ? fallback : value;
}

api::EngineRequestContext EngineContextFrom(const ParserServerEventEngineContext& context) {
  api::EngineRequestContext out;
  out.trust_mode = context.trust_mode;
  out.request_id = context.request_id;
  out.database_path = context.database_path;
  out.database_uuid.canonical = context.database_uuid.canonical;
  out.principal_uuid.canonical = context.principal_uuid.canonical;
  out.session_uuid.canonical = context.session_uuid.canonical;
  out.transaction_uuid.canonical = context.transaction_uuid.canonical;
  out.statement_uuid.canonical = context.statement_uuid.canonical;
  out.local_transaction_id = context.local_transaction_id;
  out.snapshot_visible_through_local_transaction_id = context.snapshot_visible_through_local_transaction_id;
  out.statement_timestamp = context.statement_timestamp;
  out.transaction_timestamp = context.transaction_timestamp;
  out.current_timestamp = context.current_timestamp;
  out.current_monotonic_ns = context.current_monotonic_ns;
  out.application_name = context.application_name;
  out.security_context_present = context.security_context_present;
  out.cluster_authority_available = context.cluster_authority_available;
  out.catalog_generation_id = context.catalog_generation_id;
  out.security_epoch = context.security_epoch;
  out.resource_epoch = context.resource_epoch;
  out.name_resolution_epoch = context.name_resolution_epoch;
  out.trace_tags = context.trace_tags;
  return out;
}

ParserServerMessageVector ToMessageVector(const api::EngineApiDiagnostic& diagnostic) {
  ParserServerMessageVector vector;
  vector.message_class = diagnostic.error ? "error" : "status";
  vector.diagnostic_code = diagnostic.code;
  vector.safe_message_key = diagnostic.message_key;
  vector.detail = diagnostic.detail;
  vector.error = diagnostic.error;
  return vector;
}

void AppendEngineDiagnostics(const api::EngineApiResult& engine_result,
                             std::vector<ParserServerMessageVector>* vectors) {
  for (const auto& diagnostic : engine_result.diagnostics) {
    vectors->push_back(ToMessageVector(diagnostic));
  }
}

}  // namespace

ParserServerEventIpcRuntime::ParserServerEventIpcRuntime(ParserEventNotificationRouter* router) : router_(router) {}

ParserServerMessageVector ParserServerEventIpcRuntime::DiagnosticVector(std::string code,
                                                                         std::string safe_message_key,
                                                                         std::string detail,
                                                                         bool error) const {
  ParserServerMessageVector vector;
  vector.message_class = error ? "error" : "status";
  vector.diagnostic_code = std::move(code);
  vector.safe_message_key = std::move(safe_message_key);
  vector.detail = std::move(detail);
  vector.error = error;
  return vector;
}

bool ParserServerEventIpcRuntime::SessionReady(const ParserServerEventSession& session,
                                               std::vector<ParserServerMessageVector>* vectors,
                                               bool allow_while_draining) const {
  if (router_ == nullptr) {
    if (vectors != nullptr) {
      vectors->push_back(DiagnosticVector("PARSER_SERVER_IPC.ROUTER_UNAVAILABLE",
                                          "parser_server_ipc.router_unavailable",
                                          "event notification router is not attached",
                                          true));
    }
    return false;
  }
  if (!session.session_bound || session.parser_channel_uuid.empty() ||
      session.engine_context.session_uuid.canonical.empty() || session.engine_context.database_path.empty()) {
    if (vectors != nullptr) {
      vectors->push_back(DiagnosticVector("PARSER_SERVER_IPC.SESSION_REQUIRED",
                                          "parser_server_ipc.session_required",
                                          "event messages require a bound parser/server session",
                                          true));
    }
    return false;
  }
  if (session.draining && !allow_while_draining) {
    if (vectors != nullptr) {
      vectors->push_back(DiagnosticVector("PARSER_SERVER_IPC.DRAINING",
                                          "parser_server_ipc.draining",
                                          "server is draining and refuses new event work",
                                          true));
    }
    return false;
  }
  return true;
}

PsEventSubscribeResult ParserServerEventIpcRuntime::HandleSubscribe(const PsEventSubscribeRequest& request) {
  PsEventSubscribeResult result;
  result.request_uuid = request.request_uuid;
  if (!SessionReady(request.session, &result.message_vector_set)) {
    result.outcome = "rejected";
    return result;
  }

  api::EngineListenNotificationRequest engine_request;
  engine_request.context = EngineContextFrom(request.session.engine_context);
  engine_request.operation_id = "event.channel.listen";
  engine_request.target_object = {{request.channel_uuid}, "event_channel"};
  engine_request.option_envelopes.push_back("channel_uuid:" + request.channel_uuid);
  engine_request.option_envelopes.push_back("delivery_profile:" + ValueOr(request.delivery_profile, "ephemeral_session"));
  const auto engine_result = api::EngineListenNotification(engine_request);
  if (!engine_result.ok) {
    result.outcome = "rejected";
    AppendEngineDiagnostics(engine_result, &result.message_vector_set);
    return result;
  }

  ParserEventSubscription subscription;
  subscription.subscription_uuid = engine_result.subscription.subscription_uuid;
  subscription.parser_channel_uuid = request.session.parser_channel_uuid;
  subscription.session_uuid = engine_result.subscription.session_uuid;
  subscription.principal_uuid = engine_result.subscription.principal_uuid;
  subscription.event_channel_uuid = engine_result.subscription.channel_uuid;
  subscription.rendering_profile_uuid = request.rendering_profile_uuid;
  subscription.delivery_profile = engine_result.subscription.delivery_profile;

  const auto router_result = router_->RegisterSubscription(subscription);
  result.subscription_uuid = subscription.subscription_uuid;
  if (!router_result.ok) {
    result.outcome = "rejected";
    result.message_vector_set.push_back(DiagnosticVector(router_result.diagnostic_code,
                                                         "event.subscribe_denied",
                                                         router_result.detail,
                                                         true));
    return result;
  }
  result.outcome = "accepted";
  return result;
}

PsEventUnsubscribeResult ParserServerEventIpcRuntime::HandleUnsubscribe(const PsEventUnsubscribeRequest& request) {
  PsEventUnsubscribeResult result;
  result.request_uuid = request.request_uuid;
  if (!SessionReady(request.session, &result.message_vector_set, true)) {
    result.outcome = "rejected";
    return result;
  }

  if (request.all_channels) {
    api::EngineUnlistenSessionNotificationsRequest engine_request;
    engine_request.context = EngineContextFrom(request.session.engine_context);
    engine_request.operation_id = "session.notification.unlisten_all";
    engine_request.option_envelopes.push_back("session_uuid:" +
                                             request.session.engine_context.session_uuid.canonical);
    const auto engine_result = api::EngineUnlistenSessionNotifications(engine_request);
    if (!engine_result.ok) {
      result.outcome = "rejected";
      AppendEngineDiagnostics(engine_result, &result.message_vector_set);
      return result;
    }
    const auto router_result = router_->UnregisterSession(request.session.parser_channel_uuid,
                                                          request.session.engine_context.session_uuid.canonical);
    result.removed_count = engine_result.removed_count;
    result.outcome = router_result.ok ? "accepted" : "rejected";
    if (!router_result.ok) {
      result.message_vector_set.push_back(DiagnosticVector(router_result.diagnostic_code,
                                                           "event.unsubscribe_failed",
                                                           router_result.detail,
                                                           true));
    }
    return result;
  }

  std::string channel_uuid = request.channel_uuid;
  if (channel_uuid.empty() && !request.subscription_uuid.empty()) {
    const auto* subscription = router_->FindSubscription(request.session.parser_channel_uuid, request.subscription_uuid);
    if (subscription != nullptr) channel_uuid = subscription->event_channel_uuid;
  }
  if (channel_uuid.empty() && request.subscription_uuid.empty()) {
    result.outcome = "rejected";
    result.message_vector_set.push_back(DiagnosticVector("EVENT.SUBSCRIBE_DENIED",
                                                         "event.unsubscribe_target_required",
                                                         "unsubscribe requires subscription_uuid, channel_uuid, or all_channels",
                                                         true));
    return result;
  }
  if (channel_uuid.empty()) {
    result.outcome = "rejected";
    result.message_vector_set.push_back(DiagnosticVector("EVENT.SUBSCRIBE_DENIED",
                                                         "event.unsubscribe_target_required",
                                                         "unsubscribe by subscription_uuid requires an active router subscription",
                                                         true));
    return result;
  }

  api::EngineUnlistenNotificationRequest engine_request;
  engine_request.context = EngineContextFrom(request.session.engine_context);
  engine_request.operation_id = "event.channel.unlisten";
  engine_request.target_object = {{channel_uuid}, "event_channel"};
  engine_request.option_envelopes.push_back("channel_uuid:" + channel_uuid);
  if (!request.subscription_uuid.empty()) {
    engine_request.option_envelopes.push_back("subscription_uuid:" + request.subscription_uuid);
  }
  const auto engine_result = api::EngineUnlistenNotification(engine_request);
  if (!engine_result.ok) {
    result.outcome = "rejected";
    AppendEngineDiagnostics(engine_result, &result.message_vector_set);
    return result;
  }

  const auto router_result = router_->UnregisterSubscription(request.session.parser_channel_uuid,
                                                            request.subscription_uuid,
                                                            channel_uuid);
  result.removed_count = engine_result.removed_count;
  result.outcome = router_result.ok ? "accepted" : "rejected";
  if (!router_result.ok) {
    result.message_vector_set.push_back(DiagnosticVector(router_result.diagnostic_code,
                                                         "event.unsubscribe_failed",
                                                         router_result.detail,
                                                         true));
  }
  return result;
}

PsEventDeliveryPumpResult ParserServerEventIpcRuntime::PumpCommittedEvents(const PsEventDeliveryPumpRequest& request) {
  PsEventDeliveryPumpResult result;
  result.request_uuid = request.request_uuid;
  if (!SessionReady(request.session, &result.message_vector_set, true)) {
    result.outcome = "rejected";
    return result;
  }

  api::EnginePollEventDeliveryRequest engine_request;
  engine_request.context = EngineContextFrom(request.session.engine_context);
  engine_request.operation_id = "event.delivery.poll";
  const auto engine_result = api::EnginePollEventDelivery(engine_request);
  if (!engine_result.ok) {
    result.outcome = "rejected";
    AppendEngineDiagnostics(engine_result, &result.message_vector_set);
    return result;
  }
  for (const auto& publication : engine_result.deliverable_events) {
    const auto enqueued = router_->EnqueueCommittedEvent(publication.channel_uuid,
                                                         publication.event_uuid,
                                                         publication.payload_descriptor_uuid,
                                                         publication.payload,
                                                         publication.redaction_state,
                                                         request.queue_policy);
    if (!enqueued.ok) {
      PsEventBackpressureFrame frame;
      frame.parser_channel_uuid = request.session.parser_channel_uuid;
      frame.queued_events = router_->QueuedEventCount(request.session.parser_channel_uuid);
      frame.overflow_behavior = request.queue_policy.overflow_behavior;
      frame.message_vector = DiagnosticVector(enqueued.diagnostic_code,
                                              "event.backpressure",
                                              enqueued.detail,
                                              true);
      result.backpressure_frames.push_back(std::move(frame));
    }
  }

  auto notifications = router_->DrainParserChannel(request.session.parser_channel_uuid, request.max_events);
  for (const auto& notification : notifications) {
    PsEventNotificationFrame frame;
    frame.parser_channel_uuid = request.session.parser_channel_uuid;
    frame.subscription_uuid = notification.subscription_uuid;
    frame.event_uuid = notification.event_uuid;
    frame.delivery_sequence = notification.delivery_sequence;
    frame.notification_vector.message_class = "notification";
    frame.notification_vector.diagnostic_code = "EVENT.NOTIFICATION";
    frame.notification_vector.safe_message_key = "event.notification";
    frame.notification_vector.error = false;
    frame.notification_vector.fields = {
        {"event_uuid", notification.event_uuid},
        {"event", notification.event_uuid},
        {"channel_uuid", notification.event_channel_uuid},
        {"event_channel", notification.event_channel_uuid},
        {"subscription_uuid", notification.subscription_uuid},
        {"session_uuid", notification.session_uuid},
        {"principal_uuid", notification.principal_uuid},
        {"payload_descriptor_uuid", notification.payload_descriptor_uuid},
        {"payload", notification.payload},
        {"delivery_sequence", std::to_string(notification.delivery_sequence)},
        {"message_vector_uuid", notification.message_vector_uuid},
        {"rendering_profile_uuid", notification.rendering_profile_uuid},
        {"redaction_state", notification.redaction_state},
    };
    result.notifications.push_back(std::move(frame));
  }
  result.outcome = "accepted";
  return result;
}

PsEventAckResult ParserServerEventIpcRuntime::HandleAck(const PsEventAckRequest& request) {
  PsEventAckResult result;
  result.request_uuid = request.request_uuid;
  if (!SessionReady(request.session, &result.message_vector_set, true)) {
    result.outcome = "rejected";
    return result;
  }
  api::EngineAcknowledgeEventDeliveryRequest engine_request;
  engine_request.context = EngineContextFrom(request.session.engine_context);
  engine_request.operation_id = "event.delivery.ack";
  engine_request.option_envelopes.push_back("subscription_uuid:" + request.subscription_uuid);
  engine_request.option_envelopes.push_back("event_uuid:" + request.event_uuid);
  engine_request.option_envelopes.push_back("delivery_sequence:" + std::to_string(request.delivery_sequence));
  engine_request.option_envelopes.push_back("ack_state:" + ValueOr(request.ack_state, "acknowledged"));
  const auto engine_result = api::EngineAcknowledgeEventDelivery(engine_request);
  if (!engine_result.ok) {
    result.outcome = "rejected";
    AppendEngineDiagnostics(engine_result, &result.message_vector_set);
    return result;
  }
  result.outcome = "accepted";
  result.acknowledgement_uuid = engine_result.acknowledgement_uuid;
  return result;
}

PsEventDisconnectResult ParserServerEventIpcRuntime::HandleDisconnect(const PsEventDisconnectRequest& request) {
  PsEventDisconnectResult result;
  if (router_ == nullptr) {
    result.outcome = "rejected";
    result.message_vector_set.push_back(DiagnosticVector("PARSER_SERVER_IPC.ROUTER_UNAVAILABLE",
                                                         "parser_server_ipc.router_unavailable",
                                                         "event notification router is not attached",
                                                         true));
    return result;
  }
  if (request.session.session_bound && !request.session.engine_context.session_uuid.canonical.empty()) {
    api::EngineUnlistenSessionNotificationsRequest engine_request;
    engine_request.context = EngineContextFrom(request.session.engine_context);
    engine_request.operation_id = "session.notification.unlisten_all";
    engine_request.option_envelopes.push_back("session_uuid:" +
                                             request.session.engine_context.session_uuid.canonical);
    const auto engine_result = api::EngineUnlistenSessionNotifications(engine_request);
    if (!engine_result.ok) {
      AppendEngineDiagnostics(engine_result, &result.message_vector_set);
    }
    const auto router_result = router_->UnregisterSession(request.session.parser_channel_uuid,
                                                          request.session.engine_context.session_uuid.canonical);
    result.removed_count = router_result.affected_count;
    result.outcome = (router_result.ok && engine_result.ok) ? "accepted" : "rejected";
    if (!router_result.ok) {
      result.message_vector_set.push_back(DiagnosticVector(router_result.diagnostic_code,
                                                           "event.disconnect_cleanup_failed",
                                                           router_result.detail,
                                                           true));
    }
    return result;
  }
  result.outcome = "accepted";
  return result;
}

}  // namespace scratchbird::server
