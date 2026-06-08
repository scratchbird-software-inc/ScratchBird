// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "notification/notification_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "crud_support/crud_store.hpp"
#include "security/security_model.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

struct EventLogRecord {
  std::uint64_t sequence = 0;
  std::string kind;
  std::uint64_t creator_tx = 0;
  std::string object_uuid;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct EventState {
  std::vector<EventLogRecord> records;
  CrudState crud_state;
};

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

std::string FieldValue(const std::vector<std::pair<std::string, std::string>>& fields, const std::string& key) {
  for (const auto& field : fields) {
    if (field.first == key) { return field.second; }
  }
  return {};
}

std::uint64_t ParseU64(const std::string& value, std::uint64_t fallback = 0) {
  try { return static_cast<std::uint64_t>(std::stoull(value)); } catch (...) { return fallback; }
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  return SecurityOptionValue(request, prefix);
}

bool OptionBool(const EngineApiRequest& request, const std::string& prefix, bool fallback = false) {
  return SecurityOptionBool(request, prefix, fallback);
}

std::string ChannelName(const EngineApiRequest& request) {
  const auto from_option = OptionValue(request, "channel:");
  if (!from_option.empty()) { return from_option; }
  if (!request.localized_names.empty() && !request.localized_names.front().name.empty()) {
    return request.localized_names.front().name;
  }
  if (!request.sql_object_reference.object_name.raw_text.empty()) { return request.sql_object_reference.object_name.raw_text; }
  return {};
}

std::string ChannelUuid(const EngineApiRequest& request) {
  const auto from_option = OptionValue(request, "channel_uuid:");
  if (!from_option.empty()) { return from_option; }
  if (request.target_object.object_kind == "event_channel" && !request.target_object.uuid.canonical.empty()) {
    return request.target_object.uuid.canonical;
  }
  if (!request.target_object.uuid.canonical.empty() && request.operation_id.find("event.channel") == 0) {
    return request.target_object.uuid.canonical;
  }
  for (const auto& object : request.related_objects) {
    if (object.object_kind == "event_channel" && !object.uuid.canonical.empty()) { return object.uuid.canonical; }
  }
  return {};
}

std::string SubscriptionUuid(const EngineApiRequest& request) {
  const auto from_option = OptionValue(request, "subscription_uuid:");
  if (!from_option.empty()) { return from_option; }
  if (request.target_object.object_kind == "event_subscription" && !request.target_object.uuid.canonical.empty()) {
    return request.target_object.uuid.canonical;
  }
  return GenerateCrudEngineUuid("event_subscription");
}

std::string PayloadDescriptorUuid(const EngineApiRequest& request) {
  const auto from_option = OptionValue(request, "payload_descriptor_uuid:");
  if (!from_option.empty()) { return from_option; }
  if (!request.descriptors.empty() && !request.descriptors.front().descriptor_uuid.canonical.empty()) {
    return request.descriptors.front().descriptor_uuid.canonical;
  }
  return "event_payload_descriptor:text.v1";
}

std::string PayloadText(const EngineApiRequest& request) {
  const auto from_option = OptionValue(request, "payload:");
  if (!from_option.empty()) { return from_option; }
  if (!request.rows.empty() && !request.rows.front().fields.empty()) {
    return request.rows.front().fields.front().second.encoded_value;
  }
  return {};
}

EngineApiDiagnostic Ok() { return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false); }

EngineApiDiagnostic ValidateEventContext(const EngineApiRequest& request,
                                         const std::string& operation_id,
                                         bool require_transaction,
                                         bool require_security = true) {
  if (request.context.database_path.empty()) { return MakeInvalidRequestDiagnostic(operation_id, "database_path_required"); }
  if (require_transaction && request.context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic(operation_id, "local_transaction_id_required");
  }
  if (require_security && !request.context.security_context_present) {
    return MakeSecurityContextRequiredDiagnostic(operation_id);
  }
  if (!request.context.cluster_authority_available && OptionBool(request, "cluster_event_route:", false)) {
    return MakeEngineApiDiagnostic("EVENT.CLUSTER_UNAVAILABLE", "event.cluster_unavailable", operation_id, true);
  }
  return Ok();
}

EngineApiDiagnostic RequireEventRight(const EngineRequestContext& context,
                                      const std::string& operation_id,
                                      const std::string& right,
                                      const std::string& target_uuid = {}) {
  if (!SecurityContextHasRight(context, right, target_uuid) &&
      !SecurityContextHasRight(context, "EVENT_ADMIN", target_uuid)) {
    return MakeSecurityDiagnostic("EVENT.AUTHORIZATION_DENIED",
                                  operation_id + ":" + right);
  }
  return Ok();
}

