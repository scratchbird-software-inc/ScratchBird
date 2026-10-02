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
#include "../resources/collation_profile.hpp"
#include "../resources/unicode_collation.hpp"

#include <cstdint>
#include <string>
#include <string_view>
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

enum class DatatypeCastContext : u16 {
  implicit,
  assignment,
  explicit_cast
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
  // Required for every concrete typed SQL NULL. A default descriptor remains
  // available only for the process-local null_type sentinel and legacy
  // non-NULL callers while downstream owners migrate their bindings.
  scratchbird::engine::ExecutionTypeDescriptor descriptor;
};

struct DatatypeTextSeedAuthority {
  bool active = false;
  platform::Uuid database_uuid, charset_uuid, collation_uuid;
  std::uint64_t resource_epoch = 0, collation_epoch = 0;
  resources::CollationProfile comparison_profile = resources::CollationProfile::unbound;
  std::shared_ptr<const resources::UnicodeCollationData> unicode_collation;
  std::string seed_pack_name;
  std::string seed_pack_version;
  std::string charset_name;
  std::string collation_name;
  bool collation_case_insensitive = false;
  bool collation_accent_insensitive = false;
};

struct DatatypeNumericContext {
  u32 precision = 38;
  u32 scale = 0;
  DatatypeRoundingMode rounding = DatatypeRoundingMode::half_even;
  bool allow_special_values = false;
};

struct DatatypeNumericFacts {
  bool inexact = false;
  bool underflow = false;
  bool overflow = false;
  bool invalid = false;
  bool divide_by_zero = false;
  bool subnormal = false;
  bool unordered = false;
};

struct DatatypeCastRequest {
  DatatypeOperationValue value;
  CanonicalTypeId target_type_id = CanonicalTypeId::unknown;
  DatatypeCastContext context = DatatypeCastContext::implicit;
  // Compatibility input for existing callers. New callers use context.
  bool explicit_cast = false;
  bool reference_compatibility_profile = false;
  // Execution context supplied by the bound owner, not descriptor authority.
  DatatypeNumericContext numeric_context;
  scratchbird::engine::ExecutionTypeDescriptor target_descriptor;
};

struct DatatypeCastResult {
  Status status;
  DatatypeCastCategory category = DatatypeCastCategory::forbidden;
  DatatypeOperationValue value;
  DiagnosticRecord diagnostic;
  DatatypeNumericFacts numeric_facts;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeExtractRequest {
  DatatypeOperationValue value;
  std::string field;
  scratchbird::engine::ExecutionTypeDescriptor result_descriptor;
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
  // Required when NULL elements are admitted. This is the bound element
  // descriptor; the set codec is not descriptor authority and must not
  // reconstruct it from the canonical type name.
  scratchbird::engine::ExecutionTypeDescriptor element_descriptor;
  bool ordered = false;
  bool allow_null_elements = false;
  bool allow_duplicates = false;
};

struct DatatypeSetOperationRequest {
  DatatypeSetOperationKind operation = DatatypeSetOperationKind::membership;
  DatatypeSetDescriptor descriptor;
  std::string left_encoded_set;
  DatatypeOperationValue right_value;
  std::string right_encoded_set;
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

struct DatatypeNumericOperationRequest {
  DatatypeNumericOperationKind operation = DatatypeNumericOperationKind::canonicalize;
  CanonicalTypeId type_id = CanonicalTypeId::decimal;
  DatatypeOperationValue left;
  DatatypeOperationValue right;
  DatatypeNumericContext context;
  scratchbird::engine::ExecutionTypeDescriptor result_descriptor;
};

struct DatatypeNumericOperationResult {
  Status status;
  DatatypeOperationValue value;
  int comparison = 0;
  DiagnosticRecord diagnostic;
  DatatypeNumericFacts numeric_facts;

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
  // Execution mechanism only: the caller retains descriptor/context authority.
  DatatypeNumericContext numeric_context;
};

struct DatatypeComparisonResult {
  Status status;
  int comparison = 0;
  DiagnosticRecord diagnostic;
  DatatypeNumericFacts numeric_facts;

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
  DatatypeTextSeedAuthority text_seed;
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
  // Descriptor authority remains separate from unchanged value framing.
  scratchbird::engine::ExecutionTypeDescriptor descriptor;

