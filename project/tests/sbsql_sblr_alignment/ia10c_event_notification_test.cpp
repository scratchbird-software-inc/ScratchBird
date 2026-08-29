// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_event_notification.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sb = scratchbird::engine::sblr;
namespace {
[[noreturn]] void Fail(const std::string& what) { std::cerr << what << '\n'; std::exit(1); }
void Require(bool condition, const std::string& what) { if (!condition) Fail(what); }

using Bytes = std::vector<std::uint8_t>;

Bytes Uuid(std::uint8_t seed) {
  Bytes value(16, 0);
  value[0] = seed;
  value[15] = static_cast<std::uint8_t>(seed + 1);
  return value;
}

Bytes Text(std::string_view value) { return Bytes(value.begin(), value.end()); }

Bytes U32(std::uint32_t value) {
  return {static_cast<std::uint8_t>(value),
          static_cast<std::uint8_t>(value >> 8),
          static_cast<std::uint8_t>(value >> 16),
          static_cast<std::uint8_t>(value >> 24)};
}

Bytes U64(std::uint64_t value) {
  Bytes out(8, 0);
  for (unsigned byte = 0; byte < 8; ++byte) out[byte] = value >> (byte * 8);
  return out;
}

void Add(sb::SblrEventNotificationRecord* record,
         std::uint16_t tag,
         Bytes value) {
  record->fields.push_back({tag, std::move(value)});
}

sb::SblrEventNotificationRecord Record(sb::SblrEventNotificationOpcode opcode) {
  sb::SblrEventNotificationRecord record;
  record.opcode = opcode;
  record.request_uuid[0] = 1; record.security_context_uuid[0] = 2;
  const auto code = static_cast<std::uint16_t>(opcode);
  if (code < 0x0f07) record.transaction_id = 1;
  switch (opcode) {
    case sb::SblrEventNotificationOpcode::channel_create:
      Add(&record, 1, Uuid(1)); Add(&record, 2, Text("channel"));
      Add(&record, 3, Uuid(3)); Add(&record, 4, Uuid(4));
      Add(&record, 5, Text("normal")); Add(&record, 6, Text("none"));
      break;
    case sb::SblrEventNotificationOpcode::channel_alter:
      Add(&record, 1, Uuid(1)); Add(&record, 2, Text("renamed"));
      Add(&record, 3, Uuid(3)); Add(&record, 4, Uuid(4));
      Add(&record, 5, Text("active")); Add(&record, 6, Text("hidden"));
      Add(&record, 7, Text("redact_payload"));
      break;
    case sb::SblrEventNotificationOpcode::channel_drop:
      Add(&record, 1, Uuid(1)); Add(&record, 2, Text("restrict"));
      break;
    case sb::SblrEventNotificationOpcode::channel_listen:
      Add(&record, 1, Uuid(1)); Add(&record, 2, Uuid(2));
      Add(&record, 3, Uuid(3)); Add(&record, 4, Text("native_message"));
      break;
    case sb::SblrEventNotificationOpcode::channel_unlisten:
      Add(&record, 1, Uuid(1)); Add(&record, 2, Uuid(2));
      Add(&record, 3, Uuid(3));
      break;
    case sb::SblrEventNotificationOpcode::channel_unlisten_all:
      Add(&record, 1, Uuid(1));
      break;
    case sb::SblrEventNotificationOpcode::channel_notify:
      Add(&record, 1, Uuid(1)); Add(&record, 2, Text("native_message"));
      Add(&record, 3, Uuid(3)); Add(&record, 4, Text("payload"));
      Add(&record, 5, Bytes(16, 0)); Add(&record, 6, Uuid(6));
      break;
    case sb::SblrEventNotificationOpcode::subscription_list:
      Add(&record, 1, Uuid(1)); Add(&record, 2, U32(16));
      Add(&record, 3, {});
      break;
    case sb::SblrEventNotificationOpcode::delivery_poll:
      Add(&record, 1, Uuid(1)); Add(&record, 2, Uuid(2));
      Add(&record, 3, U32(16)); Add(&record, 4, Bytes(32, 7));
      break;
    case sb::SblrEventNotificationOpcode::delivery_ack:
      Add(&record, 1, Uuid(1)); Add(&record, 2, Uuid(2));
      Add(&record, 3, Uuid(3)); Add(&record, 4, U64(1));
      Add(&record, 5, Uuid(5));
      break;
  }
  return record;
}

struct ExpectedContract {
  const char* operand;
  const char* result;
  sb::SblrOpcodeTransactionEffect effect;
  sb::SblrOpcodeSecurityClass security;
  bool transaction;
};

constexpr std::array<ExpectedContract, 10> kExpected{{
    {"event_channel_create_request", "event_channel_result", sb::SblrOpcodeTransactionEffect::catalog_write, sb::SblrOpcodeSecurityClass::event_admin, true},
    {"event_channel_alter_request", "event_channel_result", sb::SblrOpcodeTransactionEffect::catalog_write, sb::SblrOpcodeSecurityClass::event_admin, true},
    {"event_channel_drop_request", "event_channel_result", sb::SblrOpcodeTransactionEffect::catalog_write, sb::SblrOpcodeSecurityClass::event_admin, true},
    {"event_channel_listen_request", "event_subscription_result", sb::SblrOpcodeTransactionEffect::none, sb::SblrOpcodeSecurityClass::event_subscribe, true},
    {"event_channel_unlisten_request", "event_subscription_result", sb::SblrOpcodeTransactionEffect::none, sb::SblrOpcodeSecurityClass::event_subscribe, true},
    {"event_session_unlisten_all_request", "event_subscription_result", sb::SblrOpcodeTransactionEffect::none, sb::SblrOpcodeSecurityClass::event_subscribe, true},
    {"event_channel_notify_request", "event_publication_result", sb::SblrOpcodeTransactionEffect::publish_pending_commit, sb::SblrOpcodeSecurityClass::event_publish, true},
    {"event_subscription_list_request", "event_subscription_list_result", sb::SblrOpcodeTransactionEffect::none, sb::SblrOpcodeSecurityClass::event_inspect, false},
    {"event_delivery_poll_request", "event_delivery_result", sb::SblrOpcodeTransactionEffect::none, sb::SblrOpcodeSecurityClass::event_subscribe, false},
    {"event_delivery_ack_request", "event_delivery_ack_result", sb::SblrOpcodeTransactionEffect::none, sb::SblrOpcodeSecurityClass::event_subscribe, false},
}};

sb::SblrOperationEnvelope Envelope(
    sb::SblrEventNotificationOpcode opcode,
    const sb::SblrEventNotificationCodecResult& encoded,
    const ExpectedContract& expected) {
  sb::SblrOperationEnvelope envelope;
  envelope.operation_id = sb::SblrEventNotificationOperationId(opcode);
  envelope.opcode = sb::SblrEventNotificationMnemonic(opcode);
  envelope.opcode_code = static_cast<std::uint16_t>(opcode);
  envelope.result_shape = expected.result;
  envelope.diagnostic_shape = "diagnostic_vector";
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = expected.transaction;
  envelope.operands.push_back(sb::MakeSblrEventNotificationOperand(encoded));
  return envelope;
}
}