EngineApiDiagnostic ValidatePayload(const EngineApiRequest& request,
                                    const std::string& operation_id,
                                    const std::string& payload) {
  const std::uint64_t max_payload = ParseU64(OptionValue(request, "max_payload_bytes:"), 8192);
  if (payload.size() > max_payload) {
    return MakeEngineApiDiagnostic("EVENT.PAYLOAD_TOO_LARGE", "event.payload_too_large", "max_payload_bytes=" + std::to_string(max_payload), true);
  }
  if (payload.find('\0') != std::string::npos) {
    return MakeEngineApiDiagnostic("EVENT.PAYLOAD_INVALID", "event.payload_invalid", "nul_byte_forbidden", true);
  }
  return Ok();
}

std::string MakeEventLine(const std::string& kind,
                          std::uint64_t creator_tx,
                          const std::string& object_uuid,
                          const std::vector<std::pair<std::string, std::string>>& fields) {
  return std::string(kEventNotificationRecordMagic) + "\t" + kind + "\t" + std::to_string(creator_tx) + "\t" +
         object_uuid + "\t" + EncodeCrudPairs(fields);
}

std::string NotificationEventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.notification_events";
}

EngineApiDiagnostic AppendEventRecord(const EngineRequestContext& context,
                                      const std::string& kind,
                                      const std::string& object_uuid,
                                      const std::vector<std::pair<std::string, std::string>>& fields) {
  if (context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic("event.record", "database_path_required");
  }
  std::ofstream out(NotificationEventPath(context), std::ios::app | std::ios::binary);
  if (!out) { return MakeInvalidRequestDiagnostic("event.record", "event_store_unwritable"); }
  out << MakeEventLine(kind, context.local_transaction_id, object_uuid, fields) << '\n';
  out.flush();
  if (!out) { return MakeInvalidRequestDiagnostic("event.record", "event_store_write_failed"); }
  return Ok();
}

EventState LoadEventState(const EngineRequestContext& context) {
  EventState state;
  const auto crud = LoadCrudState(context);
  if (crud.ok) { state.crud_state = crud.state; }
  std::ifstream in(NotificationEventPath(context), std::ios::binary);
  std::string line;
  std::uint64_t sequence = 0;
  while (std::getline(in, line)) {
    ++sequence;
    if (line.rfind(kEventNotificationRecordMagic, 0) != 0) { continue; }
    const auto parts = Split(line, '\t');
    if (parts.size() < 5) { continue; }
    EventLogRecord record;
    record.sequence = sequence;
    record.kind = parts[1];
    record.creator_tx = ParseU64(parts[2]);
    record.object_uuid = parts[3];
    record.fields = DecodeCrudPairs(parts[4]);
    state.records.push_back(std::move(record));
  }
  return state;
}

std::uint64_t ObserverTx(const EventState& state, const EngineRequestContext& context) {
  return context.local_transaction_id != 0 ? context.local_transaction_id : state.crud_state.max_transaction_id + 1;
}

bool RecordVisible(const EventState& state, const EventLogRecord& record, const EngineRequestContext& context) {
  if (record.creator_tx == 0) { return true; }
  return CrudCreatorVisible(state.crud_state, record.creator_tx, record.sequence, ObserverTx(state, context));
}

std::map<std::string, EventChannelShape> VisibleChannels(const EventState& state, const EngineRequestContext& context) {
  std::map<std::string, EventChannelShape> channels;
  for (const auto& record : state.records) {
    if (record.kind != "CHANNEL" || !RecordVisible(state, record, context)) { continue; }
    EventChannelShape channel;
    channel.channel_uuid = record.object_uuid;
    channel.channel_name = FieldValue(record.fields, "channel_name");
    channel.payload_descriptor_uuid = FieldValue(record.fields, "payload_descriptor_uuid");
    channel.queue_policy_uuid = FieldValue(record.fields, "queue_policy_uuid");
    channel.state = FieldValue(record.fields, "state");
    channel.visibility = FieldValue(record.fields, "visibility");
    if (channel.visibility.empty()) { channel.visibility = "normal"; }
    channel.redaction_policy = FieldValue(record.fields, "redaction_policy");
    if (channel.redaction_policy.empty()) { channel.redaction_policy = "none"; }
    if (channel.state.empty()) { channel.state = "active"; }
    if (channel.state == "dropped" || channel.state == "retired") { channels.erase(channel.channel_uuid); }
    else { channels[channel.channel_uuid] = std::move(channel); }
  }
  return channels;
}

