// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_timestamp_cast.hpp"
#include "sblr_binary_value_carrier.hpp"
#include "../executor/descriptor_binding_status.hpp"
#include "../internal_api/catalog/column_metadata_codec.hpp"
#include "datatype_timestamp.hpp"

namespace scratchbird::engine::sblr {
namespace {
namespace dt = core::datatypes;
namespace api = internal_api;
bool Character(std::string_view name) {
  return name == "character" || name == "text" || name == "varchar" || name == "char";
}
struct Endpoint {
  const dt::DatatypeTypeCodecIdentityRowV3* identity = nullptr;
  ExecutionTypeDescriptor execution;
  std::shared_ptr<const dt::TimestampValidatedProfileHandleV3> profile;
  dt::TimestampDiagnosticFactV3 failure;
};
bool Bind(const api::EngineDescriptor& binding, bool timestamp, Endpoint& out,
          const Endpoint* reuse = nullptr) {
  const auto type = timestamp ? dt::CanonicalTypeId::timestamp : dt::CanonicalTypeId::character;
  if (timestamp ? binding.canonical_type_name != "timestamp" : !Character(binding.canonical_type_name))
    return false;
  api::CatalogColumnMetadata fields;
  if (!api::AdmitCatalogColumnMetadata(binding.encoded_descriptor, &fields)) return false;
  // Do not discard a domain, timezone or modifier that this adapter cannot carry.
  for (const auto& [key, ignored] : fields.text)
    if (key != "nullability" && key != "nullable" && !(!timestamp && key == "length")) return false;
  for (const auto& [key, ignored] : fields.identities)
    if (timestamp || (key != "charset_uuid" && key != "collation_uuid")) return false;
  if (timestamp && (!binding.charset_uuid.is_nil() || !binding.collation_uuid.is_nil())) return false;
  const auto& cohort = binding.datatype_cohort;
  out.identity = dt::FindDatatypeTypeCodecIdentityV3(
      cohort.catalog_snapshot_uuid, cohort.catalog_generation, cohort.registry_generation,
      binding.datatype_descriptor_uuid, binding.datatype_descriptor_generation);
  if (!out.identity || out.identity->legacy_fields.type_uuid != binding.type_uuid ||
      out.identity->legacy_fields.canonical_binary_type_code != static_cast<std::uint32_t>(type)) return false;
  std::string detail;
  core::platform::Status provider_status;
  if (!executor::BuildBoundExecutionTypeDescriptor(binding, type, &out.execution, &detail, &provider_status)) {
    if (provider_status.code == core::platform::StatusCode::memory_allocation_failed) {
      out.failure.status = provider_status;
      out.failure.diagnostic_code = "RESOURCE.BUDGET_EXCEEDED";
      out.failure.detail = "native temporal descriptor allocation failed";
    }
    return false;
  }
  if (timestamp) {
    if (!dt::ValidateTimestampExecutionDescriptorV3(out.execution, *out.identity).ok()) return false;
    if (reuse && reuse->identity == out.identity && reuse->profile) {
      out.profile = reuse->profile;
      return true;
    }
    const auto& row = out.identity->legacy_fields;
    auto profile = dt::BuildTimestampValidatedProfileHandleV3(
        {row.catalog_snapshot_uuid,row.catalog_snapshot_uuid,row.catalog_generation,row.registry_generation},
        *out.identity);
    if (!profile.ok()) { out.failure = profile.diagnostic; return false; }
    out.profile = std::make_shared<const dt::TimestampValidatedProfileHandleV3>(std::move(profile.profile));
  }
  return true;
}
bool Carrier(const SblrValue& value, bool timestamp) {
  if (value.is_null) return SblrNullPayloadEmpty(value);
  if (!value.uuid_value.is_nil() || !value.uuid_array_value.empty() ||
      value.has_int64_value || value.has_uint64_value || value.has_real64_value ||
      !value.charset_name.empty() || !value.collation_name.empty()) return false;
  if (timestamp)
    return value.descriptor_id == "timestamp" && value.payload_kind == SblrValuePayloadKind::binary &&
        value.text_value.empty() && value.encoded_value.empty() && value.binary_value.size() == 16;
  return Character(value.descriptor_id) && value.payload_kind == SblrValuePayloadKind::text &&
      value.binary_value.empty() && (value.encoded_value.empty() || value.text_value.empty() ||
                                   value.encoded_value == value.text_value);
}
}

SblrResult EvaluateSblrTimestampCast(
    std::string_view operation, const SblrValue& value,
    std::string_view target_name, const SblrExecutionContext& context,
    bool explicit_cast, bool compatibility,
    std::shared_ptr<const internal_api::EngineDescriptor> target) try {
  const auto fail = [&](std::string_view code, std::string_view detail) {
    return MakeSblrFailure(code == "RESOURCE.BUDGET_EXCEEDED" ? SblrStatusCode::resource_exhausted :
                          SblrStatusCode::execution_failed, std::string(operation),
        MakeSblrRefusalDiagnostic(std::string(code), context, std::string(detail)));
  };
  const bool from_timestamp = value.descriptor_id == "timestamp";
  const bool to_timestamp = target_name == "timestamp";
  const bool contextual_null = value.descriptor_id == "null_type" && to_timestamp;
  if ((!from_timestamp && !Character(value.descriptor_id) && !contextual_null) ||
      (!to_timestamp && !Character(target_name)) || compatibility)
    return fail("DATATYPE.CAST_FORBIDDEN", "native civil timestamp cast endpoint is not admitted");
  if (!target || (contextual_null ? value.projection_descriptor != nullptr :
      !value.projection_descriptor))
    return fail("CTI.TEMPORAL.DESCRIPTOR_INVALID", "native temporal cast requires exact endpoints");
  Endpoint source, destination;
  if ((!contextual_null && !Bind(*value.projection_descriptor, from_timestamp, source)) ||
      !Bind(*target, to_timestamp, destination, &source)) {
    const auto& cause = !source.failure.diagnostic_code.empty() ? source.failure : destination.failure;
    if (!cause.diagnostic_code.empty()) return fail(cause.diagnostic_code, cause.detail);
    return fail("CTI.TEMPORAL.DESCRIPTOR_INVALID", "native temporal cast endpoint binding is invalid");
  }
  if (contextual_null ? !SblrNullPayloadEmpty(value) : !Carrier(value, from_timestamp))
    return fail(value.is_null ? "DATATYPE.NULL_STATE.INVALID" : "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                "native temporal cast source carrier is inconsistent");
  if (value.is_null && ((!contextual_null && !source.execution.nullable_allowed) || !destination.execution.nullable_allowed))
    return fail("DATATYPE.NULL_NOT_ADMITTED", "native temporal cast endpoint is nonnullable");

  dt::TimestampOwnedValueV3 native;
  dt::DatatypeOperationValue text;
  dt::TimestampCastRequestV3 request;
  request.context = explicit_cast ? dt::DatatypeCastContext::explicit_cast : dt::DatatypeCastContext::implicit;
  request.target_null_allowed = destination.execution.nullable_allowed;
  if (from_timestamp) {
    const auto decoded = dt::DecodeCanonicalTimestampComponentNoAllocV3(*source.profile,
        value.is_null ? dt::TimestampValueStateV3::sql_null : dt::TimestampValueStateV3::value,
        source.execution.nullable_allowed, value.binary_value);
    if (!decoded.ok()) return fail(decoded.diagnostic.diagnostic_code, decoded.diagnostic.detail);
    native = {source.profile, decoded.value.state, decoded.value.civil_day, decoded.value.nanoseconds_since_midnight};
    request.timestamp_source = &native;
    if (to_timestamp) {
      request.one_based_policy_row = 53; // Frozen civil TIMESTAMP identity row.
      request.timestamp_target = &destination.profile;
    } else {
      request.one_based_policy_row = 50; // Explicit native UTF-8 character render.
      request.scalar_target = dt::CanonicalTypeId::character;
      request.scalar_target_descriptor = destination.execution;
      request.scalar_target_identity = destination.identity;
    }
  } else {
    text.type_id = contextual_null ? dt::CanonicalTypeId::null_type : dt::CanonicalTypeId::character;
    text.descriptor = source.execution;
    text.is_null = value.is_null;
    text.encoded_value = value.encoded_value.empty() ? value.text_value : value.encoded_value;
    request.one_based_policy_row = contextual_null ? 1 : 24; // Contextual NULL or explicit UTF-8 parse.
    request.scalar_source = &text;
    request.scalar_source_identity = source.identity;
    request.timestamp_target = &destination.profile;
    request.timestamp_target_descriptor = &destination.execution;
  }
  const auto result = dt::CastTimestampValueV3(request);
  if (!result.ok()) return fail(result.diagnostic.diagnostic_code, result.diagnostic.detail);
  SblrValue output;
  output.descriptor_id = to_timestamp ? "timestamp" : "character";
  output.projection_descriptor = std::move(target);
  output.is_null = result.produced_timestamp
      ? result.timestamp_value.state == dt::TimestampValueStateV3::sql_null : result.scalar_value.is_null;
  if (!output.is_null) {
    if (to_timestamp) {
      output.payload_kind = SblrValuePayloadKind::binary;
      output.binary_value.resize(dt::kTimestampComponentBytesV3);
      const auto encoded = dt::EncodeCanonicalTimestampComponentIntoNoAllocV3(
          result.timestamp_value, output.binary_value.data(), output.binary_value.size());
      if (!encoded.ok()) return fail(encoded.diagnostic.diagnostic_code, encoded.diagnostic.detail);
    } else {
      output.payload_kind = SblrValuePayloadKind::text;
      output.text_value = output.encoded_value = result.scalar_value.encoded_value;
    }
  }
  auto published = MakeSblrSuccess(std::string(operation));
  published.scalar_values.push_back(std::move(output));
  return published;
} catch (const std::bad_alloc&) {
  return MakeSblrFailure(SblrStatusCode::resource_exhausted, std::string(operation),
      MakeSblrRefusalDiagnostic("RESOURCE.BUDGET_EXCEEDED", context, "native temporal cast allocation failed"));
}
}
