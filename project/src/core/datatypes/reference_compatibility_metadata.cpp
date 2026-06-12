// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "reference_compatibility_metadata.hpp"

#include <cstdlib>
#include <utility>

namespace scratchbird::core::datatypes {
namespace {

constexpr const char* kMappingMetric =
    "sys.metrics.datatypes.reference.runtime_mapping_count";
constexpr const char* kRefusalMetric =
    "sys.metrics.datatypes.reference.fallback_refusal_count";

int ParseVersionNumber(const std::string& value) {
  if (value.empty()) {
    return 0;
  }
  char* end = nullptr;
  const auto parsed = std::strtol(value.c_str(), &end, 10);
  return end == value.c_str() ? 0 : static_cast<int>(parsed);
}

bool VersionOutsideBounds(const ReferenceCompatibilityMetadataRequest& request) {
  const int version = ParseVersionNumber(request.reference_version_profile);
  if (version == 0) {
    return false;
  }
  const int min_version = ParseVersionNumber(request.min_supported_version);
  const int max_version = ParseVersionNumber(request.max_supported_version);
  return (min_version != 0 && version < min_version) ||
         (max_version != 0 && version > max_version);
}

void AddMetric(ReferenceCompatibilityMetadataResult* result,
               std::string family,
               std::string result_label,
               std::string reason) {
  result->metric_events.push_back(
      {std::move(family), ReferenceDialectName(result->record.dialect),
       std::move(result_label), std::move(reason)});
}

ReferenceCompatibilityMetadataResult Refusal(
    const ReferenceCompatibilityMetadataRequest& request,
    std::string diagnostic_code,
    std::string fallback_behavior,
    std::string reason) {
  ReferenceCompatibilityMetadataResult result;
  result.record.dialect = request.dialect;
  result.record.reference_version_profile = request.reference_version_profile;
  result.record.reference_label = request.reference_label;
  result.record.fallback_refused = true;
  result.record.diagnostic_code = std::move(diagnostic_code);
  result.record.fallback_behavior = std::move(fallback_behavior);
  AddMetric(&result, kRefusalMetric, "refused", std::move(reason));
  return result;
}

}  // namespace

ReferenceCompatibilityMetadataResult EvaluateReferenceCompatibilityMetadata(
    const ReferenceCompatibilityMetadataRequest& request) {
  if (request.parser_claims_authority) {
    auto result = Refusal(request,
                          "SB-REFERENCE-COMPAT-PARSER-AUTHORITY-REFUSED",
                          "refuse_parser_authority",
                          "parser_authority_claim");
    result.record.parser_authority_accepted = false;
    result.record.reference_parser_required = false;
    return result;
  }

  if (VersionOutsideBounds(request)) {
    auto result = Refusal(request,
                          "SB-REFERENCE-COMPAT-UNSUPPORTED-BY-VERSION",
                          "refuse_unsupported_by_version",
                          "unsupported_by_version");
    result.record.unsupported_by_version = true;
    return result;
  }

  if (!request.allow_placeholder_mapping) {
    return Refusal(request,
                   "SB-REFERENCE-COMPAT-PLACEHOLDER-MAPPING-DISABLED",
                   "refuse_placeholder_mapping",
                   "placeholder_mapping_disabled");
  }

  const auto resolved =
      ResolveReferenceTypeLabelPlaceholder(request.dialect, request.reference_label);
  if (!resolved.ok()) {
    return Refusal(request,
                   "SB-DATATYPE-REFERENCE-LABEL-UNMAPPED",
                   "metadata_only_refusal",
                   "reference_label_unmapped");
  }

  ReferenceCompatibilityMetadataResult result;
  result.ok = true;
  result.record.dialect = request.dialect;
  result.record.reference_version_profile = request.reference_version_profile;
  result.record.reference_label = request.reference_label;
  result.record.target_type_id = resolved.descriptor.type_id;
  result.record.canonical_type_name = CanonicalTypeName(resolved.descriptor.type_id);
  result.record.runtime_mapping_record = true;
  result.record.parser_authority_accepted = false;
  result.record.reference_parser_required = false;
  result.record.diagnostic_code = "SB-REFERENCE-COMPAT-RUNTIME-MAPPING";
  result.record.fallback_behavior = "canonical_descriptor_mapping";
  AddMetric(&result, kMappingMetric, "mapped", "canonical_descriptor");
  return result;
}

}  // namespace scratchbird::core::datatypes