std::map<std::string, EventSubscriptionShape> VisibleSubscriptions(const EventState& state,
                                                                   const EngineRequestContext& context) {
  std::map<std::string, EventSubscriptionShape> subscriptions;
  const auto channels = VisibleChannels(state, context);
  for (const auto& record : state.records) {
    if (record.kind != "SUBSCRIPTION" || !RecordVisible(state, record, context)) { continue; }
    EventSubscriptionShape subscription;
    subscription.subscription_uuid = record.object_uuid;
    subscription.session_uuid = FieldValue(record.fields, "session_uuid");
    subscription.principal_uuid = FieldValue(record.fields, "principal_uuid");
    subscription.channel_uuid = FieldValue(record.fields, "channel_uuid");
    subscription.delivery_profile = FieldValue(record.fields, "delivery_profile");
    subscription.state = FieldValue(record.fields, "state");
    if (subscription.state.empty()) { subscription.state = "active"; }
    const std::string key = subscription.session_uuid + "|" + subscription.channel_uuid;
    if (subscription.state == "inactive" || channels.count(subscription.channel_uuid) == 0) { subscriptions.erase(key); }
    else { subscriptions[key] = std::move(subscription); }
  }
  return subscriptions;
}

std::vector<EventPublicationShape> VisiblePublications(const EventState& state,
                                                       const EngineRequestContext& context,
                                                       const std::string& channel_uuid_filter) {
  std::vector<EventPublicationShape> out;
  const auto channels = VisibleChannels(state, context);
  for (const auto& record : state.records) {
    if (record.kind != "PUBLICATION" || !RecordVisible(state, record, context)) { continue; }
    EventPublicationShape publication;
    publication.event_uuid = record.object_uuid;
    publication.channel_uuid = FieldValue(record.fields, "channel_uuid");
    if (!channel_uuid_filter.empty() && publication.channel_uuid != channel_uuid_filter) { continue; }
    if (channels.count(publication.channel_uuid) == 0) { continue; }
    publication.payload_descriptor_uuid = FieldValue(record.fields, "payload_descriptor_uuid");
    publication.payload = FieldValue(record.fields, "payload");
    publication.redaction_state = FieldValue(record.fields, "redaction_state");
    if (publication.redaction_state.empty()) { publication.redaction_state = "clean"; }
    publication.source_object_uuid = FieldValue(record.fields, "source_object_uuid");
    publication.local_transaction_id = record.creator_tx;
    publication.event_sequence = record.sequence;
    publication.state = "deliverable";
    out.push_back(std::move(publication));
  }
  return out;
}

std::set<std::string> VisibleAcknowledgementKeys(const EventState& state,
                                                 const EngineRequestContext& context) {
  std::set<std::string> acknowledgements;
  for (const auto& record : state.records) {
    if (record.kind != "ACK" || !RecordVisible(state, record, context)) { continue; }
    acknowledgements.insert(FieldValue(record.fields, "session_uuid") + "|" +
                            FieldValue(record.fields, "subscription_uuid") + "|" +
                            FieldValue(record.fields, "event_uuid"));
  }
  return acknowledgements;
}

std::string RedactedPayloadForChannel(const EventChannelShape& channel,
                                      const EventPublicationShape& publication,
                                      const EngineRequestContext& context) {
  if (channel.redaction_policy == "redact_payload" &&
      !SecurityContextHasRight(context, "EVENT_DELIVERY_READ", channel.channel_uuid)) {
    return "<redacted>";
  }
  if (publication.redaction_state == "redacted") return "<redacted>";
  return publication.payload;
}

std::string ResolveChannelUuidOrError(const EngineApiRequest& request,
                                      const EventState& state,
                                      EngineApiDiagnostic* diagnostic) {
  std::string channel_uuid = ChannelUuid(request);
  const auto channels = VisibleChannels(state, request.context);
  if (!channel_uuid.empty()) {
    if (channels.count(channel_uuid) == 0 && request.operation_id != "event.channel.create") {
      *diagnostic = MakeEngineApiDiagnostic("EVENT.CHANNEL_NOT_FOUND", "event.channel_not_found", channel_uuid, true);
      return {};
    }
    return channel_uuid;
  }
  const auto channel_name = ChannelName(request);
  if (channel_name.empty()) {
    *diagnostic = MakeInvalidRequestDiagnostic(request.operation_id, "event_channel_required");
    return {};
  }
  for (const auto& [uuid, channel] : channels) {
    if (channel.channel_name == channel_name) { return uuid; }
  }
  *diagnostic = MakeEngineApiDiagnostic("EVENT.CHANNEL_NOT_FOUND", "event.channel_not_found", channel_name, true);
  return {};
}

void AddEventRow(EngineApiResult* result, std::vector<std::pair<std::string, std::string>> fields) {
  result->result_shape.result_kind = "event_notification_rows";
  result->result_shape.rows.push_back(ApiBehaviorRow(std::move(fields)));
}

void AddChannelRow(EngineApiResult* result, const EventChannelShape& channel) {
  AddEventRow(result, {{"channel_uuid", channel.channel_uuid},
                       {"channel_name", channel.channel_name},
                       {"payload_descriptor_uuid", channel.payload_descriptor_uuid},
                       {"queue_policy_uuid", channel.queue_policy_uuid},
                       {"state", channel.state},
                       {"visibility", channel.visibility},
                       {"redaction_policy", channel.redaction_policy}});
}