int main() {
  for (unsigned raw=0x0f00; raw<=0x0f09; ++raw) {
    const auto opcode = static_cast<sb::SblrEventNotificationOpcode>(raw);
    const auto& expected = kExpected[raw - 0x0f00];
    const auto encoded = sb::EncodeSblrEventNotificationRecord(Record(opcode));
    Require(encoded.ok, "exact event carrier encode");
    const auto decoded = sb::DecodeSblrEventNotificationRecord(encoded.canonical_bytes.data(), encoded.canonical_bytes.size());
    Require(decoded.ok && decoded.record.opcode == opcode,
            "exact event carrier roundtrip");
    const auto* entry = sb::LookupSblrOperation(sb::SblrEventNotificationOperationId(opcode));
    Require(entry != nullptr && entry->code == raw &&
                entry->opcode == sb::SblrEventNotificationMnemonic(opcode) &&
                entry->executor_id ==
                    sb::SblrEventNotificationOperationId(opcode) &&
                entry->operand_contract == expected.operand &&
                entry->result_contract == expected.result &&
                entry->transaction_effect == expected.effect &&
                entry->security_class == expected.security &&
                entry->requires_security_context &&
                entry->requires_transaction_context == expected.transaction &&
                !entry->requires_cluster_authority &&
                entry->executor_evidence_required &&
                !entry->executor_evidence_accepted &&
                entry->missing_executor_evidence_diagnostic ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
            "exact fail-closed event registry contract");
    Require(sb::LookupSblrOpcodeCode(raw) == entry,
            "unique exact event opcode code");

    const auto envelope = Envelope(opcode, encoded, expected);
    const auto decoded_operand =
        sb::DecodeSblrEventNotificationOperand(envelope);
    Require(decoded_operand.ok && decoded_operand.record.opcode == opcode,
            "outer and inner event carrier identity binding");
    const auto unavailable = sb::ValidateSblrOpcodeForEnvelope(envelope);
    Require(!unavailable.ok &&
                unavailable.diagnostic_id ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
            "registered event executor evidence refusal");

    scratchbird::engine::internal_api::EngineRequestContext context;
    context.security_context_present = true;
    context.local_transaction_id = expected.transaction ? 1 : 0;
    unsigned cancellation_probes = 0;
    context.query_cancellation_requested = [&] {
      ++cancellation_probes;
      return true;
    };
    const auto refused = sb::DispatchSblrEventNotification(envelope, context);
    Require(!refused.accepted &&
                refused.diagnostic_id ==
                    "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
                refused.evidence.empty() && cancellation_probes == 0,
            "event evidence refusal before cancellation and API dispatch");

    auto corrupt = encoded.canonical_bytes; corrupt.back() ^= 1;
    Require(!sb::DecodeSblrEventNotificationRecord(corrupt.data(), corrupt.size()).ok,
            "event carrier digest refusal");

    auto malformed_envelope = envelope;
    malformed_envelope.operands.front().value_body.back() ^= 1;
    Require(!sb::DecodeSblrEventNotificationOperand(malformed_envelope).ok,
            "malformed event carrier codec refusal");
  }
  auto malformed = Record(sb::SblrEventNotificationOpcode::delivery_ack);
  malformed.fields.back().tag = 9;
  Require(!sb::EncodeSblrEventNotificationRecord(malformed).ok,
          "event acknowledgement field refusal");
  auto oversized = Record(sb::SblrEventNotificationOpcode::channel_create);
  oversized.fields[1].value.assign(129, 'x');
  Require(!sb::EncodeSblrEventNotificationRecord(oversized).ok,
          "event field limit refusal");
  const auto create_encoded = sb::EncodeSblrEventNotificationRecord(
      Record(sb::SblrEventNotificationOpcode::channel_create));
  const auto notify_encoded = sb::EncodeSblrEventNotificationRecord(
      Record(sb::SblrEventNotificationOpcode::channel_notify));
  Require(create_encoded.ok && notify_encoded.ok,
          "cross-bound event fixtures encode");
  auto cross_bound = Envelope(sb::SblrEventNotificationOpcode::channel_create,
                              create_encoded, kExpected.front());
  cross_bound.operands.front().value_body =
      sb::MakeSblrEventNotificationOperand(notify_encoded).value_body;
  Require(!sb::DecodeSblrEventNotificationOperand(cross_bound).ok,
          "outer and inner event opcode cross-bind refusal");
  Require(sb::LookupSblrOperation("event.channel.create") == nullptr &&
              sb::LookupSblrOperation("event.channel.alter") == nullptr &&
              sb::LookupSblrOperation("event.channel.drop") == nullptr &&
              sb::LookupSblrOperation("event.channel.listen") == nullptr &&
              sb::LookupSblrOperation("event.channel.unlisten") == nullptr &&
              sb::LookupSblrOperation("event.channel.notify") == nullptr &&
              sb::LookupSblrOperation("session.notification.unlisten") == nullptr &&
              sb::LookupSblrOperation("session.notification.unlisten_all") == nullptr,
          "event aliases are not canonical SBLR authority");
  return 0;
}
