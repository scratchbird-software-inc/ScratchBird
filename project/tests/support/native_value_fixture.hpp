// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Native SBLR DATE fixtures for function-dispatch component tests.
//
// The DATE calendar/month routines (src/engine/sblr/sblr_temporal_cast.cpp:
// EvaluateSblrTemporalCalendar, EvaluateSblrDateMonth) accept only an exact
// bound DATE carrier: descriptor_id "date", payload_kind binary, four bytes of
// signed_i32_days_since_Unix_epoch little-endian (src/core/datatypes/
// datatype_date.cpp:479 and EncodeCanonicalDateComponentIntoNoAllocV3), no
// text carrier, and a projection_descriptor in the same admitted datatype
// cohort as the request's result descriptor. The month count of ADD_MONTHS is
// a same-cohort INT64 (SblrInt64PayloadValid, sblr_binary_value_carrier.hpp).
// The binding shape mirrors Binding()/Native()/Months() in
// tests/sbsql_sblr_alignment/native_calendar_test.cpp. These are structural
// component fixtures only: no statement, catalog pin or security receipt.
#include "binary_uuid_fixture.hpp"
#include "admitted_datatype_cohort.hpp"
#include "executor/descriptor_value_runtime.hpp"
#include "sblr/sblr_runtime.hpp"

#include <bit>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace scratchbird::tests {

// Current admitted datatype cohort (V11), nullable unless stated otherwise.
inline std::shared_ptr<const engine::internal_api::EngineDescriptor> NativeBoundDescriptor(
    const std::string& canonical_type_name, bool nullable = true) {
  const engine::internal_api::EngineDatatypeCohort cohort{
      core::datatypes::kDatatypeCohortV11, 11, 11};
  auto binding = engine::executor::MakeExecutorDescriptor(
      canonical_type_name, nullable ? "nullability=nullable" : "nullability=non_null", cohort);
  binding.descriptor_uuid = FixtureUuid(2290, static_cast<std::uint32_t>(
      canonical_type_name.empty() ? 0 : static_cast<unsigned char>(canonical_type_name.front())));
  return std::make_shared<const engine::internal_api::EngineDescriptor>(std::move(binding));
}

inline std::int32_t NativeDateDays(std::chrono::year_month_day civil) {
  return static_cast<std::int32_t>(std::chrono::sys_days{civil}.time_since_epoch().count());
}

inline engine::sblr::SblrValue NativeDateSblrValue(std::chrono::year_month_day civil) {
  engine::sblr::SblrValue value;
  value.descriptor_id = "date";
  value.projection_descriptor = NativeBoundDescriptor("date");
  value.is_null = false;
  value.payload_kind = engine::sblr::SblrValuePayloadKind::binary;
  const auto bits = std::bit_cast<std::uint32_t>(NativeDateDays(civil));
  for (unsigned byte = 0; byte < 4; ++byte)
    value.binary_value.push_back(static_cast<std::uint8_t>(bits >> (8 * byte)));
  return value;
}

// SQL NULL keeps its bound descriptor and carries no payload of any kind.
inline engine::sblr::SblrValue NativeNullSblrValue(const std::string& canonical_type_name) {
  engine::sblr::SblrValue value;
  value.descriptor_id = canonical_type_name;
  value.projection_descriptor = NativeBoundDescriptor(canonical_type_name);
  value.is_null = true;
  value.payload_kind = engine::sblr::SblrValuePayloadKind::none;
  return value;
}

inline engine::sblr::SblrValue NativeBoundInt64SblrValue(std::int64_t number) {
  engine::sblr::SblrValue value;
  value.descriptor_id = "int64";
  value.projection_descriptor = NativeBoundDescriptor("int64");
  value.is_null = false;
  value.payload_kind = engine::sblr::SblrValuePayloadKind::signed_integer;
  value.has_int64_value = true;
  value.int64_value = number;
  return value;
}

// Exact native DATE result: four LE day bytes and no other carrier.
inline bool NativeDateSblrEquals(const engine::sblr::SblrValue& value,
                                 std::chrono::year_month_day expected) {
  if (value.is_null || value.descriptor_id != "date" ||
      value.payload_kind != engine::sblr::SblrValuePayloadKind::binary ||
      value.binary_value.size() != 4 || !value.text_value.empty() ||
      !value.encoded_value.empty() || value.has_int64_value || value.has_uint64_value ||
      value.has_real64_value) return false;
  std::uint32_t bits = 0;
  for (unsigned byte = 0; byte < 4; ++byte)
    bits |= std::uint32_t(value.binary_value[byte]) << (8 * byte);
  return std::bit_cast<std::int32_t>(bits) == NativeDateDays(expected);
}

}  // namespace scratchbird::tests
