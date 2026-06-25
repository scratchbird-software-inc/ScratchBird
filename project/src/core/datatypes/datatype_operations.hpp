// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-DATATYPE-OPERATIONS-ANCHOR
#include "datatype_binary.hpp"
#include "datatype_descriptor.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::datatypes {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;

enum class DatatypeCastCategory : u16 {
  identity,
  lossless_implicit,
  lossless_explicit,
  lossy_explicit,
  reference_compatibility_explicit,
  domain_to_base,
  base_to_domain,
  forbidden
};

enum class DatatypeSetOperationKind : u16 {
  membership,
  equals,
  subset,
  superset,
  cardinality
};

enum class DatatypeNumericOperationKind : u16 {
  canonicalize,
  add,
  subtract,
  multiply,
  divide,
  compare
};

enum class DatatypeRoundingMode : u16 {
  half_even,
  half_up,
  truncate
};

enum class DatatypeNullOrdering : u16 {
  nulls_first,
  nulls_last
};

struct DatatypeOperationValue {
  CanonicalTypeId type_id = CanonicalTypeId::unknown;
  std::string encoded_value;
  bool is_null = false;
};

struct DatatypeTextSeedAuthority {
  bool active = false;
  std::string seed_pack_name;
  std::string seed_pack_version;
  std::string charset_name;
  std::string collation_name;
  bool collation_case_insensitive = false;
  bool collation_accent_insensitive = false;
};

struct DatatypeCastRequest {
  DatatypeOperationValue value;
  CanonicalTypeId target_type_id = CanonicalTypeId::unknown;
  bool explicit_cast = false;
  bool reference_compatibility_profile = false;
};

struct DatatypeCastResult {
  Status status;
  DatatypeCastCategory category = DatatypeCastCategory::forbidden;
  DatatypeOperationValue value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeExtractRequest {
  DatatypeOperationValue value;
  std::string field;
};

struct DatatypeExtractResult {
  Status status;
  DatatypeOperationValue value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeSetDescriptor {
  CanonicalTypeId element_type_id = CanonicalTypeId::unknown;
  bool ordered = false;
  bool allow_null_elements = false;
  bool allow_duplicates = false;
};

struct DatatypeSetOperationRequest {
  DatatypeSetOperationKind operation = DatatypeSetOperationKind::membership;
  DatatypeSetDescriptor descriptor;
  std::string left_encoded_set;
  std::string right_encoded_set_or_value;
};

struct DatatypeSetOperationResult {
  Status status;
  DatatypeOperationValue value;
  std::string encoded_set;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeNumericContext {
  u32 precision = 38;
  u32 scale = 0;
  DatatypeRoundingMode rounding = DatatypeRoundingMode::half_even;
  bool allow_special_values = false;
};

struct DatatypeNumericOperationRequest {
  DatatypeNumericOperationKind operation = DatatypeNumericOperationKind::canonicalize;
  CanonicalTypeId type_id = CanonicalTypeId::decimal;
  DatatypeOperationValue left;
  DatatypeOperationValue right;
  DatatypeNumericContext context;
};

struct DatatypeNumericOperationResult {
  Status status;
  DatatypeOperationValue value;
  int comparison = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeComparisonRequest {
  DatatypeOperationValue left;
  DatatypeOperationValue right;
  DatatypeNullOrdering null_ordering = DatatypeNullOrdering::nulls_first;
  bool case_insensitive_character_compare = false;
  DatatypeTextSeedAuthority text_seed;
};

struct DatatypeComparisonResult {
  Status status;
  int comparison = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeSortKeyRequest {
  DatatypeOperationValue value;
  DatatypeNullOrdering null_ordering = DatatypeNullOrdering::nulls_first;
  bool case_insensitive_character_compare = false;
  DatatypeTextSeedAuthority text_seed;
};

struct DatatypeSortKeyResult {
  Status status;
  std::string sort_key;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeHashRequest {
  DatatypeOperationValue value;
};

struct DatatypeHashResult {
  Status status;
  std::string stable_hash_hex;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeSerializationRequest {
  DatatypeOperationValue value;
};

struct DatatypeSerializationResult {
  Status status;
  std::string serialized_value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeDeserializationRequest {
  CanonicalTypeId expected_type_id = CanonicalTypeId::unknown;
  std::string serialized_value;
};

struct DatatypeDeserializationResult {
  Status status;
  DatatypeOperationValue value;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeDisplayRenderRequest {
  DatatypeOperationValue value;
  bool export_literal = false;
  bool redact_opaque_payload = true;
};

struct DatatypeDisplayRenderResult {
  Status status;
  std::string canonical_type_name;
  std::string display_value;
  bool explicit_display_boundary = false;
  bool payload_redacted = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok();
  }
};

const char* DatatypeCastCategoryName(DatatypeCastCategory category);
const char* DatatypeSetOperationKindName(DatatypeSetOperationKind operation);
const char* DatatypeNumericOperationKindName(DatatypeNumericOperationKind operation);
const char* DatatypeRoundingModeName(DatatypeRoundingMode rounding);
const char* DatatypeNullOrderingName(DatatypeNullOrdering null_ordering);
DatatypeCastCategory ClassifyDatatypeCast(CanonicalTypeId source_type_id,
                                          CanonicalTypeId target_type_id,
                                          bool reference_compatibility_profile = false);
DatatypeCastResult CastDatatypeValue(const DatatypeCastRequest& request);
DatatypeExtractResult ExtractDatatypeField(const DatatypeExtractRequest& request);
DatatypeSetOperationResult EncodeSetValue(const DatatypeSetDescriptor& descriptor,
                                          const std::vector<DatatypeOperationValue>& values);
DatatypeSetOperationResult ApplySetOperation(const DatatypeSetOperationRequest& request);
DatatypeNumericOperationResult ApplyNumericOperation(const DatatypeNumericOperationRequest& request);
DatatypeComparisonResult CompareDatatypeValues(const DatatypeComparisonRequest& request);
DatatypeSortKeyResult MakeDatatypeSortKey(const DatatypeSortKeyRequest& request);
DatatypeHashResult HashDatatypeValue(const DatatypeHashRequest& request);
DatatypeSerializationResult SerializeDatatypeValue(const DatatypeSerializationRequest& request);
DatatypeDeserializationResult DeserializeDatatypeValue(const DatatypeDeserializationRequest& request);
DatatypeDisplayRenderResult RenderDatatypeValueForDisplay(
    const DatatypeDisplayRenderRequest& request);
CanonicalTypeId CanonicalTypeIdFromStableName(const std::string& stable_name);
bool IsUuidText(const std::string& value);
DiagnosticRecord MakeDatatypeOperationDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::core::datatypes