void AddSubscriptionRow(EngineApiResult* result, const EventSubscriptionShape& subscription) {
  AddEventRow(result, {{"subscription_uuid", subscription.subscription_uuid},
                       {"session_uuid", subscription.session_uuid},
                       {"principal_uuid", subscription.principal_uuid},
                       {"channel_uuid", subscription.channel_uuid},
                       {"delivery_profile", subscription.delivery_profile},
                       {"state", subscription.state}});
}

void AddPublicationRow(EngineApiResult* result, const EventPublicationShape& publication) {
  AddEventRow(result, {{"event_uuid", publication.event_uuid},
                       {"channel_uuid", publication.channel_uuid},
                       {"payload_descriptor_uuid", publication.payload_descriptor_uuid},
                       {"payload", publication.payload},
                       {"redaction_state", publication.redaction_state},
                       {"source_object_uuid", publication.source_object_uuid},
                       {"local_transaction_id", std::to_string(publication.local_transaction_id)},
                       {"event_sequence", std::to_string(publication.event_sequence)},
                       {"state", publication.state}});
}

}  // namespace

// SEARCH_KEY: EVN_IMPL_003_ENGINE_NOTIFICATION_API
EngineCreateEventChannelResult EngineCreateEventChannel(const EngineCreateEventChannelRequest& request) {
  const std::string operation_id = "event.channel.create";
  auto status = ValidateEventContext(request, operation_id, true);
  if (status.error) { return MakeCrudDiagnosticResult<EngineCreateEventChannelResult>(request.context, operation_id, status); }
  status = RequireEventRight(request.context, operation_id, "EVENT_CREATE");
  if (status.error) { return MakeCrudDiagnosticResult<EngineCreateEventChannelResult>(request.context, operation_id, status); }
  EventChannelShape channel;
  channel.channel_uuid = ChannelUuid(request).empty() ? GenerateCrudEngineUuid("event_channel") : ChannelUuid(request);
  channel.channel_name = ChannelName(request).empty() ? channel.channel_uuid : ChannelName(request);
  channel.payload_descriptor_uuid = PayloadDescriptorUuid(request);
  channel.queue_policy_uuid = OptionValue(request, "queue_policy_uuid:").empty() ? "event.queue.ephemeral_session.default" : OptionValue(request, "queue_policy_uuid:");
  channel.state = "active";
  channel.visibility = OptionValue(request, "visibility:").empty() ? "normal" : OptionValue(request, "visibility:");
  channel.redaction_policy = OptionValue(request, "redaction_policy:").empty() ? "none" : OptionValue(request, "redaction_policy:");
  status = AppendEventRecord(request.context, "CHANNEL", channel.channel_uuid,
                             {{"channel_name", channel.channel_name},
                              {"payload_descriptor_uuid", channel.payload_descriptor_uuid},
                              {"queue_policy_uuid", channel.queue_policy_uuid},
                              {"state", channel.state},
                              {"visibility", channel.visibility},
                              {"redaction_policy", channel.redaction_policy}});
  if (status.error) { return MakeCrudDiagnosticResult<EngineCreateEventChannelResult>(request.context, operation_id, status); }
  auto result = MakeCrudSuccessResult<EngineCreateEventChannelResult>(request.context, operation_id);
  result.channel = channel;
  result.primary_object = {{channel.channel_uuid}, "event_channel"};
  result.evidence.push_back({"event_channel", channel.channel_uuid});
  AddChannelRow(&result, channel);
  return result;
}

