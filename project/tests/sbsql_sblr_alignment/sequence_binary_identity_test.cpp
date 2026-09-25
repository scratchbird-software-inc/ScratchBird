// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/binary_uuid_fixture.hpp"
#include "sblr_sequence_runtime.hpp"
#include "../../src/core/uuid/uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace s = scratchbird::engine::sblr;
using scratchbird::tests::FixtureUuid;

void Require(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}
std::int64_t Number(const s::SblrResult& result) {
  Require(result.ok() && result.scalar_values.size() == 1 &&
              result.scalar_values.front().has_int64_value, "sequence result unavailable");
  return result.scalar_values.front().int64_value;
}
s::SblrValue Text(std::string value) {
  s::SblrValue result;
  result.descriptor_id = "text";
  result.payload_kind = s::SblrValuePayloadKind::text;
  result.is_null = false;
  result.text_value = std::move(value);
  return result;
}

int main() {
  s::SblrSequenceRegistry registry;
  s::SblrExecutionContext context;
  context.transaction_uuid = FixtureUuid(6015, 50);
  context.local_transaction_id = 7;
  const auto first = FixtureUuid(6015, 1);
  const auto second = FixtureUuid(6015, 2);
  s::SblrSequenceDefinition definition;
  Require(!s::RegisterSblrSequence(&registry, definition, context).ok(), "nil sequence registered");
  definition.sequence_uuid = first;
  definition.start_value = 5;
  Require(s::RegisterSblrSequence(&registry, definition, context).ok(), "sequence registration failed");
  definition.sequence_uuid = second;
  definition.start_value = 20;
  Require(s::RegisterSblrSequence(&registry, definition, context).ok(), "second sequence registration failed");
  const std::string embedded_zero_name("same\0suffix", 11);
  Require(s::RegisterSblrSequenceAlias(&registry, first, "same", context).ok() &&
              s::RegisterSblrSequenceAlias(&registry, second, embedded_zero_name, context).ok(),
          "sequence alias registration failed");
  Require(!s::RegisterSblrSequenceAlias(&registry, FixtureUuid(6015, 99), "unknown", context).ok(),
          "alias manufactured an unregistered sequence identity");
  s::SblrSequenceRequest request;
  request.context = context;
  request.sequence_name_hint = "same";
  Require(Number(s::NextSblrSequenceValue(&registry, request)) == 5, "alias did not resolve first binary identity");
  request.sequence_name_hint = embedded_zero_name;
  Require(Number(s::NextSblrSequenceValue(&registry, request)) == 20, "alias name truncated at embedded zero");
  request.sequence_uuid = first;
  Require(Number(s::NextSblrSequenceValue(&registry, request)) == 6, "name hint replaced explicit UUID authority");
  Require(registry.evidence.back().sequence_uuid == first &&
              registry.evidence.back().transaction_uuid == context.transaction_uuid,
          "sequence evidence lost native identity");
  request.set_value = 12;
  request.is_called = false;
  Require(Number(s::SetSblrSequenceValue(&registry, request)) == 12 &&
              Number(s::NextSblrSequenceValue(&registry, request)) == 12 &&
              Number(s::NextSblrSequenceValue(&registry, request)) == 13,
          "set/is_called/increment sequence behavior changed");
  s::SblrSequenceAlteration alteration;
  alteration.sequence_name_hint = "same";
  alteration.maximum_value = 13;
  Require(s::AlterSblrSequence(&registry, alteration, context).ok() &&
              !s::NextSblrSequenceValue(&registry, request).ok(), "alias alteration lost canonical sequence bounds");
  const auto states_before = registry.states.size();
  request.sequence_uuid = {};
  request.sequence_name_hint = "unregistered";
  Require(!s::NextSblrSequenceValue(&registry, request).ok() && registry.states.size() == states_before,
          "unknown alias created identity or state");
  for (unsigned bit = 0; bit < 128; ++bit) {
    auto identity = first;
    identity.bytes[bit / 8] ^= std::uint8_t(1u << (bit % 8));
    definition.sequence_uuid = identity;
    definition.start_value = 1000 + bit;
    Require(s::RegisterSblrSequence(&registry, definition, context).ok(), "distinct binary identity registration failed");
    request.sequence_uuid = identity;
    request.sequence_name_hint = "same";
    Require(Number(s::NextSblrSequenceValue(&registry, request)) == 1000 + bit &&
                registry.evidence.back().sequence_uuid == identity,
            "sequence registry ignored one of the 128 identity bits");
  }
  request.sequence_uuid = second;
  request.sequence_name_hint.clear();
  Require(Number(s::CurrentSblrSequenceValue(&registry, request)) == 20, "other sequence state changed");
  auto argument = s::MakeSblrUuidValue(first);
  Require(s::BindSblrSequenceArgumentIdentity(argument, &request) &&
              request.sequence_uuid == first && request.sequence_name_hint.empty(),
          "native sequence argument lost UUID authority");
  argument.text_value = "conflicting";
  Require(!s::BindSblrSequenceArgumentIdentity(argument, &request) && request.sequence_uuid == first,
          "conflicting sequence argument changed request");
  argument = Text(scratchbird::core::uuid::UuidToString(second));
  Require(s::BindSblrSequenceArgumentIdentity(argument, &request) && request.sequence_uuid.is_nil() &&
              request.sequence_name_hint == argument.text_value &&
              !s::CurrentSblrSequenceValue(&registry, request).ok(),
          "UUID-shaped text acquired binary sequence authority");
  Require(s::RegisterSblrSequenceAlias(&registry, second, argument.text_value, context).ok() &&
              Number(s::CurrentSblrSequenceValue(&registry, request)) == 20,
          "explicitly registered UUID-shaped name lost alias semantics");
  argument = Text(embedded_zero_name);
  Require(s::BindSblrSequenceArgumentIdentity(argument, &request) && request.sequence_uuid.is_nil() &&
              request.sequence_name_hint == embedded_zero_name &&
              Number(s::CurrentSblrSequenceValue(&registry, request)) == 20,
          "name boundary did not preserve distinct alias namespace");
  auto invalid_identity = first;
  invalid_identity.bytes[6] = (invalid_identity.bytes[6] & 0x0f) | 0x40;
  Require(!s::BindSblrSequenceArgumentIdentity(s::MakeSblrUuidValue(invalid_identity), &request),
          "user UUIDv4 became engine sequence identity");
  Require(!s::BindSblrSequenceArgumentIdentity(s::MakeSblrUuidValue({}), &request) &&
              !s::BindSblrSequenceArgumentIdentity(Text(""), &request) &&
              !s::BindSblrSequenceArgumentIdentity(Text("same"), nullptr), "invalid sequence argument admitted");
  std::cout << "sequence binary UUID identity checks passed\n";
}
