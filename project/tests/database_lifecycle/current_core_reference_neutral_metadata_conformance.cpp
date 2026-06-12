// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "reference_compatibility_metadata.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace datatypes = scratchbird::core::datatypes;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasMetric(const datatypes::ReferenceCompatibilityMetadataResult& result,
               std::string_view family,
               std::string_view reason) {
  for (const auto& metric : result.metric_events) {
    if (metric.family == family && metric.reason == reason) {
      return true;
    }
  }
  return false;
}

datatypes::ReferenceCompatibilityMetadataRequest SqlServerUuidRequest() {
  datatypes::ReferenceCompatibilityMetadataRequest request;
  request.dialect = datatypes::ReferenceDialectId::sql_server;
  request.reference_version_profile = "2022";
  request.min_supported_version = "2012";
  request.max_supported_version = "2022";
  request.reference_label = "UNIQUEIDENTIFIER";
  return request;
}

void TestCurrentCoreRuntimeMappingRecord() {
  const auto result =
      datatypes::EvaluateReferenceCompatibilityMetadata(SqlServerUuidRequest());
  Require(result.ok, "MDF-026 rejected valid reference-neutral mapping");
  Require(result.record.runtime_mapping_record,
          "MDF-026 did not create runtime mapping record");
  Require(result.record.target_type_id == datatypes::CanonicalTypeId::uuid,
          "MDF-026 mapped reference UUID label to the wrong canonical type");
  Require(result.record.canonical_type_name == "uuid",
          "MDF-026 runtime mapping did not expose canonical type name");
  Require(!result.record.parser_authority_accepted,
          "MDF-026 accepted parser authority for reference metadata");
  Require(!result.record.reference_parser_required,
          "MDF-026 required a reference/reference parser for current-core metadata");
  Require(HasMetric(result,
                    "sys.metrics.datatypes.reference.runtime_mapping_count",
                    "canonical_descriptor"),
          "MDF-026 did not emit reference runtime mapping metric event");
}

void TestUnsupportedByVersionRefusal() {
  auto request = SqlServerUuidRequest();
  request.reference_version_profile = "2008";
  const auto result =
      datatypes::EvaluateReferenceCompatibilityMetadata(request);
  Require(!result.ok, "MDF-026 admitted unsupported reference version");
  Require(result.record.unsupported_by_version,
          "MDF-026 unsupported version did not set the version flag");
  Require(result.record.diagnostic_code ==
              "SB-REFERENCE-COMPAT-UNSUPPORTED-BY-VERSION",
          "MDF-026 unsupported version diagnostic mismatch");
  Require(result.record.fallback_behavior == "refuse_unsupported_by_version",
          "MDF-026 unsupported version did not fail closed");
  Require(HasMetric(result,
                    "sys.metrics.datatypes.reference.fallback_refusal_count",
                    "unsupported_by_version"),
          "MDF-026 unsupported version metric event missing");
}

void TestParserAuthorityAndFallbackRefusals() {
  auto parser_authority = SqlServerUuidRequest();
  parser_authority.parser_claims_authority = true;
  const auto parser_result =
      datatypes::EvaluateReferenceCompatibilityMetadata(parser_authority);
  Require(!parser_result.ok, "MDF-026 admitted parser metadata authority");
  Require(parser_result.record.diagnostic_code ==
              "SB-REFERENCE-COMPAT-PARSER-AUTHORITY-REFUSED",
          "MDF-026 parser authority diagnostic mismatch");
  Require(!parser_result.record.parser_authority_accepted,
          "MDF-026 parser authority was marked accepted");

  auto unmapped = SqlServerUuidRequest();
  unmapped.reference_label = "HIERARCHYID";
  const auto unmapped_result =
      datatypes::EvaluateReferenceCompatibilityMetadata(unmapped);
  Require(!unmapped_result.ok, "MDF-026 admitted unmapped reference label");
  Require(unmapped_result.record.diagnostic_code ==
              "SB-DATATYPE-REFERENCE-LABEL-UNMAPPED",
          "MDF-026 unmapped reference label diagnostic mismatch");
  Require(unmapped_result.record.fallback_behavior == "metadata_only_refusal",
          "MDF-026 unmapped reference label did not use metadata-only refusal");

  auto disabled = SqlServerUuidRequest();
  disabled.allow_placeholder_mapping = false;
  const auto disabled_result =
      datatypes::EvaluateReferenceCompatibilityMetadata(disabled);
  Require(!disabled_result.ok, "MDF-026 admitted disabled placeholder mapping");
  Require(disabled_result.record.diagnostic_code ==
              "SB-REFERENCE-COMPAT-PLACEHOLDER-MAPPING-DISABLED",
          "MDF-026 disabled placeholder diagnostic mismatch");
}

}  // namespace

int main() {
  TestCurrentCoreRuntimeMappingRecord();
  TestUnsupportedByVersionRefusal();
  TestParserAuthorityAndFallbackRefusals();
  std::cout << "current_core_reference_neutral_metadata_conformance=passed\n";
  return EXIT_SUCCESS;
}