EngineAlterEventChannelResult EngineAlterEventChannel(const EngineAlterEventChannelRequest& request) {
  const std::string operation_id = "event.channel.alter";
  auto status = ValidateEventContext(request, operation_id, true);
  if (status.error) { return MakeCrudDiagnosticResult<EngineAlterEventChannelResult>(request.context, operation_id, status); }
  const auto state = LoadEventState(request.context);
  EngineApiDiagnostic diagnostic;
  const auto channel_uuid = ResolveChannelUuidOrError(request, state, &diagnostic);
  if (channel_uuid.empty()) { return MakeCrudDiagnosticResult<EngineAlterEventChannelResult>(request.context, operation_id, diagnostic); }
  status = RequireEventRight(request.context, operation_id, "EVENT_ALTER", channel_uuid);
  if (status.error) { return MakeCrudDiagnosticResult<EngineAlterEventChannelResult>(request.context, operation_id, status); }
  EventChannelShape channel;
  channel.channel_uuid = channel_uuid;
  channel.channel_name = ChannelName(request).empty() ? channel_uuid : ChannelName(request);
  channel.payload_descriptor_uuid = PayloadDescriptorUuid(request);
  channel.queue_policy_uuid = OptionValue(request, "queue_policy_uuid:").empty() ? "event.queue.ephemeral_session.default" : OptionValue(request, "queue_policy_uuid:");
  channel.state = OptionValue(request, "state:").empty() ? "active" : OptionValue(request, "state:");
  channel.visibility = OptionValue(request, "visibility:").empty() ? "normal" : OptionValue(request, "visibility:");
  channel.redaction_policy = OptionValue(request, "redaction_policy:").empty() ? "none" : OptionValue(request, "redaction_policy:");
  status = AppendEventRecord(request.context, "CHANNEL", channel.channel_uuid,
                             {{"channel_name", channel.channel_name}, {"payload_descriptor_uuid", channel.payload_descriptor_uuid},
                              {"queue_policy_uuid", channel.queue_policy_uuid}, {"state", channel.state},
                              {"visibility", channel.visibility}, {"redaction_policy", channel.redaction_policy}});
  if (status.error) { return MakeCrudDiagnosticResult<EngineAlterEventChannelResult>(request.context, operation_id, status); }
  auto result = MakeCrudSuccessResult<EngineAlterEventChannelResult>(request.context, operation_id);
  result.channel = channel;
  result.primary_object = {{channel.channel_uuid}, "event_channel"};
  result.evidence.push_back({"event_channel_alter", channel.channel_uuid});
  AddChannelRow(&result, channel);
  return result;
}

EngineDropEventChannelResult EngineDropEventChannel(const EngineDropEventChannelRequest& request) {
  const std::string operation_id = "event.channel.drop";
  auto status = ValidateEventContext(request, operation_id, true);
  if (status.error) { return MakeCrudDiagnosticResult<EngineDropEventChannelResult>(request.context, operation_id, status); }
  const auto state = LoadEventState(request.context);
  EngineApiDiagnostic diagnostic;
  const auto channel_uuid = ResolveChannelUuidOrError(request, state, &diagnostic);
  if (channel_uuid.empty()) { return MakeCrudDiagnosticResult<EngineDropEventChannelResult>(request.context, operation_id, diagnostic); }
  status = RequireEventRight(request.context, operation_id, "EVENT_DROP", channel_uuid);
  if (status.error) { return MakeCrudDiagnosticResult<EngineDropEventChannelResult>(request.context, operation_id, status); }
  EventChannelShape channel{channel_uuid, ChannelName(request), PayloadDescriptorUuid(request), "event.queue.ephemeral_session.default", "dropped", "normal", "none"};
  status = AppendEventRecord(request.context, "CHANNEL", channel_uuid,
                             {{"channel_name", channel.channel_name}, {"payload_descriptor_uuid", channel.payload_descriptor_uuid},
                              {"queue_policy_uuid", channel.queue_policy_uuid}, {"state", "dropped"},
                              {"visibility", channel.visibility}, {"redaction_policy", channel.redaction_policy}});
  if (status.error) { return MakeCrudDiagnosticResult<EngineDropEventChannelResult>(request.context, operation_id, status); }
  auto result = MakeCrudSuccessResult<EngineDropEventChannelResult>(request.context, operation_id);
  result.channel = channel;
  result.evidence.push_back({"event_channel_drop", channel_uuid});
  AddChannelRow(&result, channel);
  return result;
}

EngineListenNotificationResult EngineListenNotification(const EngineListenNotificationRequest& request) {
  const std::string operation_id = "event.channel.listen";
  auto status = ValidateEventContext(request, operation_id, OptionBool(request, "commit_delayed:", false));
  if (status.error) { return MakeCrudDiagnosticResult<EngineListenNotificationResult>(request.context, operation_id, status); }
  const auto state = LoadEventState(request.context);
  EngineApiDiagnostic diagnostic;
  const auto channel_uuid = ResolveChannelUuidOrError(request, state, &diagnostic);
  if (channel_uuid.empty()) { return MakeCrudDiagnosticResult<EngineListenNotificationResult>(request.context, operation_id, diagnostic); }
  status = RequireEventRight(request.context, operation_id, "EVENT_SUBSCRIBE", channel_uuid);
  if (status.error) { return MakeCrudDiagnosticResult<EngineListenNotificationResult>(request.context, operation_id, status); }
  EventSubscriptionShape subscription;
  subscription.subscription_uuid = SubscriptionUuid(request);
  subscription.session_uuid = request.context.session_uuid.canonical.empty() ? OptionValue(request, "session_uuid:") : request.context.session_uuid.canonical;
  subscription.principal_uuid = request.context.principal_uuid.canonical.empty() ? OptionValue(request, "principal_uuid:") : request.context.principal_uuid.canonical;
  subscription.channel_uuid = channel_uuid;
  subscription.delivery_profile = OptionValue(request, "delivery_profile:").empty() ? "ephemeral_session" : OptionValue(request, "delivery_profile:");
  subscription.state = "active";
  if (subscription.session_uuid.empty()) { return MakeCrudDiagnosticResult<EngineListenNotificationResult>(request.context, operation_id, MakeInvalidRequestDiagnostic(operation_id, "session_uuid_required")); }
  status = AppendEventRecord(request.context, "SUBSCRIPTION", subscription.subscription_uuid,
                             {{"session_uuid", subscription.session_uuid}, {"principal_uuid", subscription.principal_uuid},
                              {"channel_uuid", subscription.channel_uuid}, {"delivery_profile", subscription.delivery_profile}, {"state", "active"}});
  if (status.error) { return MakeCrudDiagnosticResult<EngineListenNotificationResult>(request.context, operation_id, status); }
  auto result = MakeCrudSuccessResult<EngineListenNotificationResult>(request.context, operation_id);
  result.subscription = subscription;
  result.primary_object = {{subscription.subscription_uuid}, "event_subscription"};
  result.evidence.push_back({"event_subscription", subscription.subscription_uuid});
  AddSubscriptionRow(&result, subscription);
  return result;
}

