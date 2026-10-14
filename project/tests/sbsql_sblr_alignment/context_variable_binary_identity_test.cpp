// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../support/binary_uuid_fixture.hpp"
#include "sblr_context_variables.hpp"
#include "sblr_projection_value_runtime.hpp"
#include "sblr_special_forms.hpp"
#include "sblr_operator_runtime.hpp"
#include "../../src/engine/functions/common/function_result_helpers.hpp"
#include "datatype_operations.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
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

void RequireUInt64(const sblr::SblrResult& result, std::uint64_t expected) {
  if (!result.ok()) {
    std::cerr << "UINT64 operation=" << result.operation_id << '\n';
    for (const auto& diagnostic : result.diagnostics)
      std::cerr << diagnostic.diagnostic_id << ':' << diagnostic.detail << '\n';
  }
  Require(result.ok() && result.scalar_values.size() == 1, "UINT64 context unavailable");
  const auto& value = result.scalar_values.front();
  Require(sblr::SblrUInt64PayloadValid(value) && value.uint64_value == expected,
          "UINT64 context changed bits or retained signed/text shadows");
  const auto projected = sblr::EngineTypedValueFromSblrValue(value);
  Require(projected.state == scratchbird::engine::internal_api::EngineValueState::value &&
              !projected.is_null && projected.encoded_value.empty() &&
              projected.binary_value.size() == 8 && projected.descriptor.descriptor_uuid.is_nil(),
          "UINT64 context projection lost native width or manufactured binding authority");
  for (unsigned i = 0; i < 8; ++i)
    Require(projected.binary_value[i] == static_cast<std::uint8_t>(expected >> (i * 8)),
            "UINT64 context projection is not canonical LE8");
}

