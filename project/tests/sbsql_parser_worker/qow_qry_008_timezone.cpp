// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "datatype_temporal_wire.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;

namespace scratchbird::engine::internal_api {
bool QowNormalizeCanonicalTimezoneScalarV1(
    const EngineTypedValue& input_value,
    const scratchbird::core::datatypes::TimezoneSeedAuthority& timezone_seed,
    EngineApiU64 resource_epoch,
    EngineApiU64 timezone_epoch,
    EngineTypedValue* output_value,
    std::string* timezone_identifier,
    int* timezone_offset_minutes,
    bool* used_timezone_seed,
    std::string* refusal_detail);
}  // namespace scratchbird::engine::internal_api

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

api::EngineDescriptor TimestampDescriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000000861";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "timestamp";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000000862;"
      "nullability=nullable;timezone_profile_id=timestamp_timezone_profile;"
      "fractional_second_precision=6";
  return descriptor;
}

dt::TimezoneSeedAuthority TimezoneAuthority() {
  dt::TimezoneSeedAuthority authority;
  authority.active = true;
  authority.seed_pack_name = "qow_core_resource_catalog";
  authority.seed_pack_version = "2026.07";
  authority.content_hash = "sha256:qow-timezone-seed";
  authority.timezone_records = 2;
  authority.timezone_transition_records = 100;
  authority.timezone_leap_second_records = 27;
  authority.timezone_names = {"America/Toronto", "Etc/UTC"};
  return authority;
}

api::EngineTypedValue TimestampValue(const std::string& value) {
  api::EngineTypedValue typed;
  typed.descriptor = TimestampDescriptor();
  typed.encoded_value = value;
  typed.state = api::EngineValueState::value;
  return typed;
}

bool Normalize(const api::EngineTypedValue& input,
               const dt::TimezoneSeedAuthority& authority,
               api::EngineTypedValue* output,
               std::string* timezone_identifier,
               int* offset_minutes,
               bool* used_seed,
               std::string* refusal) {
  return api::QowNormalizeCanonicalTimezoneScalarV1(
      input, authority, 41, 19, output, timezone_identifier, offset_minutes,
      used_seed, refusal);
}

bool ValidateAcceptedTimezoneNormalization() {
  api::EngineTypedValue output;
  std::string timezone_identifier;
  int offset_minutes = 99;
  bool used_seed = false;
  std::string refusal;
  const bool named_accepted = Normalize(
      TimestampValue("2026-07-25T10:15:30 America/Toronto"),
      TimezoneAuthority(), &output, &timezone_identifier, &offset_minutes,
      &used_seed, &refusal);
  bool passed = true;
  passed &= Require(named_accepted && refusal.empty(),
                    "catalog-known named timezone was refused");
  passed &= Require(
      used_seed && timezone_identifier == "America/Toronto" &&
          output.descriptor.descriptor_uuid.canonical ==
              TimestampDescriptor().descriptor_uuid.canonical &&
          output.encoded_value.find("zone=America/Toronto") !=
              std::string::npos &&
          output.state == api::EngineValueState::value,
      "named timezone normalization lost authority or descriptor identity");

  refusal.clear();
  used_seed = true;
  const bool offset_accepted = Normalize(
      TimestampValue("2026-07-25T10:15:30-04:00"), TimezoneAuthority(),
      &output, &timezone_identifier, &offset_minutes, &used_seed, &refusal);
  passed &= Require(offset_accepted && timezone_identifier == "-04:00" &&
                        offset_minutes == -240 && !used_seed,
                    "explicit timezone offset was not normalized exactly");
  return passed;
}

bool ValidateTimezoneRefusalAndNull() {
  api::EngineTypedValue output;
  std::string timezone_identifier;
  int offset_minutes = 0;
  bool used_seed = false;
  std::string refusal;
  const bool unknown_accepted = Normalize(
      TimestampValue("2026-07-25T10:15:30 Mars/Olympus"),
      TimezoneAuthority(), &output, &timezone_identifier, &offset_minutes,
      &used_seed, &refusal);

  auto duplicate = TimestampValue("2026-07-25T10:15:30Z");
  duplicate.descriptor.encoded_descriptor +=
      ";timezone_profile_id=timestamp_timezone_profile";
  refusal.clear();
  const bool duplicate_accepted = Normalize(
      duplicate, TimezoneAuthority(), &output, &timezone_identifier,
      &offset_minutes, &used_seed, &refusal);

  auto incomplete_authority = TimezoneAuthority();
  incomplete_authority.content_hash.clear();
  refusal.clear();
  const bool incomplete_accepted = Normalize(
      TimestampValue("2026-07-25T10:15:30Z"), incomplete_authority, &output,
      &timezone_identifier, &offset_minutes, &used_seed, &refusal);

  auto null_value = TimestampValue({});
  null_value.state = api::EngineValueState::sql_null;
  null_value.is_null = false;
  refusal.clear();
  const bool null_accepted = Normalize(
      null_value, TimezoneAuthority(), &output, &timezone_identifier,
      &offset_minutes, &used_seed, &refusal);

  bool passed = true;
  passed &= Require(!unknown_accepted,
                    "timezone absent from seed catalog was accepted");
  passed &= Require(!duplicate_accepted,
                    "duplicate timezone profile descriptor was accepted");
  passed &= Require(!incomplete_accepted,
                    "incomplete timezone seed authority was accepted");
  passed &= Require(null_accepted && output.isSqlNull() &&
                        output.encoded_value.empty() &&
                        output.descriptor.descriptor_uuid.canonical ==
                            TimestampDescriptor().descriptor_uuid.canonical,
                    "canonical temporal SQL NULL was not propagated");
  return passed;
}

}  // namespace

// QOW-TEST-QRY-008-TIMEZONE-V1
int main() {
  bool passed = true;
  passed &= ValidateAcceptedTimezoneNormalization();
  passed &= ValidateTimezoneRefusalAndNull();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