EngineUnlistenNotificationResult EngineUnlistenNotification(const EngineUnlistenNotificationRequest& request) {
  const std::string operation_id = "event.channel.unlisten";
  auto status = ValidateEventContext(request, operation_id, OptionBool(request, "commit_delayed:", false));
  if (status.error) { return MakeCrudDiagnosticResult<EngineUnlistenNotificationResult>(request.context, operation_id, status); }
  const auto state = LoadEventState(request.context);
  EngineApiDiagnostic diagnostic;
  const auto channel_uuid = ResolveChannelUuidOrError(request, state, &diagnostic);
  if (channel_uuid.empty()) { return MakeCrudDiagnosticResult<EngineUnlistenNotificationResult>(request.context, operation_id, diagnostic); }
  status = RequireEventRight(request.context, operation_id, "EVENT_SUBSCRIBE", channel_uuid);
  if (status.error) { return MakeCrudDiagnosticResult<EngineUnlistenNotificationResult>(request.context, operation_id, status); }
  EventSubscriptionShape subscription;
  subscription.subscription_uuid = SubscriptionUuid(request);
  subscription.session_uuid = request.context.session_uuid.canonical.empty() ? OptionValue(request, "session_uuid:") : request.context.session_uuid.canonical;
  subscription.principal_uuid = request.context.principal_uuid.canonical;
  subscription.channel_uuid = channel_uuid;
  subscription.delivery_profile = "ephemeral_session";
  subscription.state = "inactive";
  status = AppendEventRecord(request.context, "SUBSCRIPTION", subscription.subscription_uuid,
                             {{"session_uuid", subscription.session_uuid}, {"principal_uuid", subscription.principal_uuid},
                              {"channel_uuid", subscription.channel_uuid}, {"delivery_profile", subscription.delivery_profile}, {"state", "inactive"}});
  if (status.error) { return MakeCrudDiagnosticResult<EngineUnlistenNotificationResult>(request.context, operation_id, status); }
  auto result = MakeCrudSuccessResult<EngineUnlistenNotificationResult>(request.context, operation_id);
  result.removed_count = 1;
  result.evidence.push_back({"event_unlisten", channel_uuid});
  AddSubscriptionRow(&result, subscription);
  return result;
}