  bool ok() const {
    return status.ok();
  }
};

struct DatatypeDeserializationRequest {
  CanonicalTypeId expected_type_id = CanonicalTypeId::unknown;
  std::string serialized_value;
  scratchbird::engine::ExecutionTypeDescriptor expected_descriptor;
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
const char* DatatypeCastContextName(DatatypeCastContext context);
const char* DatatypeSetOperationKindName(DatatypeSetOperationKind operation);
const char* DatatypeNumericOperationKindName(DatatypeNumericOperationKind operation);
const char* DatatypeRoundingModeName(DatatypeRoundingMode rounding);
const char* DatatypeNullOrderingName(DatatypeNullOrdering null_ordering);
// Canonical INT8 value boundary. Present values are exactly one native byte;
// decimal text exists only at parser, SBLR, Engine, and display boundaries.
bool EncodeCanonicalInt8Value(std::string_view decimal_text,
                              std::string* canonical_bytes);
bool DecodeCanonicalInt8Value(std::string_view canonical_bytes,
                              std::string* decimal_text);
// Canonical UINT8 value boundary. Present values are exactly one unsigned byte;
// decimal text exists only at parser, SBLR, Engine, and display boundaries.
bool EncodeCanonicalUint8Value(std::string_view decimal_text,
                               std::string* canonical_bytes);
bool DecodeCanonicalUint8Value(std::string_view canonical_bytes,
                               std::string* decimal_text);
// Canonical INT16 value boundary. Present values are exactly two
// little-endian two's-complement bytes. This bridge is mathematical and does
// not define a text, parser, display, or cast policy.
bool EncodeCanonicalInt16Value(std::int64_t value,
                               std::string* canonical_bytes);
bool DecodeCanonicalInt16Value(std::string_view canonical_bytes,
                               std::int64_t* value);
// Canonical UINT16 value boundary. Present values are exactly two
// little-endian unsigned bytes. This bridge is mathematical and does not
// define a text, parser, display, or cast policy.
bool EncodeCanonicalUint16Value(std::int64_t value,
                                std::string* canonical_bytes);
bool DecodeCanonicalUint16Value(std::string_view canonical_bytes,
                                std::uint64_t* value);
// Canonical INT32 value boundary. Present values are exactly four
// little-endian two's-complement bytes. This bridge is mathematical and does
// not define a text, parser, display, or cast policy.
bool EncodeCanonicalInt32Value(std::int64_t value,
                               std::string* canonical_bytes);
bool DecodeCanonicalInt32Value(std::string_view canonical_bytes,
                               std::int64_t* value);
// Canonical UINT32 value boundary. Present values are exactly four
// little-endian unsigned bytes. This bridge is mathematical and does not
// define a text, parser, display, or cast policy.
bool EncodeCanonicalUint32Value(std::uint64_t value,
                                std::string* canonical_bytes);
bool DecodeCanonicalUint32Value(std::string_view canonical_bytes,
                                std::uint64_t* value);
// Canonical INT64 value boundary. Present values are exactly eight
// little-endian two's-complement bytes. This bridge is mathematical and does
// not define a text, parser, display, or cast policy.
bool EncodeCanonicalInt64Value(std::int64_t value,
                               std::string* canonical_bytes);
bool DecodeCanonicalInt64Value(std::string_view canonical_bytes,
                               std::int64_t* value);
// Canonical UINT64 value boundary. Present values are exactly eight
// little-endian unsigned bytes. This bridge is mathematical and does not
// define a text, parser, display, or cast policy.
bool EncodeCanonicalUint64Value(std::uint64_t value,
                                std::string* canonical_bytes);
bool DecodeCanonicalUint64Value(std::string_view canonical_bytes,
                                std::uint64_t* value);
// Canonical INT128 value boundary. Present values are exactly sixteen
// little-endian two's-complement bytes. Canonical decimal text exists only at
// this sbl_numeric-backed conversion boundary; DatatypeOperationValue carries
// the resulting binary bytes.
bool EncodeCanonicalInt128Value(std::string_view canonical_decimal,
                                std::string* canonical_bytes);
bool DecodeCanonicalInt128Value(std::string_view canonical_bytes,
                                std::string* canonical_decimal);
// Exact bulk-import converter 019d...b775 generation 1. This accepts only the
// lane's canonical decimal grammar and is not a general parser, cast, or
// display policy.
bool EncodeCanonicalInt32BulkImportTextV1(
    std::string_view decimal_text,
    std::string* canonical_bytes);
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
