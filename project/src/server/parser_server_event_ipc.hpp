// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "event_notification_router.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::server {

// SEARCH_KEY: EVN_IMPL_010_SB_SERVER_EVENT_IPC_RUNTIME
enum class ParserServerEventMessageType : std::uint16_t {
  kEventSubscribeRequest = 80,
  kEventSubscribeResult = 81,
  kEventUnsubscribeRequest = 82,
  kEventUnsubscribeResult = 83,
  kEventNotification = 84,
  kEventAck = 85,
  kEventBackpressure = 86,
  kEventSubscriptionInvalidate = 87,
  kEventChannelClosed = 88,
};

struct ParserServerMessageVector {
  std::string message_class;
  std::string diagnostic_code;
  std::string safe_message_key;
  std::string detail;
  bool error = false;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct ParserServerEventUuidRef {
  std::string canonical;
};

enum class ParserServerEventTrustMode {
  server_isolated,
  embedded_in_process
};

struct ParserServerEventLanguageContext {
  std::string language_profile_id = "sbsql.builtin.recovery.en";
  std::string language_tag = "en";
  std::string default_language_tag = "en";
  std::string input_syntax_profile = "sbsql.syntax.standard";
  std::string input_language_fallback_tag;
  std::string common_resource_hash = "builtin.common.sbsql.v1";
  std::uint64_t language_resource_epoch = 1;
  std::uint64_t localized_name_epoch = 1;
  std::uint64_t message_resource_epoch = 1;
  std::string resource_compatibility_identity = "sbsql.resource.compat.v1";
  std::string resource_version_identity = "sbsql.resource-pack.v1";
};

struct ParserServerEventEngineContext {
  ParserServerEventTrustMode trust_mode = ParserServerEventTrustMode::server_isolated;
  std::string request_id;
  std::string database_path;
  ParserServerEventUuidRef database_uuid;
  ParserServerEventUuidRef principal_uuid;
  ParserServerEventUuidRef session_uuid;
  ParserServerEventUuidRef transaction_uuid;
  ParserServerEventUuidRef statement_uuid;
  std::uint64_t local_transaction_id = 0;
  std::uint64_t snapshot_visible_through_local_transaction_id = 0;
  std::string statement_timestamp;
  std::string transaction_timestamp;
  std::string current_timestamp;
  std::string current_monotonic_ns;
  std::string application_name;
  bool security_context_present = false;
  bool cluster_authority_available = false;
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  ParserServerEventLanguageContext language_context;
  std::vector<std::string> trace_tags;
};

struct ParserServerEventSession {
  std::string parser_channel_uuid;
  std::string parser_family_uuid;
  std::string parser_package_uuid;
  ParserServerEventEngineContext engine_context;
  bool session_bound = false;
  bool draining = false;
};

struct PsEventSubscribeRequest {
  std::string request_uuid;
  ParserServerEventSession session;
  std::string channel_uuid;
  std::string rendering_profile_uuid;
  std::string delivery_profile = "ephemeral_session";
  std::uint64_t policy_generation = 0;
};

struct PsEventSubscribeResult {
  std::string request_uuid;
  std::string subscription_uuid;
  std::string outcome;
  std::vector<ParserServerMessageVector> message_vector_set;
};

struct PsEventUnsubscribeRequest {
  std::string request_uuid;
  ParserServerEventSession session;
  std::string subscription_uuid;
  std::string channel_uuid;
  bool all_channels = false;
};

struct PsEventUnsubscribeResult {
  std::string request_uuid;
  std::string outcome;
  std::uint64_t removed_count = 0;
  std::vector<ParserServerMessageVector> message_vector_set;
};

struct PsEventNotificationFrame {
  ParserServerEventMessageType message_type = ParserServerEventMessageType::kEventNotification;
  std::string parser_channel_uuid;
  std::string subscription_uuid;
  std::string event_uuid;
  std::uint64_t delivery_sequence = 0;
  ParserServerMessageVector notification_vector;
};

struct PsEventBackpressureFrame {
  ParserServerEventMessageType message_type = ParserServerEventMessageType::kEventBackpressure;
  std::string parser_channel_uuid;
  std::string subscription_uuid;
  std::uint64_t queued_events = 0;
  std::uint64_t queued_bytes = 0;
  std::string overflow_behavior;
  ParserServerMessageVector message_vector;
};

struct PsEventDeliveryPumpRequest {
  std::string request_uuid;
  ParserServerEventSession session;
  std::uint64_t max_events = 64;
  ParserEventQueuePolicy queue_policy;
};

struct PsEventDeliveryPumpResult {
  std::string request_uuid;
  std::string outcome;
  std::vector<PsEventNotificationFrame> notifications;
  std::vector<PsEventBackpressureFrame> backpressure_frames;
  std::vector<ParserServerMessageVector> message_vector_set;
};

struct PsEventAckRequest {
  std::string request_uuid;
  ParserServerEventSession session;
  std::string subscription_uuid;
  std::string event_uuid;
  std::uint64_t delivery_sequence = 0;
  std::string ack_state = "acknowledged";
};

struct PsEventAckResult {
  std::string request_uuid;
  std::string outcome;
  std::string acknowledgement_uuid;
  std::vector<ParserServerMessageVector> message_vector_set;
};

struct PsEventDisconnectRequest {
  ParserServerEventSession session;
  std::string disconnect_reason;
};

struct PsEventDisconnectResult {
  std::string outcome;
  std::uint64_t removed_count = 0;
  std::vector<ParserServerMessageVector> message_vector_set;
};

class ParserServerEventIpcRuntime {
 public:
  explicit ParserServerEventIpcRuntime(ParserEventNotificationRouter* router);

  PsEventSubscribeResult HandleSubscribe(const PsEventSubscribeRequest& request);
  PsEventUnsubscribeResult HandleUnsubscribe(const PsEventUnsubscribeRequest& request);
  PsEventDeliveryPumpResult PumpCommittedEvents(const PsEventDeliveryPumpRequest& request);
  PsEventAckResult HandleAck(const PsEventAckRequest& request);
  PsEventDisconnectResult HandleDisconnect(const PsEventDisconnectRequest& request);

 private:
  ParserEventNotificationRouter* router_;

  ParserServerMessageVector DiagnosticVector(std::string code,
                                             std::string safe_message_key,
                                             std::string detail,
                                             bool error) const;
  bool SessionReady(const ParserServerEventSession& session,
                    std::vector<ParserServerMessageVector>* vectors,
                    bool allow_while_draining = false) const;
};

}  // namespace scratchbird::server