EngineNotifyEventChannelResult EngineNotifyEventChannel(const EngineNotifyEventChannelRequest& request) {
  const std::string operation_id = "event.channel.notify";
  auto status = ValidateEventContext(request, operation_id, true);
  if (status.error) { return MakeCrudDiagnosticResult<EngineNotifyEventChannelResult>(request.context, operation_id, status); }
  const auto state = LoadEventState(request.context);
  EngineApiDiagnostic diagnostic;
  const auto channel_uuid = ResolveChannelUuidOrError(request, state, &diagnostic);
  if (channel_uuid.empty()) { return MakeCrudDiagnosticResult<EngineNotifyEventChannelResult>(request.context, operation_id, diagnostic); }
  status = RequireEventRight(request.context, operation_id, "EVENT_PUBLISH", channel_uuid);
  if (status.error) { return MakeCrudDiagnosticResult<EngineNotifyEventChannelResult>(request.context, operation_id, status); }
  if (OptionBool(request, "savepoint_rolled_back:", false)) {
    return MakeCrudDiagnosticResult<EngineNotifyEventChannelResult>(
        request.context,
        operation_id,
        MakeEngineApiDiagnostic("EVENT.SAVEPOINT_ROLLED_BACK",
                                "event.savepoint_rolled_back",
                                "publication_discarded_before_commit",
                                true));
  }
  const auto payload = PayloadText(request);
  status = ValidatePayload(request, operation_id, payload);
  if (status.error) { return MakeCrudDiagnosticResult<EngineNotifyEventChannelResult>(request.context, operation_id, status); }
  EventPublicationShape publication;
  publication.event_uuid = GenerateCrudEngineUuid("event");
  publication.channel_uuid = channel_uuid;
  publication.payload_descriptor_uuid = PayloadDescriptorUuid(request);
  publication.payload = payload;
  publication.redaction_state = OptionBool(request, "redact_payload:", false) ? "redacted" : "clean";
  publication.source_object_uuid = OptionValue(request, "source_object_uuid:");
  if (publication.source_object_uuid.empty()) { publication.source_object_uuid = request.target_object.uuid.canonical; }
  publication.local_transaction_id = request.context.local_transaction_id;
  publication.state = "pending_commit";
  status = AppendEventRecord(request.context, "PUBLICATION", publication.event_uuid,
                             {{"channel_uuid", publication.channel_uuid}, {"payload_descriptor_uuid", publication.payload_descriptor_uuid},
                              {"payload", publication.payload}, {"redaction_state", publication.redaction_state},
                              {"source_object_uuid", publication.source_object_uuid}, {"state", "pending_commit"}});
  if (status.error) { return MakeCrudDiagnosticResult<EngineNotifyEventChannelResult>(request.context, operation_id, status); }
  auto result = MakeCrudSuccessResult<EngineNotifyEventChannelResult>(request.context, operation_id);
  result.publication = publication;
  result.primary_object = {{publication.event_uuid}, "event_publication"};
  result.evidence.push_back({"event_publication", publication.event_uuid});
  AddPublicationRow(&result, publication);
  return result;
}

EngineListEventSubscriptionsResult EngineListEventSubscriptions(const EngineListEventSubscriptionsRequest& request) {
  const std::string operation_id = "event.subscription.list";
  auto status = ValidateEventContext(request, operation_id, false);
  if (status.error) { return MakeCrudDiagnosticResult<EngineListEventSubscriptionsResult>(request.context, operation_id, status); }
  status = RequireEventRight(request.context, operation_id, "EVENT_DELIVERY_READ");
  if (status.error) { return MakeCrudDiagnosticResult<EngineListEventSubscriptionsResult>(request.context, operation_id, status); }
  const auto state = LoadEventState(request.context);
  auto result = MakeCrudSuccessResult<EngineListEventSubscriptionsResult>(request.context, operation_id);
  for (const auto& [key, subscription] : VisibleSubscriptions(state, request.context)) {
    if (!request.context.session_uuid.canonical.empty() && subscription.session_uuid != request.context.session_uuid.canonical &&
        !OptionBool(request, "admin_scope:", false)) {
      continue;
    }
    result.subscriptions.push_back(subscription);
    AddSubscriptionRow(&result, subscription);
  }
  return result;
}

EnginePollEventDeliveryResult EnginePollEventDelivery(const EnginePollEventDeliveryRequest& request) {
  const std::string operation_id = "event.delivery.poll";
  auto status = ValidateEventContext(request, operation_id, false);
  if (status.error) { return MakeCrudDiagnosticResult<EnginePollEventDeliveryResult>(request.context, operation_id, status); }
  status = RequireEventRight(request.context, operation_id, "EVENT_DELIVERY_READ");
  if (status.error) { return MakeCrudDiagnosticResult<EnginePollEventDeliveryResult>(request.context, operation_id, status); }
  const auto state = LoadEventState(request.context);
  const auto subscriptions = VisibleSubscriptions(state, request.context);
  const auto channels = VisibleChannels(state, request.context);
  const auto acknowledged = VisibleAcknowledgementKeys(state, request.context);
  auto result = MakeCrudSuccessResult<EnginePollEventDeliveryResult>(request.context, operation_id);
  for (const auto& [key, subscription] : subscriptions) {
    if (!request.context.session_uuid.canonical.empty() && subscription.session_uuid != request.context.session_uuid.canonical) { continue; }
    for (auto publication : VisiblePublications(state, request.context, subscription.channel_uuid)) {
      const std::string ack_key = subscription.session_uuid + "|" + subscription.subscription_uuid + "|" + publication.event_uuid;
      if (acknowledged.count(ack_key) != 0) { continue; }
      const auto channel = channels.find(publication.channel_uuid);
      if (channel != channels.end()) {
        publication.payload = RedactedPayloadForChannel(channel->second, publication, request.context);
        if (publication.payload == "<redacted>") publication.redaction_state = "redacted";
      }
      publication.state = "deliverable";
      result.deliverable_events.push_back(publication);
      AddPublicationRow(&result, publication);
    }
  }
  result.evidence.push_back({"event_delivery_poll", std::to_string(result.deliverable_events.size())});
  return result;
}

