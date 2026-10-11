// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_signed_character_cast.hpp"
#include "sblr_binary_value_carrier.hpp"
#include "../executor/descriptor_value_runtime.hpp"
#include <bit>
#include <limits>

namespace scratchbird::engine::sblr {
SblrResult EvaluateSblrSignedCharacterCast(std::string_view operation,
    const SblrValue& value, std::string_view target_name,
    const SblrExecutionContext& context, bool explicit_cast, bool compatibility,
    std::shared_ptr<const internal_api::EngineDescriptor> target) {
  core::datatypes::DatatypeNumericFacts facts;
  try {
  const auto refuse = [&](std::string code, std::string detail) {
    const auto status = code == "RESOURCE.BUDGET_EXCEEDED" || code == "DATATYPE.RESOURCE_EXHAUSTED"
        ? SblrStatusCode::resource_exhausted : SblrStatusCode::execution_failed;
    return MakeSblrFailure(status, std::string(operation),
        MakeSblrRefusalDiagnostic(std::move(code), context, std::move(detail)));
  };
  if (!explicit_cast || compatibility)
    return refuse("DATATYPE.CAST_FORBIDDEN", "native signed CHARACTER conversion requires an explicit cast");
  if (!IsSblrSignedCharacterCast(value.descriptor_id, target_name) ||
      !value.projection_descriptor || !target ||
      value.projection_descriptor->canonical_type_name != value.descriptor_id ||
      target->canonical_type_name != target_name)
    return refuse("DATATYPE.DESCRIPTOR.INVALID", "native signed CHARACTER cast requires both exact endpoint bindings");
  if (value.is_null ? !SblrNullPayloadEmpty(value) :
      (!value.uuid_value.is_nil() || !value.uuid_array_value.empty() ||
       !value.binary_value.empty() || !value.charset_name.empty() ||
       !value.collation_name.empty() || value.has_uint64_value || value.has_real64_value))
    return refuse("NUMERIC.ENCODING.NONCANONICAL", "signed CHARACTER cast has conflicting value carriers");
  const unsigned width = SblrSignedIntegerWidth(value.descriptor_id);
  if (!value.is_null) {
    if (width) {
      if (value.payload_kind != SblrValuePayloadKind::signed_integer || !value.has_int64_value ||
          !value.encoded_value.empty() || !value.text_value.empty())
        return refuse("NUMERIC.ENCODING.NONCANONICAL", "signed source requires a machine integer without text shadows");
      if (width < 8) {
        const auto limit = std::int64_t{1} << (width * 8 - 1);
        if (value.int64_value < -limit || value.int64_value >= limit)
          return refuse("NUMERIC.ENCODING.NONCANONICAL", "signed source exceeds its declared width");
      }
    } else if (value.payload_kind != SblrValuePayloadKind::text || value.has_int64_value ||
               value.encoded_value != value.text_value) {
      return refuse("NUMERIC.ENCODING.NONCANONICAL", "CHARACTER source requires matching exact text carriers");
    }
  }
  internal_api::EngineTypedValue input;
  input.descriptor = *value.projection_descriptor;
  input.setState(value.is_null ? internal_api::EngineValueState::sql_null : internal_api::EngineValueState::value);
  if (!value.is_null) {
    if (width) {
      const auto bits = static_cast<std::uint64_t>(value.int64_value);
      input.binary_value.resize(width);
      for (unsigned i = 0; i < width; ++i) input.binary_value[i] = static_cast<std::uint8_t>(bits >> (8 * i));
    } else input.encoded_value = value.text_value;
  }
  executor::DescriptorRuntimeDiagnostic diagnostic;
  const auto converted = executor::CastDescriptorValue(input, *target, &diagnostic);
  facts = diagnostic.numeric_facts;
  if (!diagnostic.ok) {
    auto failed = refuse(diagnostic.diagnostic_code, diagnostic.detail);
    failed.numeric_facts = diagnostic.numeric_facts;
    return failed;
  }
  SblrValue published;
  published.descriptor_id = target_name;
  published.projection_descriptor = std::move(target);
  published.is_null = converted.isSqlNull();
  if (!published.is_null) {
    const unsigned result_width = SblrSignedIntegerWidth(target_name);
    if (result_width) {
      if (converted.binary_value.size() != result_width || !converted.encoded_value.empty())
        return refuse("NUMERIC.ENCODING.NONCANONICAL", "signed cast owner returned an invalid native width");
      std::uint64_t bits = 0;
      for (unsigned i = 0; i < result_width; ++i)
        bits |= static_cast<std::uint64_t>(converted.binary_value[i]) << (8 * i);
      if (result_width < 8 && (converted.binary_value.back() & 0x80))
        bits |= std::numeric_limits<std::uint64_t>::max() << (8 * result_width);
      published.int64_value = std::bit_cast<std::int64_t>(bits);
      published.has_int64_value = true;
      published.payload_kind = SblrValuePayloadKind::signed_integer;
    } else {
      published.encoded_value = converted.encoded_value;
      published.text_value = converted.encoded_value;
      published.payload_kind = SblrValuePayloadKind::text;
    }
  }
  auto result = MakeSblrSuccess(std::string(operation));
  result.scalar_values.push_back(std::move(published));
  result.numeric_facts = diagnostic.numeric_facts;
  return result;
} catch (const std::bad_alloc&) {
  auto failed = MakeSblrFailure(SblrStatusCode::resource_exhausted, std::string(operation),
      MakeSblrRefusalDiagnostic("DATATYPE.RESOURCE_EXHAUSTED", context,
                               "signed CHARACTER cast allocation failed"));
  failed.numeric_facts = facts;
  return failed;
  }
}
}
