// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/sblr/sblr_event_notification.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace sb = scratchbird::engine::sblr;
namespace {
[[noreturn]] void Fail(const std::string& what) { std::cerr << what << '\n'; std::exit(1); }
void Require(bool condition, const std::string& what) { if (!condition) Fail(what); }

sb::SblrEventNotificationRecord Record(sb::SblrEventNotificationOpcode opcode) {
  sb::SblrEventNotificationRecord record; record.opcode = opcode;
  record.request_uuid[0] = 1; record.security_context_uuid[0] = 2;
  const auto code = static_cast<std::uint16_t>(opcode);
  if (code < 0x0f07) record.transaction_id = 1;
  unsigned count = 0;
  switch (opcode) {
    case sb::SblrEventNotificationOpcode::channel_create: count=6; break;
    case sb::SblrEventNotificationOpcode::channel_alter: count=7; break;
    case sb::SblrEventNotificationOpcode::channel_drop: count=2; break;
    case sb::SblrEventNotificationOpcode::channel_listen: count=4; break;
    case sb::SblrEventNotificationOpcode::channel_unlisten: count=3; break;
    case sb::SblrEventNotificationOpcode::channel_unlisten_all: count=1; break;
    case sb::SblrEventNotificationOpcode::channel_notify: count=6; break;
    case sb::SblrEventNotificationOpcode::subscription_list: count=3; break;
    case sb::SblrEventNotificationOpcode::delivery_poll: count=4; break;
    case sb::SblrEventNotificationOpcode::delivery_ack: count=5; break;
  }
  for (unsigned tag=1; tag<=count; ++tag) record.fields.push_back({static_cast<std::uint16_t>(tag), {static_cast<std::uint8_t>(tag)}});
  return record;
}
}

int main() {
  for (unsigned raw=0x0f00; raw<=0x0f09; ++raw) {
    const auto opcode = static_cast<sb::SblrEventNotificationOpcode>(raw);
    const auto encoded = sb::EncodeSblrEventNotificationRecord(Record(opcode));
    Require(encoded.ok, "CSC-TEST-003338 encode");
    const auto decoded = sb::DecodeSblrEventNotificationRecord(encoded.canonical_bytes.data(), encoded.canonical_bytes.size());
    Require(decoded.ok && decoded.record.opcode == opcode, "CSC-TEST-003338 roundtrip");
    const auto* entry = sb::LookupSblrOperation(sb::SblrEventNotificationOperationId(opcode));
    Require(entry != nullptr && entry->code == raw && entry->executor_evidence_required && entry->executor_evidence_accepted,
            "CSC-TEST-003339 exact registry evidence");
    auto corrupt = encoded.canonical_bytes; corrupt.back() ^= 1;
    Require(!sb::DecodeSblrEventNotificationRecord(corrupt.data(), corrupt.size()).ok, "CSC-TEST-003338 digest refusal");
  }
  auto malformed = Record(sb::SblrEventNotificationOpcode::delivery_ack);
  malformed.fields.back().tag = 9;
  Require(!sb::EncodeSblrEventNotificationRecord(malformed).ok, "CSC-TEST-003339 ack identity refusal");
  return 0;
}