void CheckUInt64ContextsAndProjection() {
  namespace api = scratchbird::engine::internal_api;
  using State = api::EngineValueState;
  sblr::SblrExecutionContext context;
  constexpr std::array transaction_aliases{
      "ctx_current_local_transaction_id", "TRANSACTION_ID", "CURRENT_TRANSACTION_ID",
      "firebird.transaction_id"};
  constexpr std::array row_aliases{
      "ctx_last_row_count", "ROW_COUNT", "ROW_COUNT()", "FOUND_ROWS", "sqlite.changes"};
  std::vector<std::uint64_t> values{0, 1, 0x0102030405060708ull,
      std::uint64_t{std::numeric_limits<std::int64_t>::max()},
      std::uint64_t{std::numeric_limits<std::int64_t>::max()} + 1,
      std::numeric_limits<std::uint64_t>::max()};
  for (unsigned bit = 0; bit < 64; ++bit) values.push_back(std::uint64_t{1} << bit);
  for (unsigned position = 0; position < 8; ++position)
    for (unsigned octet = 0; octet < 256; ++octet) {
      const auto mask = std::uint64_t{0xff} << (8 * position);
      values.push_back((0x0102030405060708ull & ~mask) |
                      (static_cast<std::uint64_t>(octet) << (8 * position)));
    }
  for (const auto expected : values) {
    context.local_transaction_id = expected;
    context.last_row_count = expected;
    context.last_row_count_present = true;
    for (const auto alias : transaction_aliases) {
      const auto result = sblr::ResolveSblrContextVariable(alias, context);
      if (expected) RequireUInt64(result, expected);
      else Require(result.ok() && result.scalar_values.size() == 1 &&
                       sblr::SblrNullPayloadEmpty(result.scalar_values.front()),
                   "absent local transaction ID changed into numeric zero");
    }
    for (const auto alias : row_aliases)
      RequireUInt64(sblr::ResolveSblrContextVariable(alias, context), expected);

    api::EngineProjectionFunctionArgument input;
    input.type_name = "uint64";
    input.state = State::value;
    input.is_null = false;
    // Independent structural sentinel, never a statement/catalog admission.
    input.descriptor.canonical_type_name = "uint64";
    input.descriptor.descriptor_uuid = FixtureUuid(6014, 71);
    input.descriptor.type_uuid = FixtureUuid(6014, 72);
    input.descriptor.datatype_descriptor_uuid = FixtureUuid(6014, 73);
    input.descriptor.datatype_descriptor_generation = 11;
    input.descriptor.encoded_descriptor = "nullability=nullable";
    input.binary_value.resize(8);
    for (unsigned i = 0; i < 8; ++i)
      input.binary_value[i] = static_cast<std::uint8_t>(expected >> (8 * i));
    Require(sblr::ProjectionArgumentEncodingValid(input), "canonical UINT64 argument refused");
    const auto decoded = sblr::SblrValueFromProjectionArgument(input);
    Require(sblr::ProjectionSblrValueResolved(decoded) && sblr::SblrUInt64PayloadValid(decoded) &&
                decoded.uint64_value == expected && decoded.projection_descriptor &&
                *decoded.projection_descriptor == input.descriptor,
            "UINT64 decoding changed native value/binding");
    const auto output = sblr::EngineTypedValueFromSblrValue(decoded);
    Require(output.binary_value == input.binary_value && output.encoded_value.empty() &&
                output.descriptor == input.descriptor && output.state == input.state,
            "UINT64 projection roundtrip changed bits/state/binding");
    std::string core_bytes;
    Require(scratchbird::core::datatypes::EncodeCanonicalUint64Value(expected, &core_bytes),
            "Core UINT64 encoder failed");
    const auto from_core = sblr::SblrValueFromCanonicalUInt64Bytes(core_bytes);
    Require(sblr::SblrUInt64PayloadValid(from_core) && from_core.uint64_value == expected &&
                !from_core.projection_descriptor &&
                sblr::EngineTypedValueFromSblrValue(from_core).binary_value == input.binary_value,
            "Core UINT64 result was parsed as decimal text or acquired admission authority");
    const auto function_value = scratchbird::engine::functions::MakeUint64Value("uint64", expected);
    Require(sblr::SblrUInt64PayloadValid(function_value) &&
                sblr::EngineTypedValueFromSblrValue(function_value).binary_value == input.binary_value,
            "UINT64 function-result constructor retained text or changed value");
    const auto when_true = scratchbird::engine::functions::MakeUint64Value("uint64", 11);
    const auto when_false = scratchbird::engine::functions::MakeUint64Value("uint64", 12);
    const auto branch = sblr::EvaluateSblrIifForm("form_iif", decoded, when_true, when_false);
    Require(branch.ok() && branch.scalar_values.size() == 1 &&
                branch.scalar_values.front().uint64_value == (expected ? 11u : 12u),
            "native UINT64 condition depended on removed signed/text shadow");
    const auto equal = sblr::EvaluateSblrNullIfForm("form_nullif", decoded, decoded);
    Require(equal.ok() && equal.scalar_values.size() == 1 && equal.scalar_values.front().is_null,
            "same-type UINT64 equality changed");
    const auto different = scratchbird::engine::functions::MakeUint64Value("uint64", expected ^ 1);
    const auto unequal = sblr::EvaluateSblrNullIfForm("form_nullif", decoded, different);
    Require(unequal.ok() && unequal.scalar_values.size() == 1 &&
                unequal.scalar_values.front().uint64_value == expected &&
                !unequal.scalar_values.front().is_null,
            "UINT64 comparison selected empty text instead of magnitude");
    const auto mixed = sblr::EvaluateSblrNullIfForm("form_nullif", decoded,
        scratchbird::engine::functions::MakeInt64Value("int64", 3));
    Require(!mixed.ok() && mixed.scalar_values.empty(),
            "mixed signed/unsigned comparison invented missing numeric policy");
    const sblr::SblrValue contextual_null;
    const auto contextual_right = sblr::EvaluateSblrNullIfForm("form_nullif", decoded, contextual_null);
    const auto contextual_left = sblr::EvaluateSblrNullIfForm("form_nullif", contextual_null, decoded);
    Require(contextual_right.ok() && contextual_right.scalar_values.size() == 1 &&
                contextual_right.scalar_values.front().uint64_value == expected &&
                contextual_left.ok() && contextual_left.scalar_values.size() == 1 &&
                contextual_left.scalar_values.front().is_null,
            "clean contextual NULL was refused in UINT64 NULLIF");
    const auto in_null = sblr::EvaluateSblrInListForm("form_in", decoded, {contextual_null});
    const auto in_lazy_null = sblr::EvaluateSblrInListFormLazy("form_in",
        [decoded] { auto r = sblr::MakeSblrSuccess("probe"); r.scalar_values.push_back(decoded); return r; },
        {[contextual_null] { auto r = sblr::MakeSblrSuccess("candidate");
           r.scalar_values.push_back(contextual_null); return r; }}, context);
    const auto between_null = sblr::EvaluateSblrBetweenForm("form_between", decoded, contextual_null, decoded);
    Require(in_null.ok() && in_null.scalar_values.front().is_null &&
                in_lazy_null.ok() && in_lazy_null.scalar_values.front().is_null &&
                between_null.ok() && between_null.scalar_values.front().is_null,
            "UINT64 IN/BETWEEN contextual NULL lost three-valued semantics");
    auto dirty_null = contextual_null;
    dirty_null.encoded_value = "0";
    Require(!sblr::EvaluateSblrNullIfForm("form_nullif", decoded, dirty_null).ok() &&
                !sblr::EvaluateSblrInListForm("form_in", decoded, {dirty_null}).ok(),
            "UINT64 comparison accepted dirty contextual NULL");
    auto foreign_null = contextual_null;
    foreign_null.descriptor_id = "int64";
    Require(!sblr::EvaluateSblrNullIfForm("form_nullif", decoded, foreign_null).ok(),
            "typed foreign NULL invented missing mixed-numeric policy");

    for (unsigned mutation = 0; mutation < 11; ++mutation) {
      auto conflict = decoded;
      switch (mutation) {
        case 0: conflict.text_value = "0"; break;
        case 1: conflict.encoded_value = "0"; break;
        case 2: conflict.binary_value = input.binary_value; break;
        case 3: conflict.has_int64_value = true; break;
        case 4: conflict.has_real64_value = true; break;
        case 5: conflict.uuid_value = FixtureUuid(6014, 74); break;
        case 6: conflict.uuid_array_value.push_back(FixtureUuid(6014, 74)); break;
        case 7: conflict.charset_name = "UTF8"; break;
        case 8: conflict.collation_name = "binary"; break;
        case 9: conflict.payload_kind = sblr::SblrValuePayloadKind::signed_integer; break;
        case 10: conflict.has_uint64_value = false; break;
      }
      Require(!sblr::ProjectionSblrValueResolved(conflict), "conflicting UINT64 was resolved");
      bool refused = false;
      try { (void)sblr::EngineTypedValueFromSblrValue(conflict); }
      catch (const std::invalid_argument&) { refused = true; }
      Require(refused, "UINT64 projection selected one conflicting authority");
    }
    const auto reject = [](const api::EngineProjectionFunctionArgument& malformed) {
      Require(!sblr::ProjectionArgumentEncodingValid(malformed) &&
                  !sblr::ProjectionSblrValueResolved(sblr::SblrValueFromProjectionArgument(malformed)),
              "malformed UINT64 argument admitted or manufactured NULL");
    };
    for (unsigned width = 0; width <= 17; ++width) {
      if (width == 8) continue;
      auto malformed = input;
      malformed.binary_value.resize(width);
      reject(malformed);
      bool core_refused = false;
      try { (void)sblr::SblrValueFromCanonicalUInt64Bytes(std::string(width, '0')); }
      catch (const std::invalid_argument&) { core_refused = true; }
      Require(core_refused, "Core UINT64 adapter accepted incorrect width");
    }
    auto malformed = input;
    malformed.encoded_value = std::to_string(expected);
    reject(malformed);
    malformed.binary_value.clear();
    reject(malformed);
    malformed = input;
    malformed.descriptor.canonical_type_name = "int64";
    reject(malformed);
    for (const auto state : {State::sql_null, State::missing, State::default_requested,
                             State::unknown, State::error, State::lob_handle, State::protected_value,
                             static_cast<State>(0xff)}) {
      malformed = input;
      malformed.state = state;
      reject(malformed);
    }
    malformed = input;
    malformed.is_null = true;
    reject(malformed);

    auto null = input;
    null.state = State::sql_null;
    null.is_null = true;
    null.binary_value.clear();
    Require(sblr::ProjectionArgumentEncodingValid(null), "canonical UINT64 NULL refused");
    const auto null_decoded = sblr::SblrValueFromProjectionArgument(null);
    Require(sblr::ProjectionSblrValueResolved(null_decoded) && sblr::SblrNullPayloadEmpty(null_decoded),
            "UINT64 NULL acquired payload");
    const auto null_output = sblr::EngineTypedValueFromSblrValue(null_decoded);
    Require(null_output.state == State::sql_null && null_output.is_null &&
                null_output.binary_value.empty() && null_output.encoded_value.empty() &&
                null_output.descriptor == input.descriptor,
            "UINT64 NULL projection lost binding/state");
    null.is_null = false;
    reject(null);
    constexpr sblr::SblrUuid decimal_type{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x13}};
    constexpr sblr::SblrUuid decimal_datatype{{0xa0,0,0,0,0x64,0x65,0x73,0x69,0xad,0x61,0x6c,0,0,0,0,0}};
    for (const auto& protected_identity : {sblr::uint16_projection_detail::type,
         sblr::uint16_projection_detail::datatype, decimal_type, decimal_datatype}) {
      for (const bool datatype_field : {false, true}) {
        auto renamed = input;
        (datatype_field ? renamed.descriptor.datatype_descriptor_uuid : renamed.descriptor.type_uuid) =
            protected_identity;
        // Only the owning identity field is a protected type reference.
        if (!sblr::ReferencesDecimalProjection(renamed.descriptor) &&
            !sblr::ReferencesUint16Projection(renamed.descriptor)) continue;
        reject(renamed);
        auto conflict = decoded;
        conflict.projection_descriptor = std::make_shared<const api::EngineDescriptor>(renamed.descriptor);
        Require(!sblr::ProjectionSblrValueResolved(conflict),
                "renamed UINT16/DECIMAL identity escaped UINT64 result validation");
        bool refused = false;
        try { (void)sblr::EngineTypedValueFromSblrValue(conflict); }
        catch (const std::invalid_argument&) { refused = true; }
        Require(refused, "renamed protected identity escaped UINT64 publication");
        renamed.is_null = true;
        renamed.state = State::sql_null;
        renamed.binary_value.clear();
        reject(renamed);
      }
    }
  }
  sblr::SblrValue collection;
  collection.descriptor_id = "array";
  collection.payload_kind = sblr::SblrValuePayloadKind::text;
  collection.is_null = false;
  collection.text_value = collection.encoded_value = "[1,2,3]";
  const auto cardinality = sblr::EvaluateSblrCollectionOperator(
      "op_collection_cardinality", collection, {}, context);
  RequireUInt64(cardinality, 3);
  context.last_row_count_present = false;
  for (const auto alias : row_aliases) {
    const auto absent = sblr::ResolveSblrContextVariable(alias, context);
    Require(absent.ok() && absent.scalar_values.size() == 1 &&
                sblr::SblrNullPayloadEmpty(absent.scalar_values.front()),
            "absent row count was emitted as zero or stale data");
  }
}

int main() {
  CheckUInt64ContextsAndProjection();
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
