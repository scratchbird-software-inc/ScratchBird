// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "datatype_interval.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>

#if defined(SB_PUBLIC_RELEASE_ASAN_UBSAN_PROFILE)
#include <sanitizer/asan_interface.h>
#endif

namespace dt = scratchbird::core::datatypes;
namespace p = scratchbird::core::platform;

__extension__ typedef __int128 WideInt;

namespace {

unsigned checks;
unsigned interval_dispatch_calls;

constexpr p::u64 kSeed = 0x5342494e54455256ull;
constexpr std::string_view kMapping = "base.interval.property.splitmix64.v1";
constexpr p::u64 kMalformedSeed = 0x5342494e46415a5aull;
constexpr p::u64 kMalformedOrdinal = 731;
constexpr std::string_view kMalformedMapping =
    "base_interval_malformed_fuzz_mapping_v1";

void Check(bool value, std::string_view message) {
  ++checks;
  if (!value) {
    std::cerr << "FAIL " << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void Property(bool value, std::string_view message, p::u64 ordinal) {
  ++checks;
  if (!value) {
    std::cerr << "FAIL " << message << " mapping=" << kMapping
              << " seed=0x" << std::hex << kSeed << std::dec
              << " ordinal=" << ordinal << '\n';
    std::exit(EXIT_FAILURE);
  }
}

p::u64 Next(p::u64& state) {
  state += 0x9e3779b97f4a7c15ull;
  p::u64 value = state;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

p::Uuid D710() {
  return p::Uuid(std::array<p::byte, 16>{
      1, 0x9d, 0, 0, 0, 0, 0x70, 0, 0x80, 0, 0, 0, 0, 0, 0xd7, 0x10});
}

std::shared_ptr<const dt::IntervalValidatedProfileHandleV3> Profile() {
  auto result = dt::BuildCurrentIntervalValidatedProfileHandleV3(D710());
  Check(result.ok(), "build current interval profile");
  return std::make_shared<const dt::IntervalValidatedProfileHandleV3>(
      std::move(result.profile));
}

const dt::DatatypeTypeCodecIdentityRowV3* Identity(dt::CanonicalTypeId type) {
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    if (row.legacy_fields.canonical_binary_type_code ==
            static_cast<p::u32>(type) &&
        row.legacy_fields.catalog_snapshot_uuid == D710() &&
        row.legacy_fields.catalog_generation == 10 &&
        row.legacy_fields.registry_generation == 10) {
      return &row;
    }
  }
  return nullptr;
}

scratchbird::engine::ExecutionTypeDescriptor Descriptor(
    dt::CanonicalTypeId type) {
  auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Check(manifest.ok(), "load current catalog manifest");
  auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type);
  Check(row.ok() && row.manifest.descriptor_rows.size() == 1,
        "locate exact catalog descriptor row");
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch = row.manifest.descriptor_rows.front().descriptor_epoch;
  auto descriptor = dt::LookupExecutionTypeDescriptorFromCatalog(type, metadata);
  Check(descriptor.ok(), "bind execution descriptor");
  return descriptor.descriptor;
}

dt::IntervalOwnedValueV3 Value(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile,
    std::int32_t months,
    std::int32_t days,
    std::int64_t nanoseconds) {
  auto result = dt::ConstructIntervalV3(profile, months, days, nanoseconds);
  Check(result.ok(), "construct interval fixture");
  return std::move(result.value);
}

bool DefaultIntervalValue(const dt::IntervalOwnedValueV3& value) {
  return !value.profile && value.state == dt::IntervalValueStateV3::value &&
         value.months == 0 && value.civil_days == 0 &&
         value.fixed_nanoseconds == 0;
}

bool DefaultScalarValue(const dt::DatatypeOperationValue& value) {
  return value.type_id == dt::CanonicalTypeId::unknown &&
         value.encoded_value.empty() && !value.is_null;
}

bool AtomicCastFailure(const dt::IntervalCastResultV3& result) {
  return !result.ok() &&
         result.category == dt::DatatypeCastCategory::forbidden &&
         !result.produced_interval && !result.used_character_output_buffer &&
         result.bytes_required == 0 && result.bytes_written == 0 &&
         DefaultIntervalValue(result.interval_value) &&
         DefaultScalarValue(result.scalar_value);
}

bool UnpublishedCastPayload(const dt::IntervalCastResultV3& result) {
  return !result.ok() && !result.produced_interval &&
         !result.used_character_output_buffer && result.bytes_written == 0 &&
         DefaultIntervalValue(result.interval_value) &&
         DefaultScalarValue(result.scalar_value);
}

dt::IntervalCastPolicyDispositionV3 ExpectedDisposition(
    p::u32 row,
    dt::DatatypeCastContext context) {
  if (row == 158) {
    return dt::IntervalCastPolicyDispositionV3::contextual_null;
  }
  if (row == 39 || row == 100 || row == 210) {
    return dt::IntervalCastPolicyDispositionV3::identity;
  }
  if (row == 124 && context == dt::DatatypeCastContext::explicit_cast) {
    return dt::IntervalCastPolicyDispositionV3::explicit_character_to_interval;
  }
  if (row == 13 && context == dt::DatatypeCastContext::explicit_cast) {
    return dt::IntervalCastPolicyDispositionV3::explicit_interval_to_character;
  }
  return dt::IntervalCastPolicyDispositionV3::forbidden;
}

void CastCorpus(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile) {
  const auto source = Value(profile, -7, 8, -9);
  const auto character_descriptor = Descriptor(dt::CanonicalTypeId::character);
  const auto interval_descriptor = Descriptor(dt::CanonicalTypeId::interval);
  const auto* character_identity = Identity(dt::CanonicalTypeId::character);
  Check(character_identity != nullptr, "current character identity");

  dt::DatatypeOperationValue character_source;
  character_source.type_id = dt::CanonicalTypeId::character;
  character_source.encoded_value = "SBINTERVAL1:M=-7;D=8;NS=-9";
  character_source.descriptor = character_descriptor;

  dt::DatatypeOperationValue null_source;
  null_source.type_id = dt::CanonicalTypeId::null_type;
  null_source.is_null = true;

  unsigned executed = 0;
  unsigned admitted = 0;
  unsigned forbidden = 0;
  for (p::u32 row = 1; row <= 221; ++row) {
    for (const auto context : {dt::DatatypeCastContext::implicit,
                               dt::DatatypeCastContext::assignment,
                               dt::DatatypeCastContext::explicit_cast}) {
      const auto expected = ExpectedDisposition(row, context);
      Check(dt::ClassifyIntervalCastPolicyRowV3(row, context) == expected,
            "closed cast classifier decision");

      dt::IntervalCastRequestV3 request;
      request.one_based_policy_row = row;
      request.context = context;
      if (row <= 111) {
        request.interval_source = &source;
      } else {
        request.interval_target = &profile;
      }

      if (row == 13) {
        request.scalar_target = dt::CanonicalTypeId::character;
        request.scalar_target_identity = character_identity;
        request.scalar_target_descriptor = character_descriptor;
      } else if (row == 39 || row == 100 || row == 210) {
        request.interval_source = &source;
        request.interval_target = &profile;
        request.interval_target_descriptor = nullptr;
      } else if (row == 124) {
        request.scalar_source = &character_source;
        request.scalar_source_identity = character_identity;
        request.interval_target_descriptor = &interval_descriptor;
      } else if (row == 158) {
        request.scalar_source = &null_source;
        request.interval_target_descriptor = &interval_descriptor;
      }

      std::array<char, 64> destination;
      destination.fill(static_cast<char>(0xa5));
      const auto destination_before = destination;
      if (expected == dt::IntervalCastPolicyDispositionV3::forbidden) {
        request.use_character_output_buffer = true;
        request.character_output = destination.data();
        request.character_output_capacity = destination.size();
      }

      const auto result = dt::CastIntervalValueV3(request);
      ++executed;
      if (expected == dt::IntervalCastPolicyDispositionV3::forbidden) {
        ++forbidden;
        Check(AtomicCastFailure(result),
              "forbidden cast returns default unpublished result");
        Check(result.diagnostic.diagnostic_code == "DATATYPE.CAST_FORBIDDEN",
              "qualified forbidden cast diagnostic");
        Check(destination == destination_before,
              "forbidden cast destination remains byte identical");
        continue;
      }

      ++admitted;
      Check(result.ok(), "admitted cast succeeds");
      if (row == 13) {
        Check(result.category == dt::DatatypeCastCategory::lossless_explicit &&
                  !result.produced_interval &&
                  !result.used_character_output_buffer &&
                  result.scalar_value.type_id == dt::CanonicalTypeId::character &&
                  result.scalar_value.encoded_value == character_source.encoded_value &&
                  !result.scalar_value.is_null,
              "explicit interval to character exact result");
      } else if (row == 124) {
        Check(result.category == dt::DatatypeCastCategory::lossless_explicit &&
                  result.produced_interval &&
                  result.interval_value.months == -7 &&
                  result.interval_value.civil_days == 8 &&
                  result.interval_value.fixed_nanoseconds == -9,
              "explicit character to interval exact result");
      } else if (row == 158) {
        Check(result.category == dt::DatatypeCastCategory::identity &&
                  result.produced_interval &&
                  result.interval_value.state ==
                      dt::IntervalValueStateV3::sql_null &&
                  result.interval_value.months == 0 &&
                  result.interval_value.civil_days == 0 &&
                  result.interval_value.fixed_nanoseconds == 0,
              "contextual NULL exact result");
      } else {
        Check(result.category == dt::DatatypeCastCategory::identity &&
                  result.produced_interval &&
                  result.interval_value.state == dt::IntervalValueStateV3::value &&
                  result.interval_value.months == -7 &&
                  result.interval_value.civil_days == 8 &&
                  result.interval_value.fixed_nanoseconds == -9,
              "identity and alias exact result");
      }
    }
  }

  Check(executed == 663 && forbidden == 649 && admitted == 14,
        "exact 663-case cast disposition distribution");
  Check(dt::ClassifyIntervalCastPolicyRowV3(
            0, dt::DatatypeCastContext::explicit_cast) ==
            dt::IntervalCastPolicyDispositionV3::forbidden,
        "row zero refused");
  Check(dt::ClassifyIntervalCastPolicyRowV3(
            222, dt::DatatypeCastContext::explicit_cast) ==
            dt::IntervalCastPolicyDispositionV3::forbidden,
        "row 222 refused");
}

bool AlwaysCancelled(void*) noexcept {
  return true;
}

void CastFailureBoundaries(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile) {
  const auto source = Value(profile, -7, 8, -9);
  const auto* character_identity = Identity(dt::CanonicalTypeId::character);
  Check(character_identity != nullptr,
        "failure boundary character identity");
  const auto character_descriptor = Descriptor(dt::CanonicalTypeId::character);
  const auto interval_descriptor = Descriptor(dt::CanonicalTypeId::interval);

  dt::DatatypeOperationValue character_source;
  character_source.type_id = dt::CanonicalTypeId::character;
  character_source.encoded_value = "SBINTERVAL1:M=-7;D=8;NS=-9";
  character_source.descriptor = character_descriptor;

  dt::IntervalCastRequestV3 incoming;
  incoming.one_based_policy_row = 124;
  incoming.context = dt::DatatypeCastContext::explicit_cast;
  incoming.scalar_source = &character_source;
  incoming.scalar_source_identity = character_identity;
  incoming.interval_target = &profile;
  incoming.interval_target_descriptor = &interval_descriptor;

  auto wrong_character_identity = *character_identity;
  ++wrong_character_identity.legacy_fields.type_generation;
  incoming.scalar_source_identity = &wrong_character_identity;
  auto result = dt::CastIntervalValueV3(incoming);
  Check(UnpublishedCastPayload(result) &&
            result.diagnostic.diagnostic_code ==
                "CTI.INTERVAL.DESCRIPTOR_INVALID",
        "character identity mutation refuses atomically");

  incoming.scalar_source_identity = character_identity;
  auto wrong_interval_descriptor = interval_descriptor;
  ++wrong_interval_descriptor.descriptor_epoch;
  incoming.interval_target_descriptor = &wrong_interval_descriptor;
  result = dt::CastIntervalValueV3(incoming);
  Check(UnpublishedCastPayload(result) &&
            result.diagnostic.diagnostic_code ==
                "CTI.INTERVAL.DESCRIPTOR_INVALID",
        "interval descriptor mutation refuses atomically");

  incoming.interval_target_descriptor = &interval_descriptor;
  character_source.encoded_value = "not-an-interval";
  result = dt::CastIntervalValueV3(incoming);
  Check(UnpublishedCastPayload(result) &&
            result.diagnostic.diagnostic_code ==
                "CTI.TEMPORAL.INVALID_LITERAL",
        "invalid character payload refuses atomically");
  character_source.encoded_value = "SBINTERVAL1:M=-7;D=8;NS=-9";

  incoming.control.maximum_allocation_bytes = 15;
  result = dt::CastIntervalValueV3(incoming);
  Check(UnpublishedCastPayload(result) &&
            result.diagnostic.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED",
        "incoming result grant refuses atomically");
  incoming.control = {};
  incoming.control.cancelled = AlwaysCancelled;
  result = dt::CastIntervalValueV3(incoming);
  Check(UnpublishedCastPayload(result) &&
            result.diagnostic.diagnostic_code == "PROCESS.CANCELLED",
        "incoming cancellation refuses atomically");

  dt::IntervalCastRequestV3 outgoing;
  outgoing.one_based_policy_row = 13;
  outgoing.context = dt::DatatypeCastContext::explicit_cast;
  outgoing.interval_source = &source;
  outgoing.scalar_target = dt::CanonicalTypeId::character;
  outgoing.scalar_target_identity = character_identity;
  outgoing.scalar_target_descriptor = character_descriptor;
  outgoing.use_character_output_buffer = true;
  std::array<char, 64> destination;
  destination.fill(static_cast<char>(0xa5));
  const auto destination_before = destination;
  outgoing.character_output = destination.data();
  outgoing.character_output_capacity = character_source.encoded_value.size() - 1;
  result = dt::CastIntervalValueV3(outgoing);
  Check(UnpublishedCastPayload(result) &&
            result.diagnostic.diagnostic_code == "CTB.TEXT.LENGTH_EXCEEDED" &&
            result.bytes_required == character_source.encoded_value.size() &&
            destination == destination_before,
        "short character destination refuses without publication");

  outgoing.character_output_capacity = destination.size();
  outgoing.control.maximum_allocation_bytes =
      character_source.encoded_value.size() - 1;
  result = dt::CastIntervalValueV3(outgoing);
  Check(UnpublishedCastPayload(result) &&
            result.diagnostic.diagnostic_code == "RESOURCE.BUDGET_EXCEEDED" &&
            destination == destination_before,
        "outgoing resource refusal preserves destination");
  outgoing.control = {};
  outgoing.control.cancelled = AlwaysCancelled;
  result = dt::CastIntervalValueV3(outgoing);
  Check(UnpublishedCastPayload(result) &&
            result.diagnostic.diagnostic_code == "PROCESS.CANCELLED" &&
            destination == destination_before,
        "outgoing cancellation preserves destination");

  dt::DatatypeOperationValue null_source;
  null_source.type_id = dt::CanonicalTypeId::null_type;
  null_source.is_null = true;
  dt::IntervalCastRequestV3 contextual;
  contextual.one_based_policy_row = 158;
  contextual.context = dt::DatatypeCastContext::implicit;
  contextual.scalar_source = &null_source;
  contextual.interval_target = &profile;
  contextual.interval_target_descriptor = &interval_descriptor;
  contextual.target_null_allowed = false;
  result = dt::CastIntervalValueV3(contextual);
  Check(UnpublishedCastPayload(result) &&
            result.diagnostic.diagnostic_code == "DATATYPE.NULL_NOT_ADMITTED",
        "contextual NULL respects target nullability atomically");
  contextual.target_null_allowed = true;
  null_source.encoded_value = "dirty";
  result = dt::CastIntervalValueV3(contextual);
  Check(UnpublishedCastPayload(result) &&
            result.diagnostic.diagnostic_code ==
                "DATATYPE.NULL_STATE.INVALID",
        "contextual dirty NULL refuses atomically");
}

void GenericCarrierRefusal() {
  dt::DatatypeCastRequest request;
  request.value.type_id = dt::CanonicalTypeId::interval;
  request.value.encoded_value.assign(4096, static_cast<char>(0x5a));
  request.value.descriptor = Descriptor(dt::CanonicalTypeId::interval);
  request.target_type_id = dt::CanonicalTypeId::character;
  request.target_descriptor = Descriptor(dt::CanonicalTypeId::character);
  request.context = dt::DatatypeCastContext::explicit_cast;

  const auto source_before = request.value.encoded_value;
  const auto dispatch_before = interval_dispatch_calls;
#if defined(SB_PUBLIC_RELEASE_ASAN_UBSAN_PROFILE)
  ASAN_POISON_MEMORY_REGION(request.value.encoded_value.data(),
                            request.value.encoded_value.size());
#endif
  const auto result = dt::CastDatatypeValue(request);
#if defined(SB_PUBLIC_RELEASE_ASAN_UBSAN_PROFILE)
  ASAN_UNPOISON_MEMORY_REGION(request.value.encoded_value.data(),
                              request.value.encoded_value.size());
#endif
  Check(!result.ok() &&
            result.category == dt::DatatypeCastCategory::forbidden &&
            result.diagnostic.diagnostic_code ==
                "CTI.INTERVAL.DESCRIPTOR_INVALID" &&
            DefaultScalarValue(result.value),
        "generic unauthenticated interval carrier refusal");
  Check(interval_dispatch_calls == dispatch_before,
        "generic carrier refusal never dispatches qualified interval converter");
  Check(request.value.encoded_value == source_before,
        "generic carrier refusal preserves source payload");
}

void Properties(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile) {
  p::u64 random = kSeed;
  for (p::u64 ordinal = 0; ordinal < 4096; ++ordinal) {
    const auto months = static_cast<std::int32_t>(Next(random));
    const auto days = static_cast<std::int32_t>(Next(random));
    const auto nanoseconds = static_cast<std::int64_t>(Next(random));
    auto constructed =
        dt::ConstructIntervalV3(profile, months, days, nanoseconds);
    Property(constructed.ok() || DefaultIntervalValue(constructed.value),
             "construct failure atomic", ordinal);
    Property(constructed.ok(), "construct", ordinal);
    const auto& value = constructed.value;

    auto encoded = dt::EncodeCanonicalIntervalComponentV3(value);
    Property(encoded.ok() || encoded.bytes.empty(),
             "encode failure atomic", ordinal);
    Property(encoded.ok() && encoded.bytes.size() == 16,
             "encode", ordinal);

    auto decoded = dt::DecodeCanonicalIntervalComponentNoAllocV3(
        *profile, dt::IntervalValueStateV3::value, true, encoded.bytes);
    Property(decoded.ok() ||
                 (!decoded.value.profile && decoded.value.months == 0 &&
                  decoded.value.civil_days == 0 &&
                  decoded.value.fixed_nanoseconds == 0),
             "decode failure atomic", ordinal);
    Property(decoded.ok() && decoded.value.months == months &&
                 decoded.value.civil_days == days &&
                 decoded.value.fixed_nanoseconds == nanoseconds,
             "LE16 identity", ordinal);

    auto text = dt::RenderCanonicalIntervalV3(value);
    Property(text.ok() || (text.text.empty() && !text.containing_null),
             "render failure atomic", ordinal);
    Property(text.ok(), "render", ordinal);
    auto parsed = dt::ParseCanonicalIntervalV3(profile, text.text);
    Property(parsed.ok() || DefaultIntervalValue(parsed.value),
             "parse failure atomic", ordinal);
    Property(parsed.ok() && parsed.value.months == months &&
                 parsed.value.civil_days == days &&
                 parsed.value.fixed_nanoseconds == nanoseconds,
             "render parse identity", ordinal);

    auto hash1 = dt::HashIntervalValueV3(value);
    auto hash2 = dt::HashIntervalValueV3(value);
    Property(hash1.ok() || hash1.bytes.empty(),
             "first hash failure atomic", ordinal);
    Property(hash2.ok() || hash2.bytes.empty(),
             "second hash failure atomic", ordinal);
    Property(hash1.ok() && hash2.ok() && hash1.bytes == hash2.bytes &&
                 hash1.bytes.size() == 32,
             "deterministic hash", ordinal);

    auto equality = dt::EqualIntervalValuesV3(value.view(), value.view());
    Property(equality.ok() ||
                 (equality.fact == dt::IntervalEqualityFactV3::not_equal &&
                  !equality.grouping_equivalent && !equality.null_equivalent),
             "equality failure atomic", ordinal);
    Property(equality.ok() &&
                 equality.fact == dt::IntervalEqualityFactV3::equal &&
                 equality.grouping_equivalent,
             "equality reflexivity", ordinal);

    const auto other_months = static_cast<std::int32_t>(Next(random));
    const auto other_days = static_cast<std::int32_t>(Next(random));
    const auto other_nanoseconds = static_cast<std::int64_t>(Next(random));
    auto other = dt::ConstructIntervalV3(profile, other_months, other_days,
                                         other_nanoseconds);
    Property(other.ok() || DefaultIntervalValue(other.value),
             "other construct failure atomic", ordinal);
    Property(other.ok(), "other construct", ordinal);

    const WideInt wide_add_months =
        static_cast<WideInt>(months) + other_months;
    const WideInt wide_add_days = static_cast<WideInt>(days) + other_days;
    const WideInt wide_add_nanoseconds =
        static_cast<WideInt>(nanoseconds) + other_nanoseconds;
    const bool add_ok =
        wide_add_months >= std::numeric_limits<std::int32_t>::min() &&
        wide_add_months <= std::numeric_limits<std::int32_t>::max() &&
        wide_add_days >= std::numeric_limits<std::int32_t>::min() &&
        wide_add_days <= std::numeric_limits<std::int32_t>::max() &&
        wide_add_nanoseconds >= std::numeric_limits<std::int64_t>::min() &&
        wide_add_nanoseconds <= std::numeric_limits<std::int64_t>::max();
    auto add = dt::AddIntervalCheckedV3(value, other.value);
    Property(add.ok() || DefaultIntervalValue(add.value),
             "checked add failure atomic", ordinal);
    Property(add.ok() == add_ok, "checked add disposition", ordinal);
    if (add_ok) {
      Property(add.value.months == wide_add_months &&
                   add.value.civil_days == wide_add_days &&
                   add.value.fixed_nanoseconds == wide_add_nanoseconds,
               "checked add oracle and no carry", ordinal);
    }

    const WideInt wide_sub_months =
        static_cast<WideInt>(months) - other_months;
    const WideInt wide_sub_days = static_cast<WideInt>(days) - other_days;
    const WideInt wide_sub_nanoseconds =
        static_cast<WideInt>(nanoseconds) - other_nanoseconds;
    const bool subtract_ok =
        wide_sub_months >= std::numeric_limits<std::int32_t>::min() &&
        wide_sub_months <= std::numeric_limits<std::int32_t>::max() &&
        wide_sub_days >= std::numeric_limits<std::int32_t>::min() &&
        wide_sub_days <= std::numeric_limits<std::int32_t>::max() &&
        wide_sub_nanoseconds >= std::numeric_limits<std::int64_t>::min() &&
        wide_sub_nanoseconds <= std::numeric_limits<std::int64_t>::max();
    auto subtract = dt::SubtractIntervalCheckedV3(value, other.value);
    Property(subtract.ok() || DefaultIntervalValue(subtract.value),
             "checked subtract failure atomic", ordinal);
    Property(subtract.ok() == subtract_ok,
             "checked subtract disposition", ordinal);
    if (subtract_ok) {
      Property(subtract.value.months == wide_sub_months &&
                   subtract.value.civil_days == wide_sub_days &&
                   subtract.value.fixed_nanoseconds == wide_sub_nanoseconds,
               "checked subtract oracle and no carry", ordinal);
    }
  }
}

struct MalformedMutation {
  std::int32_t months = 0;
  std::int32_t days = 0;
  std::int64_t nanoseconds = 0;
  std::size_t changed_offset = 0;
  unsigned char changed_mask = 1;
  std::string trailing;
};

MalformedMutation ReplayMalformedMutation() {
  p::u64 random = kMalformedSeed;
  for (p::u64 ordinal = 0; ordinal <= kMalformedOrdinal; ++ordinal) {
    (void) Next(random);
  }
  MalformedMutation mutation;
  mutation.months = static_cast<std::int32_t>(Next(random));
  mutation.days = static_cast<std::int32_t>(Next(random));
  mutation.nanoseconds = static_cast<std::int64_t>(Next(random));
  mutation.changed_offset = static_cast<std::size_t>(Next(random) % 20u) + 5u;
  mutation.changed_mask =
      static_cast<unsigned char>((Next(random) & 0xffu) | 1u);
  mutation.trailing.assign(static_cast<std::size_t>(Next(random) % 4u) + 1u,
                           '!');
  return mutation;
}

std::string MaterializeMalformed(const MalformedMutation& mutation) {
  std::string text = "SBINTERVAL1:M=" + std::to_string(mutation.months) +
                     ";D=" + std::to_string(mutation.days) +
                     ";NS=" + std::to_string(mutation.nanoseconds);
  const auto offset = std::min(mutation.changed_offset, text.size() - 1);
  text[offset] = static_cast<char>(
      static_cast<unsigned char>(text[offset]) ^ mutation.changed_mask);
  text += mutation.trailing;
  return text;
}

bool MalformedFailureIsAtomic(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile,
    const MalformedMutation& mutation) {
  const auto result =
      dt::ParseCanonicalIntervalV3(profile, MaterializeMalformed(mutation));
  return !result.ok() && DefaultIntervalValue(result.value);
}

void MalformedShrink(
    const std::shared_ptr<const dt::IntervalValidatedProfileHandleV3>& profile) {
  auto mutation = ReplayMalformedMutation();
  Check(MalformedFailureIsAtomic(profile, mutation),
        "recorded malformed replay fails atomically");

  while (!mutation.trailing.empty()) {
    auto candidate = mutation;
    candidate.trailing.pop_back();
    if (!MalformedFailureIsAtomic(profile, candidate)) {
      break;
    }
    mutation = std::move(candidate);
  }

  while (mutation.changed_offset != 0) {
    auto candidate = mutation;
    --candidate.changed_offset;
    if (!MalformedFailureIsAtomic(profile, candidate)) {
      break;
    }
    mutation = candidate;
  }

  for (int bit = 7; bit >= 0; --bit) {
    const auto cleared = static_cast<unsigned char>(
        mutation.changed_mask & ~(static_cast<unsigned char>(1u << bit)));
    if (cleared == 0) {
      continue;
    }
    auto candidate = mutation;
    candidate.changed_mask = cleared;
    if (MalformedFailureIsAtomic(profile, candidate)) {
      mutation = candidate;
    }
  }

  const auto shrink_i32 = [&](std::int32_t MalformedMutation::* field) {
    while (mutation.*field != 0) {
      auto candidate = mutation;
      candidate.*field /= 2;
      if (candidate.*field == mutation.*field) {
        candidate.*field = 0;
      }
      if (!MalformedFailureIsAtomic(profile, candidate)) {
        break;
      }
      mutation = candidate;
    }
  };
  shrink_i32(&MalformedMutation::months);
  shrink_i32(&MalformedMutation::days);
  while (mutation.nanoseconds != 0) {
    auto candidate = mutation;
    candidate.nanoseconds /= 2;
    if (candidate.nanoseconds == mutation.nanoseconds) {
      candidate.nanoseconds = 0;
    }
    if (!MalformedFailureIsAtomic(profile, candidate)) {
      break;
    }
    mutation = candidate;
  }

  const auto minimal = MaterializeMalformed(mutation);
  if (minimal != "RBINTERVAL1:M=0;D=0;NS=0") {
    std::cerr << "FAIL malformed shrink minimal replay mapping="
              << kMalformedMapping << " seed=0x" << std::hex
              << kMalformedSeed << std::dec << " ordinal="
              << kMalformedOrdinal << " minimal=" << minimal << '\n';
    std::exit(EXIT_FAILURE);
  }
  ++checks;
  Check(MalformedFailureIsAtomic(profile, mutation),
        "minimal malformed replay remains an atomic failure");
}

}  // namespace

extern "C" dt::IntervalCastResultV3
__real__ZN11scratchbird4core9datatypes19CastIntervalValueV3ERKNS1_21IntervalCastRequestV3E(
    const dt::IntervalCastRequestV3&);

extern "C" dt::IntervalCastResultV3
__wrap__ZN11scratchbird4core9datatypes19CastIntervalValueV3ERKNS1_21IntervalCastRequestV3E(
    const dt::IntervalCastRequestV3& request) {
  ++interval_dispatch_calls;
  return __real__ZN11scratchbird4core9datatypes19CastIntervalValueV3ERKNS1_21IntervalCastRequestV3E(
      request);
}

int main() {
  auto profile = Profile();
  CastCorpus(profile);
  CastFailureBoundaries(profile);
  GenericCarrierRefusal();
  Properties(profile);
  MalformedShrink(profile);
  std::cout << "PASS checks=" << checks << " mapping=" << kMapping
            << " seed=0x" << std::hex << kSeed << std::dec
            << " property_cases=4096 cast_decisions=663 cast_executions=663"
            << " forbidden=649 admitted=14 malformed_mapping="
            << kMalformedMapping << " malformed_seed=0x" << std::hex
            << kMalformedSeed << std::dec << " malformed_ordinal="
            << kMalformedOrdinal << '\n';
}
