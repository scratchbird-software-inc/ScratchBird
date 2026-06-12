// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "datatype_exchange.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

// SEARCH_KEY: MDF-026-CURRENT-CORE-REFERENCE-NEUTRAL-METADATA
// Current-core reference compatibility metadata. This surface is intentionally
// reference-system-neutral: it produces descriptor mapping/refusal records and metric
// names without invoking reference parsers or accepting parser authority.

struct ReferenceCompatibilityMetadataRequest {
  ReferenceDialectId dialect = ReferenceDialectId::unknown;
  std::string reference_version_profile;
  std::string reference_label;
  std::string min_supported_version;
  std::string max_supported_version;
  bool parser_claims_authority = false;
  bool allow_placeholder_mapping = true;
};

struct ReferenceCompatibilityRuntimeMappingRecord {
  ReferenceDialectId dialect = ReferenceDialectId::unknown;
  std::string reference_version_profile;
  std::string reference_label;
  CanonicalTypeId target_type_id = CanonicalTypeId::unknown;
  std::string canonical_type_name;
  bool runtime_mapping_record = false;
  bool unsupported_by_version = false;
  bool fallback_refused = false;
  bool parser_authority_accepted = false;
  bool reference_parser_required = false;
  std::string diagnostic_code;
  std::string fallback_behavior;
};

struct ReferenceCompatibilityMetricEvent {
  std::string family;
  std::string dialect;
  std::string result;
  std::string reason;
};

struct ReferenceCompatibilityMetadataResult {
  bool ok = false;
  ReferenceCompatibilityRuntimeMappingRecord record;
  std::vector<ReferenceCompatibilityMetricEvent> metric_events;
};

ReferenceCompatibilityMetadataResult EvaluateReferenceCompatibilityMetadata(
    const ReferenceCompatibilityMetadataRequest& request);

}  // namespace scratchbird::core::datatypes
