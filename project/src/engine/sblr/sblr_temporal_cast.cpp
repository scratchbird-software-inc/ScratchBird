// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_temporal_cast.hpp"
#include "sblr_binary_value_carrier.hpp"
#include "../executor/descriptor_binding_status.hpp"
#include "../internal_api/catalog/column_metadata_codec.hpp"
#include "datatype_timestamp.hpp"
#include "datatype_date.hpp"
#include "datatype_time.hpp"
#include "canonical_utf8.hpp"
#include <algorithm>
#include <type_traits>
#include <limits>

namespace scratchbird::engine::sblr {
namespace {
namespace dt = core::datatypes;
namespace api = internal_api;
bool Character(std::string_view name) {
  return name == "character" || name == "text" || name == "varchar" || name == "char";
}

struct DateTraits {
  using Profile = dt::DateValidatedProfileHandleV3;
  using Value = dt::DateOwnedValueV3;
  using State = dt::DateValueStateV3;
  using Request = dt::DateCastRequestV3;
  using Diagnostic = dt::DateDiagnosticFactV3;
  static constexpr std::string_view name = "date";
  static constexpr auto type = dt::CanonicalTypeId::date;
  static constexpr std::size_t bytes = 4;
  static auto Validate(const ExecutionTypeDescriptor& d, const dt::DatatypeTypeCodecIdentityRowV3& i) {
    return dt::ValidateDateExecutionDescriptorV3(d, i);
  }
  static auto Build(const dt::DatatypeTypeCodecIdentityRowV3& i) {
    const auto& r = i.legacy_fields;
    return dt::BuildDateValidatedProfileHandleV3(
        {r.catalog_snapshot_uuid,r.catalog_snapshot_uuid,r.catalog_generation,r.registry_generation}, i);
  }
  static auto Decode(const Profile& p, State state, bool nullable, const auto& bytes) {
    return dt::DecodeCanonicalDateComponentNoAllocV3(p, state, nullable, bytes);
  }
  static Value Own(const std::shared_ptr<const Profile>& p, const auto& v) {
    return {p, v.state, v.day};
  }
  static void Source(Request& r, const Value& v) { r.date_source = &v; }
  static void Target(Request& r, const std::shared_ptr<const Profile>& p,
                     const ExecutionTypeDescriptor& d, bool incoming) {
    r.date_target = &p;
    if (incoming) r.date_target_descriptor = &d;
  }
  static auto Cast(const Request& r) { return dt::CastDateValueV3(r); }
  static bool HasNative(const auto& r) { return r.produced_date; }
  static const Value& Native(const auto& r) { return r.date_value; }
  static auto Encode(const Value& v, auto& bytes) {
    return dt::EncodeCanonicalDateComponentIntoNoAllocV3(v, bytes.data(), bytes.size());
  }
};

struct TimeTraits {
  using Profile = dt::TimeValidatedProfileHandleV3;
  using Value = dt::TimeOwnedValueV3;
  using State = dt::TimeValueStateV3;
  using Request = dt::TimeCastRequestV3;
  using Diagnostic = dt::TimeDiagnosticFactV3;
  static constexpr std::string_view name = "time";
  static constexpr auto type = dt::CanonicalTypeId::time;
  static constexpr std::size_t bytes = 8;
  static auto Validate(const ExecutionTypeDescriptor& d, const dt::DatatypeTypeCodecIdentityRowV3& i) {
    return dt::ValidateTimeExecutionDescriptorV3(d, i);
  }
  static auto Build(const dt::DatatypeTypeCodecIdentityRowV3& i) {
    const auto& r = i.legacy_fields;
    return dt::BuildTimeValidatedProfileHandleV3(
        {r.catalog_snapshot_uuid,r.catalog_snapshot_uuid,r.catalog_generation,r.registry_generation}, i);
  }
  static auto Decode(const Profile& p, State state, bool nullable, const auto& bytes) {
    return dt::DecodeCanonicalTimeComponentNoAllocV3(p, state, nullable, bytes);
  }
  static Value Own(const std::shared_ptr<const Profile>& p, const auto& v) {
    return {p, v.state, v.nanoseconds_since_midnight};
  }
  static void Source(Request& r, const Value& v) { r.time_source = &v; }
  static void Target(Request& r, const std::shared_ptr<const Profile>& p,
                     const ExecutionTypeDescriptor& d, bool incoming) {
    r.time_target = &p;
    (void)incoming;
    r.time_target_descriptor = &d;
  }
  static auto Cast(const Request& r) { return dt::CastTimeValueV3(r); }
  static bool HasNative(const auto& r) { return r.produced_time; }
  static const Value& Native(const auto& r) { return r.time_value; }
  static auto Encode(const Value& v, auto& bytes) {
    return dt::EncodeCanonicalTimeComponentIntoNoAllocV3(v, bytes.data(), bytes.size());
  }
};

struct TimestampTraits {
  using Profile = dt::TimestampValidatedProfileHandleV3;
  using Value = dt::TimestampOwnedValueV3;
  using State = dt::TimestampValueStateV3;
  using Request = dt::TimestampCastRequestV3;
  using Diagnostic = dt::TimestampDiagnosticFactV3;
  static constexpr std::string_view name = "timestamp";
  static constexpr auto type = dt::CanonicalTypeId::timestamp;
  static constexpr std::size_t bytes = 16;
  static auto Validate(const ExecutionTypeDescriptor& d, const dt::DatatypeTypeCodecIdentityRowV3& i) {
    return dt::ValidateTimestampExecutionDescriptorV3(d, i);
  }
  static auto Build(const dt::DatatypeTypeCodecIdentityRowV3& i) {
    const auto& r = i.legacy_fields;
    return dt::BuildTimestampValidatedProfileHandleV3(
        {r.catalog_snapshot_uuid,r.catalog_snapshot_uuid,r.catalog_generation,r.registry_generation}, i);
  }
  static auto Decode(const Profile& p, State state, bool nullable, const auto& bytes) {
    return dt::DecodeCanonicalTimestampComponentNoAllocV3(p, state, nullable, bytes);
  }
  static Value Own(const std::shared_ptr<const Profile>& p, const auto& v) {
    return {p, v.state, v.civil_day, v.nanoseconds_since_midnight};
  }
  static void Source(Request& r, const Value& v) { r.timestamp_source = &v; }
  static void Target(Request& r, const std::shared_ptr<const Profile>& p,
                     const ExecutionTypeDescriptor& d, bool incoming) {
    r.timestamp_target = &p;
    if (incoming) r.timestamp_target_descriptor = &d;
  }
  static auto Cast(const Request& r) { return dt::CastTimestampValueV3(r); }
  static bool HasNative(const auto& r) { return r.produced_timestamp; }
  static const Value& Native(const auto& r) { return r.timestamp_value; }
  static auto Encode(const Value& v, auto& bytes) {
    return dt::EncodeCanonicalTimestampComponentIntoNoAllocV3(v, bytes.data(), bytes.size());
  }
};

template<class Traits>
struct Endpoint {
  const dt::DatatypeTypeCodecIdentityRowV3* identity = nullptr;
  ExecutionTypeDescriptor execution;
  std::shared_ptr<const typename Traits::Profile> profile;
  typename Traits::Diagnostic failure;
};
template<class Traits>
bool Bind(const api::EngineDescriptor& binding, bool native, Endpoint<Traits>& out,
          const Endpoint<Traits>* reuse = nullptr) {
  const auto type = native ? Traits::type : dt::CanonicalTypeId::character;
  if (native ? binding.canonical_type_name != Traits::name : !Character(binding.canonical_type_name))
    return false;
  api::CatalogColumnMetadata fields;
  if (!api::AdmitCatalogColumnMetadata(binding.encoded_descriptor, &fields)) return false;
  // Do not discard a domain, timezone or modifier that this adapter cannot carry.
  for (const auto& [key, ignored] : fields.text)
    if (key != "nullability" && key != "nullable" && !(!native && key == "length")) return false;
  for (const auto& [key, ignored] : fields.identities)
    if (native || (key != "charset_uuid" && key != "collation_uuid")) return false;
  if (native && (!binding.charset_uuid.is_nil() || !binding.collation_uuid.is_nil())) return false;
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
  if (native) {
    if (!Traits::Validate(out.execution, *out.identity).ok()) return false;
    if (reuse && reuse->identity == out.identity && reuse->profile) {
      out.profile = reuse->profile;
      return true;
    }
    auto profile = Traits::Build(*out.identity);
    if (!profile.ok()) { out.failure = profile.diagnostic; return false; }
    out.profile = std::make_shared<const typename Traits::Profile>(std::move(profile.profile));
  }
  return true;
}
template<class Traits>
bool Carrier(const SblrValue& value, bool native) {
  if (value.is_null) return SblrNullPayloadEmpty(value);
  if (!value.uuid_value.is_nil() || !value.uuid_array_value.empty() ||
      value.has_int64_value || value.has_uint64_value || value.has_real64_value ||
      !value.charset_name.empty() || !value.collation_name.empty()) return false;
  if (native)
    return value.descriptor_id == Traits::name && value.payload_kind == SblrValuePayloadKind::binary &&
        value.text_value.empty() && value.encoded_value.empty() && value.binary_value.size() == Traits::bytes;
  return Character(value.descriptor_id) && value.payload_kind == SblrValuePayloadKind::text &&
      value.binary_value.empty() && (value.encoded_value.empty() || value.text_value.empty() ||
                                   value.encoded_value == value.text_value);
}


template<class Traits>
SblrResult Cast(
    std::string_view operation, const SblrValue& value,
    std::string_view target_name, const SblrExecutionContext& context,
    bool explicit_cast, bool compatibility,
    std::shared_ptr<const internal_api::EngineDescriptor> target,
    bool try_conversion) try {
  const auto fail = [&](std::string_view code, std::string_view detail) {
    return MakeSblrFailure(code == "RESOURCE.BUDGET_EXCEEDED" ? SblrStatusCode::resource_exhausted :
                          SblrStatusCode::execution_failed, std::string(operation),
        MakeSblrRefusalDiagnostic(std::string(code), context, std::string(detail)));
  };
  const bool from_native = value.descriptor_id == Traits::name;
  const bool to_native = target_name == Traits::name;
  const bool contextual_null = value.descriptor_id == "null_type" && to_native;
  if ((!from_native && !to_native) ||
      (!from_native && !Character(value.descriptor_id) && !contextual_null) ||
      (!to_native && !Character(target_name)) || compatibility)
    return fail("DATATYPE.CAST_FORBIDDEN", "native civil temporal cast endpoint is not admitted");
  if (!target || (contextual_null ? value.projection_descriptor != nullptr :
      !value.projection_descriptor))
    return fail("CTI.TEMPORAL.DESCRIPTOR_INVALID", "native temporal cast requires exact endpoints");
  Endpoint<Traits> source, destination;
  if ((!contextual_null && !Bind(*value.projection_descriptor, from_native, source)) ||
      !Bind(*target, to_native, destination, &source)) {
    const auto& cause = !source.failure.diagnostic_code.empty() ? source.failure : destination.failure;
    if (!cause.diagnostic_code.empty()) return fail(cause.diagnostic_code, cause.detail);
    return fail("CTI.TEMPORAL.DESCRIPTOR_INVALID", "native temporal cast endpoint binding is invalid");
  }
  if (contextual_null ? !SblrNullPayloadEmpty(value) : !Carrier<Traits>(value, from_native))
    return fail(value.is_null ? "DATATYPE.NULL_STATE.INVALID" : "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                "native temporal cast source carrier is inconsistent");
  if (value.is_null && ((!contextual_null && !source.execution.nullable_allowed) || !destination.execution.nullable_allowed))
    return fail("DATATYPE.NULL_NOT_ADMITTED", "native temporal cast endpoint is nonnullable");

  typename Traits::Value native;
  dt::DatatypeOperationValue text;
  typename Traits::Request request;
  request.context = explicit_cast ? dt::DatatypeCastContext::explicit_cast : dt::DatatypeCastContext::implicit;
  request.target_null_allowed = destination.execution.nullable_allowed;
  if (from_native) {
    const auto decoded = Traits::Decode(*source.profile,
        value.is_null ? Traits::State::sql_null : Traits::State::value,
        source.execution.nullable_allowed, value.binary_value);
    if (!decoded.ok()) return fail(decoded.diagnostic.diagnostic_code, decoded.diagnostic.detail);
    native = Traits::Own(source.profile, decoded.value);
    Traits::Source(request, native);
    if (to_native) {
      request.one_based_policy_row = 53; // Frozen same-family identity row.
      Traits::Target(request, destination.profile, destination.execution, false);
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
    Traits::Target(request, destination.profile, destination.execution, true);
  }
  const auto result = Traits::Cast(request);
  bool conversion_null = false;
  if (!result.ok()) {
    // Eligibility is owned here, after exact binding, native decoding and Core
    // cast-policy admission. Never infer it from an arbitrary callback error.
    const auto& code = result.diagnostic.diagnostic_code;
    if (try_conversion && explicit_cast && !value.is_null) {
      if (from_native && !to_native) {
        conversion_null = code == "CTB.TEXT.LENGTH_EXCEEDED";
      } else if (!from_native && to_native &&
          (code == "CTI.TEMPORAL.INVALID_LITERAL" ||
           code == "CTI.TEMPORAL.ZERO_DATE_REFUSED" ||
           code == "CTI.TEMPORAL.LEAP_SECOND_REFUSED" ||
           code == "CTI.TEMPORAL.PRECISION_LOSS" ||
           code == "CTI.TEMPORAL.RANGE_EXCEEDED")) {
        std::uint64_t characters = 0;
        conversion_null = text.encoded_value.size() <=
                source.identity->legacy_fields.canonical_value_maximum_bytes &&
            dt::ValidateCanonicalUtf8(
                reinterpret_cast<const std::uint8_t*>(text.encoded_value.data()),
                text.encoded_value.size(), &characters) &&
            (source.execution.length == 0 || characters <= source.execution.length);
      }
    }
    if (!conversion_null) return fail(code, result.diagnostic.detail);
    if (!destination.execution.nullable_allowed)
      return fail("DATATYPE.NULL_NOT_ADMITTED", "TRY_CAST target is nonnullable");
  }
  SblrValue output;
  output.descriptor_id = to_native ? std::string(Traits::name) : "character";
  output.projection_descriptor = std::move(target);
  output.is_null = conversion_null || (Traits::HasNative(result)
      ? Traits::Native(result).state == Traits::State::sql_null : result.scalar_value.is_null);
  if (!output.is_null) {
    if (to_native) {
      output.payload_kind = SblrValuePayloadKind::binary;
      output.binary_value.resize(Traits::bytes);
      const auto encoded = Traits::Encode(Traits::Native(result), output.binary_value);
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
bool BindInt64(const api::EngineDescriptor& binding, ExecutionTypeDescriptor& out,
               std::string& detail, core::platform::Status& status) {
  if (binding.canonical_type_name != "int64" || !binding.charset_uuid.is_nil() ||
      !binding.collation_uuid.is_nil()) return false;
  api::CatalogColumnMetadata fields;
  if (!api::AdmitCatalogColumnMetadata(binding.encoded_descriptor, &fields) || !fields.identities.empty())
    return false;
  for (const auto& [key, ignored] : fields.text)
    if (key != "nullability" && key != "nullable") return false;
  return executor::BuildBoundExecutionTypeDescriptor(binding, dt::CanonicalTypeId::int64,
                                                     &out, &detail, &status);
}

template<class Traits>
SblrResult Calendar(std::string_view operation, SblrCalendarField field,
                    const SblrValue& value, const SblrExecutionContext& context,
                    std::shared_ptr<const api::EngineDescriptor> target) try {
  const auto fail = [&](std::string_view code, std::string_view detail) {
    return MakeSblrFailure(code == "RESOURCE.BUDGET_EXCEEDED" ? SblrStatusCode::resource_exhausted :
                          SblrStatusCode::execution_failed, std::string(operation),
        MakeSblrRefusalDiagnostic(std::string(code), context, std::string(detail)));
  };
  if (value.descriptor_id != Traits::name || !value.projection_descriptor || !target ||
      target->canonical_type_name != "int64" || !target->charset_uuid.is_nil() ||
      !target->collation_uuid.is_nil() ||
      target->datatype_cohort != value.projection_descriptor->datatype_cohort)
    return fail("CTI.TEMPORAL.DESCRIPTOR_INVALID", "calendar requires native temporal and INT64 bindings");
  Endpoint<Traits> source;
  if (!Bind(*value.projection_descriptor, true, source))
    return fail(source.failure.diagnostic_code.empty() ? "CTI.TEMPORAL.DESCRIPTOR_INVALID" :
                source.failure.diagnostic_code, "invalid calendar source binding");
  ExecutionTypeDescriptor destination;
  std::string detail;
  core::platform::Status provider_status;
  if (!BindInt64(*target, destination, detail, provider_status))
    return fail(provider_status.code == core::platform::StatusCode::memory_allocation_failed ?
                "RESOURCE.BUDGET_EXCEEDED" : "CTI.TEMPORAL.DESCRIPTOR_INVALID", detail);
  if (!Carrier<Traits>(value, true))
    return fail(value.is_null ? "DATATYPE.NULL_STATE.INVALID" : "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                "invalid calendar source carrier");
  const auto decoded = Traits::Decode(*source.profile,
      value.is_null ? Traits::State::sql_null : Traits::State::value,
      source.execution.nullable_allowed, value.binary_value);
  if (!decoded.ok()) return fail(decoded.diagnostic.diagnostic_code, decoded.diagnostic.detail);
  const auto native = Traits::Own(source.profile, decoded.value);
  if (value.is_null && !destination.nullable_allowed)
    return fail("DATATYPE.NULL_NOT_ADMITTED", "calendar result is nonnullable");
  if (field != SblrCalendarField::dow && field != SblrCalendarField::doy &&
      field != SblrCalendarField::quarter && field != SblrCalendarField::isodow &&
      field != SblrCalendarField::week)
    return fail("CTI.TEMPORAL.OPERATION_REFUSED", "unknown calendar field");
  std::int64_t number = 0;
  bool is_null = false;
  if (field == SblrCalendarField::week) {
    const auto fact = [&] {
      if constexpr (std::is_same_v<Traits, DateTraits>)
        return dt::DateIsoWeekV3(native.view(), destination.nullable_allowed);
      else return dt::TimestampIsoWeekV3(native.view(), destination.nullable_allowed);
    }();
    if (!fact.ok()) return fail(fact.diagnostic.diagnostic_code, fact.diagnostic.detail);
    is_null = fact.is_null;
    if constexpr (std::is_same_v<Traits, DateTraits>) number = fact.iso_week;
    else number = fact.value.iso_week;
  } else {
    const auto fact = [&] {
      if constexpr (std::is_same_v<Traits, DateTraits>) {
        if (field == SblrCalendarField::doy) return dt::DateDayOfYearV3(native.view(), destination.nullable_allowed);
        if (field == SblrCalendarField::quarter) return dt::DateQuarterV3(native.view(), destination.nullable_allowed);
        return dt::DateIsoWeekdayV3(native.view(), destination.nullable_allowed);
      } else {
        if (field == SblrCalendarField::doy) return dt::TimestampDayOfYearV3(native.view(), destination.nullable_allowed);
        if (field == SblrCalendarField::quarter) return dt::TimestampQuarterV3(native.view(), destination.nullable_allowed);
        return dt::TimestampIsoWeekdayV3(native.view(), destination.nullable_allowed);
      }
    }();
    if (!fact.ok()) return fail(fact.diagnostic.diagnostic_code, fact.diagnostic.detail);
    is_null = fact.is_null;
    number = fact.signed_value;
    if (field == SblrCalendarField::dow && !is_null) number %= 7;
  }
  SblrValue output;
  output.descriptor_id = "int64";
  output.projection_descriptor = std::move(target);
  output.is_null = is_null;
  if (!is_null) {
    output.payload_kind = SblrValuePayloadKind::signed_integer;
    output.has_int64_value = true;
    output.int64_value = number;
  }
  auto published = MakeSblrSuccess(std::string(operation));
  published.scalar_values.push_back(std::move(output));
  return published;
} catch (const std::bad_alloc&) {
  return MakeSblrFailure(SblrStatusCode::resource_exhausted, std::string(operation),
      MakeSblrRefusalDiagnostic("RESOURCE.BUDGET_EXCEEDED", context, "native calendar allocation failed"));
}
} // namespace

SblrResult EvaluateSblrDateMonth(
    std::string_view operation, const SblrValue& value, const SblrValue* months,
    const SblrExecutionContext& context, std::shared_ptr<const api::EngineDescriptor> target) try {
  const auto fail = [&](std::string_view code, std::string_view detail) {
    return MakeSblrFailure(code == "RESOURCE.BUDGET_EXCEEDED" ? SblrStatusCode::resource_exhausted :
                          SblrStatusCode::execution_failed, std::string(operation),
        MakeSblrRefusalDiagnostic(std::string(code), context, std::string(detail)));
  };
  if (value.descriptor_id != "date" || !value.projection_descriptor || !target ||
      target->datatype_cohort != value.projection_descriptor->datatype_cohort)
    return fail("CTI.TEMPORAL.DESCRIPTOR_INVALID", "date month operation requires exact DATE endpoints");
  Endpoint<DateTraits> source, destination;
  if (!Bind(*value.projection_descriptor, true, source) || !Bind(*target, true, destination, &source)) {
    const auto& cause = source.failure.diagnostic_code.empty() ? destination.failure : source.failure;
    return fail(cause.diagnostic_code.empty() ? "CTI.TEMPORAL.DESCRIPTOR_INVALID" : cause.diagnostic_code,
                "invalid date month endpoint binding");
  }
  ExecutionTypeDescriptor offset_binding;
  if (months) {
    if (months->descriptor_id != "int64" || !months->projection_descriptor ||
        months->projection_descriptor->datatype_cohort != target->datatype_cohort)
      return fail("CTI.TEMPORAL.DESCRIPTOR_INVALID", "month count requires same-cohort INT64 binding");
    std::string detail;
    core::platform::Status status;
    if (!BindInt64(*months->projection_descriptor, offset_binding, detail, status))
      return fail(status.code == core::platform::StatusCode::memory_allocation_failed ?
                  "RESOURCE.BUDGET_EXCEEDED" : "CTI.TEMPORAL.DESCRIPTOR_INVALID", detail);
  }
  if (!Carrier<DateTraits>(value, true))
    return fail(value.is_null ? "DATATYPE.NULL_STATE.INVALID" : "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                "invalid date month source carrier");
  if (months && (months->is_null ? !SblrNullPayloadEmpty(*months) : !SblrInt64PayloadValid(*months)))
    return fail(months->is_null ? "DATATYPE.NULL_STATE.INVALID" : "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                "invalid month count carrier");
  const auto decoded = DateTraits::Decode(*source.profile,
      value.is_null ? DateTraits::State::sql_null : DateTraits::State::value,
      source.execution.nullable_allowed, value.binary_value);
  if (!decoded.ok()) return fail(decoded.diagnostic.diagnostic_code, decoded.diagnostic.detail);
  const bool is_null = value.is_null || (months && months->is_null);
  if ((months && months->is_null && !offset_binding.nullable_allowed) ||
      (is_null && !destination.execution.nullable_allowed))
    return fail("DATATYPE.NULL_NOT_ADMITTED", "date month operation has a nonnullable NULL occurrence");
  SblrValue output;
  output.descriptor_id = "date";
  output.projection_descriptor = std::move(target);
  output.is_null = is_null;
  if (!is_null) {
    const auto native = DateTraits::Own(source.profile, decoded.value);
    const auto civil = dt::DecomposeDateCivilV3(native.view(), false);
    if (!civil.ok()) return fail(civil.diagnostic.diagnostic_code, civil.diagnostic.detail);
    std::int64_t year = civil.civil.year;
    std::int64_t month = civil.civil.month;
    if (months) {
      const std::int64_t initial = year * 12 + month - 1;
      const auto delta = months->int64_value;
      if ((delta > 0 && initial > std::numeric_limits<std::int64_t>::max() - delta) ||
          (delta < 0 && initial < std::numeric_limits<std::int64_t>::min() - delta))
        return fail("CTI.TEMPORAL.RANGE_EXCEEDED", "month count overflow");
      const auto total = initial + delta;
      year = total / 12;
      month = total % 12;
      if (month < 0) { --year; month += 12; }
      ++month;
    }
    constexpr unsigned month_days[]{31,28,31,30,31,30,31,31,30,31,30,31};
    const auto last = month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)
        ? 29u : month_days[month - 1];
    const auto day = months ? std::min<unsigned>(civil.civil.day, last) : last;
    const auto changed = dt::ConstructDateFromCivilV3(destination.profile, year, month, day,
                                                     destination.execution.nullable_allowed);
    if (!changed.ok()) return fail(changed.diagnostic.diagnostic_code, changed.diagnostic.detail);
    output.payload_kind = SblrValuePayloadKind::binary;
    output.binary_value.resize(4);
    const auto encoded = DateTraits::Encode(changed.value, output.binary_value);
    if (!encoded.ok()) return fail(encoded.diagnostic.diagnostic_code, encoded.diagnostic.detail);
  }
  auto result = MakeSblrSuccess(std::string(operation));
  result.scalar_values.push_back(std::move(output));
  return result;
} catch (const std::bad_alloc&) {
  return MakeSblrFailure(SblrStatusCode::resource_exhausted, std::string(operation),
      MakeSblrRefusalDiagnostic("RESOURCE.BUDGET_EXCEEDED", context, "date month operation allocation failed"));
}

SblrResult EvaluateSblrTemporalCalendar(
    std::string_view operation, SblrCalendarField field, const SblrValue& value,
    const SblrExecutionContext& context, std::shared_ptr<const api::EngineDescriptor> target) {
  if (value.descriptor_id == "date") return Calendar<DateTraits>(operation, field, value, context, std::move(target));
  return Calendar<TimestampTraits>(operation, field, value, context, std::move(target));
}

SblrResult EvaluateSblrTemporalCast(
    std::string_view operation, const SblrValue& value,
    std::string_view target_name, const SblrExecutionContext& context,
    bool explicit_cast, bool compatibility,
    std::shared_ptr<const internal_api::EngineDescriptor> target,
    bool try_conversion) {
  const auto family = IsSblrCivilTemporalType(value.descriptor_id)
      ? std::string_view(value.descriptor_id) : target_name;
  if (family == "date") return Cast<DateTraits>(operation, value, target_name, context, explicit_cast, compatibility, std::move(target), try_conversion);
  if (family == "time") return Cast<TimeTraits>(operation, value, target_name, context, explicit_cast, compatibility, std::move(target), try_conversion);
  return Cast<TimestampTraits>(operation, value, target_name, context, explicit_cast, compatibility, std::move(target), try_conversion);
}
}
