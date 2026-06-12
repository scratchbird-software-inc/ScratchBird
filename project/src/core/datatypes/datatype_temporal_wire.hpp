// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DATATYPE-TEMPORAL-WIRE-ANCHOR
#include "datatype_descriptor.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;

struct TimezoneSeedAuthority {
  bool active = false;
  std::string seed_pack_name;
  std::string seed_pack_version;
  std::string content_hash;
  u32 timezone_records = 0;
  u32 timezone_transition_records = 0;
  u32 timezone_leap_second_records = 0;
  std::vector<std::string> timezone_names;
};

struct ReferenceTemporalWireProfileRequest {
  std::string reference_engine;
  std::string reference_type_or_family;
  std::string wire_profile;
  std::string encoded_value;
  TimezoneSeedAuthority timezone_seed;
  u32 fractional_second_precision = 12;
  bool require_timezone_seed = true;
};

struct ReferenceTemporalWireProfileResult {
  Status status;
  CanonicalTypeId canonical_type_id = CanonicalTypeId::unknown;
  std::string normalized_value;
  std::string timezone_identifier;
  int timezone_offset_minutes = 0;
  bool used_timezone_seed = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

ReferenceTemporalWireProfileResult ValidateReferenceTemporalWireProfile(const ReferenceTemporalWireProfileRequest& request);
DiagnosticRecord MakeTemporalWireDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail = {});

}  // namespace scratchbird::core::datatypes