EngineAcknowledgeEventDeliveryResult EngineAcknowledgeEventDelivery(const EngineAcknowledgeEventDeliveryRequest& request) {
  const std::string operation_id = "event.delivery.ack";
  auto status = ValidateEventContext(request, operation_id, false);
  if (status.error) { return MakeCrudDiagnosticResult<EngineAcknowledgeEventDeliveryResult>(request.context, operation_id, status); }
  status = RequireEventRight(request.context, operation_id, "EVENT_DELIVERY_ACK");
  if (status.error) { return MakeCrudDiagnosticResult<EngineAcknowledgeEventDeliveryResult>(request.context, operation_id, status); }
  const auto state = LoadEventState(request.context);
  const auto subscription_uuid = OptionValue(request, "subscription_uuid:");
  const auto event_uuid = OptionValue(request, "event_uuid:");
  bool subscription_found = false;
  std::string channel_uuid;
  for (const auto& [key, subscription] : VisibleSubscriptions(state, request.context)) {
    if (subscription.subscription_uuid == subscription_uuid &&
        (request.context.session_uuid.canonical.empty() ||
         subscription.session_uuid == request.context.session_uuid.canonical)) {
      subscription_found = true;
      channel_uuid = subscription.channel_uuid;
      break;
    }
  }
  bool event_found = false;
  for (const auto& publication : VisiblePublications(state, request.context, channel_uuid)) {
    if (publication.event_uuid == event_uuid) {
      event_found = true;
      break;
    }
  }
  if (!subscription_found || !event_found || subscription_uuid.empty() || event_uuid.empty()) {
    return MakeCrudDiagnosticResult<EngineAcknowledgeEventDeliveryResult>(
        request.context,
        operation_id,
        MakeEngineApiDiagnostic("EVENT.ACK_INVALID",
                                "event.ack_invalid",
                                "subscription_or_event_not_deliverable",
                                true));
  }
  const auto acknowledgement_uuid = GenerateCrudEngineUuid("event_ack");
  status = AppendEventRecord(request.context, "ACK", acknowledgement_uuid,
                             {{"session_uuid", request.context.session_uuid.canonical},
                              {"subscription_uuid", subscription_uuid},
                              {"event_uuid", event_uuid},
                              {"state", "acknowledged"}});
  if (status.error) { return MakeCrudDiagnosticResult<EngineAcknowledgeEventDeliveryResult>(request.context, operation_id, status); }
  EngineAcknowledgeEventDeliveryResult result = MakeCrudSuccessResult<EngineAcknowledgeEventDeliveryResult>(request.context, operation_id);
  result.acknowledgement_uuid = acknowledgement_uuid;
  result.evidence.push_back({"event_delivery_ack", result.acknowledgement_uuid});
  AddEventRow(&result, {{"acknowledgement_uuid", result.acknowledgement_uuid}, {"event_uuid", event_uuid}, {"state", "acknowledged"}});
  return result;
}

EngineUnlistenSessionNotificationsResult EngineUnlistenSessionNotifications(const EngineUnlistenSessionNotificationsRequest& request) {
  const std::string operation_id = "session.notification.unlisten_all";
  auto status = ValidateEventContext(request, operation_id, OptionBool(request, "commit_delayed:", false));
  if (status.error) { return MakeCrudDiagnosticResult<EngineUnlistenSessionNotificationsResult>(request.context, operation_id, status); }
  status = RequireEventRight(request.context, operation_id, "EVENT_SUBSCRIBE");
  if (status.error) { return MakeCrudDiagnosticResult<EngineUnlistenSessionNotificationsResult>(request.context, operation_id, status); }
  const auto state = LoadEventState(request.context);
  auto result = MakeCrudSuccessResult<EngineUnlistenSessionNotificationsResult>(request.context, operation_id);
  const auto session_uuid = request.context.session_uuid.canonical.empty() ? OptionValue(request, "session_uuid:") : request.context.session_uuid.canonical;
  for (const auto& [key, subscription] : VisibleSubscriptions(state, request.context)) {
    if (subscription.session_uuid != session_uuid) { continue; }
    status = AppendEventRecord(request.context, "SUBSCRIPTION", subscription.subscription_uuid,
                               {{"session_uuid", subscription.session_uuid}, {"principal_uuid", subscription.principal_uuid},
                                {"channel_uuid", subscription.channel_uuid}, {"delivery_profile", subscription.delivery_profile}, {"state", "inactive"}});
    if (status.error) { return MakeCrudDiagnosticResult<EngineUnlistenSessionNotificationsResult>(request.context, operation_id, status); }
    ++result.removed_count;
  }
  result.evidence.push_back({"event_unlisten_all", std::to_string(result.removed_count)});
  AddEventRow(&result, {{"removed_count", std::to_string(result.removed_count)}, {"state", "inactive"}});
  return result;
}

}  // namespace scratchbird::engine::internal_api
