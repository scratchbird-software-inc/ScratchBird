// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_event_notification.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include <openssl/sha.h>
#include <algorithm>

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
  value[6] = 0x70;
  value[8] = 0x80;
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
  record.request_uuid[6] = record.security_context_uuid[6] = 0x70;
  record.request_uuid[8] = record.security_context_uuid[8] = 0x80;
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
  envelope.trace_key = "ia10c.event-native-carrier";
  envelope.parser_package_uuid = scratchbird::tests::FixtureUuidLiteral(
      "019f2000-0000-7000-8000-000000000002");
  envelope.registry_snapshot_uuid = scratchbird::tests::FixtureUuidLiteral(
      "019f2000-0000-7000-8000-000000000003");
  envelope.operands.push_back(sb::MakeSblrEventNotificationOperand(encoded));
  return envelope;
}

bool IsUuidField(unsigned opcode, unsigned tag) {
  switch (opcode) {
    case 0x0f00: case 0x0f01: return tag == 1 || tag == 3 || tag == 4;
    case 0x0f02: case 0x0f05: case 0x0f07: return tag == 1;
    case 0x0f03: case 0x0f04: return tag <= 3;
    case 0x0f06: return tag == 1 || tag == 3 || tag == 5 || tag == 6;
    case 0x0f08: return tag <= 2;
    case 0x0f09: return tag == 1 || tag == 2 || tag == 3 || tag == 5;
  }
  return false;
}

void CheckSystemIdentity(const sb::SblrEventNotificationRecord& record,
                         const Bytes& canonical, std::size_t offset,
                         int field_index) {
  const auto check = [&](const Bytes& id) {
    auto invalid = record;
    if (field_index >= 0) invalid.fields[field_index].value = id;
    else {
      auto& destination = offset == 16 ? invalid.request_uuid
                                      : invalid.security_context_uuid;
      std::copy(id.begin(), id.end(), destination.begin());
    }
    const auto refused = sb::EncodeSblrEventNotificationRecord(invalid);
    Require(!refused.ok && refused.canonical_bytes.empty(),
            "invalid event system identity encoded");
    auto resealed = canonical;
    std::copy(id.begin(), id.end(), resealed.begin() + offset);
    // Independent integrity recomputation: refusal must be semantic, not merely
    // the checksum failure exercised separately below.
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    Require(SHA256(resealed.data(), resealed.size() - digest.size(),
                   digest.data()) != nullptr, "test SHA256 recomputation");
    std::copy(digest.begin(), digest.end(), resealed.end() - digest.size());
    const auto decoded = sb::DecodeSblrEventNotificationRecord(
        resealed.data(), resealed.size());
    Require(!decoded.ok && decoded.canonical_bytes.empty(),
            "resealed invalid event system identity decoded");
  };
  for (unsigned version = 0; version < 256; ++version) {
    if ((version >> 4) == 7) continue;
    auto id = Uuid(31);
    id[6] = static_cast<std::uint8_t>(version);
    check(id);
  }
  for (unsigned variant = 0; variant < 256; ++variant) {
    if ((variant & 0xc0) == 0x80) continue;
    auto id = Uuid(31);
    id[8] = static_cast<std::uint8_t>(variant);
    check(id);
  }
  // Only NOTIFY source_object_uuid has an explicitly optional nil identity.
  if (!(record.opcode == sb::SblrEventNotificationOpcode::channel_notify &&
        field_index >= 0 && record.fields[field_index].tag == 5)) {
    check(Bytes(16, 0));
  }
}

