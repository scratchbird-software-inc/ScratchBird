// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/binary_uuid_fixture.hpp"
#include "sblr_context_variables.hpp"
#include "sblr_projection_value_runtime.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace sblr = scratchbird::engine::sblr;
using scratchbird::tests::FixtureUuid;

void Require(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

void RequireUuid(const sblr::SblrResult& result, const sblr::SblrUuid& expected) {
  Require(result.ok() && result.scalar_values.size() == 1, "UUID context result unavailable");
  const auto& value = result.scalar_values.front();
  Require(!value.is_null && value.descriptor_id == "uuid" &&
              value.payload_kind == sblr::SblrValuePayloadKind::uuid_binary &&
              value.uuid_value == expected && value.text_value.empty() &&
              value.encoded_value.empty() && value.binary_value.empty() &&
              value.uuid_array_value.empty(), "UUID context result lost bits or acquired text authority");
  std::vector<std::uint8_t> transferred;
  Require(sblr::CopySblrUuidPayload(value, &transferred) &&
              transferred == std::vector<std::uint8_t>(expected.bytes.begin(), expected.bytes.end()),
          "UUID context payload transfer was not exact binary16");
}

int main() {
  struct Binding {
    std::string_view variable;
    sblr::SblrUuid sblr::SblrExecutionContext::*member;
    bool required;
  };
  const std::array bindings{
      Binding{"ctx_current_user_uuid", &sblr::SblrExecutionContext::user_uuid, true},
      Binding{"ctx_current_role_uuid", &sblr::SblrExecutionContext::current_role_uuid, false},
      Binding{"ctx_current_schema_uuid", &sblr::SblrExecutionContext::current_schema_uuid, true},
      Binding{"ctx_current_database_uuid", &sblr::SblrExecutionContext::database_uuid, true},
      Binding{"ctx_current_cluster_uuid", &sblr::SblrExecutionContext::cluster_uuid, false},
      Binding{"ctx_current_node_uuid", &sblr::SblrExecutionContext::node_uuid, true},
      Binding{"ctx_current_session_uuid", &sblr::SblrExecutionContext::session_uuid, true},
      Binding{"ctx_current_attachment_uuid", &sblr::SblrExecutionContext::attachment_uuid, true},
      Binding{"ctx_current_transaction_uuid", &sblr::SblrExecutionContext::transaction_uuid, false},
      Binding{"ctx_current_statement_uuid", &sblr::SblrExecutionContext::statement_uuid, true},
      Binding{"ctx_parser_profile_uuid", &sblr::SblrExecutionContext::parser_profile_uuid, true},
      Binding{"ctx_client_protocol_uuid", &sblr::SblrExecutionContext::client_protocol_uuid, false}};
  const auto identity = FixtureUuid(6014, 1);
  for (const auto& binding : bindings) {
    sblr::SblrExecutionContext context;
    const auto absent = sblr::ResolveSblrContextVariable(binding.variable, context);
    if (binding.required) {
      Require(!absent.ok() && !absent.diagnostics.empty(), "missing required UUID was admitted");
    } else {
      Require(absent.ok() && absent.scalar_values.size() == 1 &&
                  absent.scalar_values.front().is_null, "optional nil UUID lost null semantics");
    }
    context.*binding.member = identity;
    RequireUuid(sblr::ResolveSblrContextVariable(binding.variable, context), identity);
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto changed = identity;
      changed.bytes[bit / 8] ^= std::uint8_t(1u << (bit % 8));
      context.*binding.member = changed;
      RequireUuid(sblr::ResolveSblrContextVariable(binding.variable, context), changed);
    }
  }
  sblr::SblrExecutionContext context;
  context.user_uuid = identity;
  context.transaction_uuid = FixtureUuid(6014, 2);
  RequireUuid(sblr::ResolveSblrContextVariable("CURRENT_USER", context), identity);
  RequireUuid(sblr::ResolveSblrContextVariable("CURRENT_TRANSACTION", context), context.transaction_uuid);
  context.current_group_uuid_set = {identity, FixtureUuid(6014, 2), FixtureUuid(6014, 3)};
  Require(!sblr::ResolveSblrContextVariable("ctx_current_group_uuid_set", context).ok(),
          "group set escaped its security context gate");
  context.security_context_present = true;
  const auto groups = sblr::ResolveSblrContextVariable("ctx_current_group_uuid_set", context);
  Require(groups.ok() && groups.scalar_values.size() == 1, "group set unavailable");
  const auto& value = groups.scalar_values.front();
  Require(!value.is_null && value.descriptor_id == "uuid_array" &&
              value.payload_kind == sblr::SblrValuePayloadKind::uuid_array_binary &&
              value.uuid_array_value == context.current_group_uuid_set &&
              value.uuid_value.is_nil() && value.text_value.empty() &&
              value.encoded_value.empty() && value.binary_value.empty(),
          "group set lost native UUID elements");
  const auto projected = sblr::EngineTypedValueFromSblrValue(value);
  std::vector<std::uint8_t> expected;
  for (const auto& group : context.current_group_uuid_set)
    expected.insert(expected.end(), group.bytes.begin(), group.bytes.end());
  Require(projected.binary_value == expected && projected.encoded_value.empty(),
          "UUID group projection did not preserve consecutive binary16 values");
  scratchbird::engine::internal_api::EngineProjectionFunctionArgument argument;
  argument.type_name = "uuid_array";
  argument.binary_value = expected;
  Require(sblr::ProjectionArgumentEncodingValid(argument), "binary UUID array projection refused");
  const auto roundtrip = sblr::SblrValueFromProjectionArgument(argument);
  Require(roundtrip.uuid_array_value == value.uuid_array_value &&
              sblr::ProjectionSblrValueResolved(roundtrip), "UUID array projection roundtrip changed values");
  for (const std::size_t width : {1, 15, 17, 31, 47, 49}) {
    argument.binary_value.resize(width);
    Require(!sblr::ProjectionArgumentEncodingValid(argument) &&
                !sblr::ProjectionSblrValueResolved(sblr::SblrValueFromProjectionArgument(argument)),
            "UUID array projection accepted an incomplete element");
  }
  argument.binary_value = expected;
  argument.encoded_value = "text-shadow";
  Require(!sblr::ProjectionArgumentEncodingValid(argument), "UUID array accepted text authority");
  auto array_conflict = value;
  array_conflict.uuid_value = identity;
  std::vector<std::uint8_t> preserved{0xa5};
  Require(!sblr::CopySblrUuidArrayPayload(array_conflict, &preserved) &&
              preserved == std::vector<std::uint8_t>{0xa5}, "UUID array accepted conflicting scalar authority");
  context.current_group_uuid_set.clear();
  Require(value.uuid_array_value.size() == 3, "group result borrowed mutable context storage");
  const auto empty = sblr::ResolveSblrContextVariable("ctx_current_group_uuid_set", context);
  Require(empty.ok() && empty.scalar_values.front().is_null, "empty group set lost null semantics");
  auto conflicting = sblr::MakeSblrUuidValue(identity);
  conflicting.uuid_array_value.push_back(identity);
  std::vector<std::uint8_t> destination{0xa5};
  Require(!sblr::CopySblrUuidPayload(conflicting, &destination) && destination == std::vector<std::uint8_t>{0xa5},
          "scalar UUID accepted a conflicting collection authority");
  const auto timezone = sblr::ResolveSblrContextVariable("CURRENT_TIMEZONE", context);
  Require(timezone.ok() && timezone.scalar_values.front().text_value == "UTC",
          "non-UUID context value changed");
  std::cout << "context binary UUID identity checks passed\n";
}