void CheckCarrier(const sb::SblrOperationEnvelope& envelope,
                  const sb::SblrEventNotificationCodecResult& encoded) {
  constexpr std::array<std::uint8_t, 16> descriptor{
      0x01, 0xa0, 0xea, 0xa7, 0xec, 0x69, 0x78, 0xea,
      0xbb, 0x02, 0x6c, 0xa2, 0xea, 0x89, 0xf9, 0xba};
  Require(std::equal(descriptor.begin(), descriptor.end(),
                     envelope.operands.front().value_body.begin()),
          "independent event carrier descriptor");
  const auto validation = sb::ValidateSblrEnvelope(envelope);
  Require(validation.ok, "event generic native SBOP validation: " +
              sb::SerializeSblrValidationToJson(validation));
  const auto wire = sb::EncodeSblrEnvelope(envelope);
  const auto roundtrip = sb::DecodeSblrEnvelope(wire);
  Require(!wire.empty() && roundtrip.ok &&
              sb::EncodeSblrEnvelope(roundtrip.envelope) == wire,
          "event canonical SBOP roundtrip");
  const auto nested = sb::DecodeSblrEventNotificationOperand(roundtrip.envelope);
  Require(nested.ok && nested.canonical_bytes == encoded.canonical_bytes,
          "event exact request retained across canonical SBOP");
  for (unsigned byte = 0; byte < descriptor.size(); ++byte) {
    for (unsigned value = 0; value < 256; ++value) {
      if (value == descriptor[byte]) continue;
      auto bad = envelope;
      bad.operands.front().value_body[byte] = static_cast<std::uint8_t>(value);
      Require(!sb::DecodeSblrEventNotificationOperand(bad).ok,
              "event descriptor mutation admitted");
    }
  }
  auto bad = envelope;
  std::fill_n(bad.operands.front().value_body.begin(), 16, 0);
  bad.operands.front().value_body[0] = 1;
  Require(!sb::DecodeSblrEventNotificationOperand(bad).ok,
          "legacy one-byte event carrier descriptor admitted");
  bad = envelope;
  bad.operands.front().value = "01a0eaa7-ec69-78ea-bb02-6ca2ea89f9ba";
  Require(!sb::DecodeSblrEventNotificationOperand(bad).ok,
          "legacy event text authority admitted");
  for (unsigned bit = 0; bit < 16; ++bit) {
    bad = envelope;
    bad.operands.front().value_flags = 1U << bit;
    Require(!sb::DecodeSblrEventNotificationOperand(bad).ok,
            "event carrier flags admitted");
  }
  for (std::size_t size = 0; size < encoded.canonical_bytes.size(); ++size) {
    auto truncated = encoded;
    truncated.canonical_bytes.resize(size);
    Require(sb::MakeSblrEventNotificationOperand(truncated).value_body.empty(),
            "event operand builder trusted truncated frame ok flag");
  }
  auto forged = encoded;
  forged.record.opcode = encoded.record.opcode ==
      sb::SblrEventNotificationOpcode::channel_create
          ? sb::SblrEventNotificationOpcode::channel_drop
          : sb::SblrEventNotificationOpcode::channel_create;
  Require(sb::MakeSblrEventNotificationOperand(forged).value_body.empty(),
          "event operand builder trusted mismatched opcode");
  forged = encoded;
  forged.canonical_bytes.back() ^= 1;
  Require(sb::MakeSblrEventNotificationOperand(forged).value_body.empty(),
          "event operand builder trusted corrupted frame ok flag");
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
    CheckCarrier(envelope, encoded);
    const auto record = Record(opcode);
    CheckSystemIdentity(record, encoded.canonical_bytes, 16, -1);
    CheckSystemIdentity(record, encoded.canonical_bytes, 32, -1);
    std::size_t field_offset = 80;
    for (std::size_t i = 0; i < record.fields.size(); ++i) {
      if (IsUuidField(raw, record.fields[i].tag)) {
        CheckSystemIdentity(record, encoded.canonical_bytes, field_offset + 8,
                            static_cast<int>(i));
      }
      field_offset += 8 + record.fields[i].value.size();
    }
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
  auto user_payload = Record(sb::SblrEventNotificationOpcode::channel_notify);
  user_payload.fields[3].value = Text("6ba7b810-9dad-41d1-80b4-00c04fd430c8");
  user_payload.fields[4].value = Uuid(13);
  const auto user_encoded = sb::EncodeSblrEventNotificationRecord(user_payload);
  Require(user_encoded.ok, "optional source UUIDv7 and opaque UUIDv4 user data");
  const auto user_decoded = sb::DecodeSblrEventNotificationRecord(
      user_encoded.canonical_bytes.data(), user_encoded.canonical_bytes.size());
  Require(user_decoded.ok && user_decoded.record.fields[3].value ==
              user_payload.fields[3].value &&
              user_decoded.record.fields[4].value == user_payload.fields[4].value,
          "opaque user UUID data and optional system source preserved");
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
