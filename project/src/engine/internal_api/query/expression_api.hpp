// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "datatype_operations.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::core::datatypes {
struct TimezoneSeedAuthority;
}

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_EXPRESSION_API
struct EngineBindExpressionRequest : EngineApiRequest {};
struct EngineBindExpressionResult : EngineApiResult {
  EngineObjectReference bound_reference;
  EngineBoundObjectIdentity bound_identity;
  EngineDescriptor bound_descriptor;
};
EngineBindExpressionResult EngineBindExpression(const EngineBindExpressionRequest& request);

enum class EngineSqlTruthValue : std::uint8_t {
  unspecified = 0,
  false_value,
  true_value,
  unknown,
};

enum class EngineComparisonPredicateOperator : std::uint8_t {
  unspecified = 0,
  equal,
  not_equal,
  less_than,
  less_than_or_equal,
  greater_than,
  greater_than_or_equal,
  is_null,
  is_not_null,
  logical_not,
  logical_and,
  logical_or,
};

enum class EnginePredicateConsumer : std::uint8_t {
  unspecified = 0,
  filter,
  join_on,
  having,
  qualify,
};

// RCP-023-CANONICAL-TYPED-EXPRESSION-RUNTIME-V1
// The consumer identifies the physical expression seam, never the expression
// semantics. All consumers below execute the same descriptor, NULL, numeric,
// comparison, and truth-state rules.
enum class EngineCanonicalExpressionConsumer : std::uint8_t {
  unspecified = 0,
  filter,
  projection,
  join,
  aggregate,
  window,
  subquery,
};

enum class EngineCanonicalExpressionOperation : std::uint8_t {
  unspecified = 0,
  identity,
  consume_truth,
  numeric_add,
  numeric_subtract,
  numeric_multiply,
  numeric_divide,
  text_concat,
  equal,
  not_equal,
  less_than,
  less_than_or_equal,
  greater_than,
  greater_than_or_equal,
  is_null,
  is_not_null,
  logical_not,
  logical_and,
  logical_or,
  // RCP-024 appends operations so the RCP-023 ordinal contract remains
  // stable for every pre-existing consumer.
  explicit_cast,
  implicit_cast,
  numeric_modulo,
  like,
  ilike,
  is_distinct_from,
  is_not_distinct_from,
  logical_xor,
  scalar_function,
};

struct EngineCanonicalExpressionEvaluationRequest {
  EngineCanonicalExpressionConsumer consumer =
      EngineCanonicalExpressionConsumer::unspecified;
  EngineCanonicalExpressionOperation operation =
      EngineCanonicalExpressionOperation::unspecified;
  EngineTypedValue left_value;
  EngineTypedValue right_value;
  EngineDescriptor result_descriptor;
  EngineSqlTruthValue input_truth = EngineSqlTruthValue::unspecified;
  scratchbird::core::datatypes::DatatypeNumericContext numeric_context;
  std::optional<int> precomputed_comparison;
  std::optional<EngineTypedValue> precomputed_value;
  bool bound_text_authority = false;
};

struct EngineCanonicalExpressionEvaluationResult {
  EngineTypedValue value;
  EngineSqlTruthValue truth = EngineSqlTruthValue::unspecified;
  int comparison = 0;
  bool passes_consumer = false;
  // Stable refusal identity for the selected canonical operation. This is
  // populated before evaluation so all failure exits remain deterministic.
  std::string diagnostic_id;
};

// Descriptor/scalar primitives are implemented by the lower expression
// contract target so both the internal API and physical executor consume the
// same runtime without a static-library dependency cycle.
bool QowCanonicalDescriptorIdentityV1(
    const EngineDescriptor& descriptor);
scratchbird::core::datatypes::CanonicalTypeId
QowCanonicalTypeFromDescriptorV1(const EngineDescriptor& descriptor);
EngineTypedValue QowPreserveCanonicalDescriptorAfterScalarV1(
    const EngineDescriptor& result_descriptor,
    EngineTypedValue computed_value);
bool QowCanonicalSqlNullStateV1(const EngineTypedValue& value);
EngineTypedValue QowPropagateSqlNullAfterScalarV1(
    const EngineDescriptor& result_descriptor,
    EngineTypedValue computed_value);
bool QowApplyCanonicalDescriptorCoercionV1(
    const EngineTypedValue& input_value,
    const EngineDescriptor& target_descriptor,
    bool explicit_cast,
    EngineTypedValue* output_value,
    std::string* cast_category,
    std::string* refusal_detail);
bool QowPreserveInvalidDescriptorStateAndCoerceV1(
    const EngineTypedValue& input_value,
    const EngineDescriptor& target_descriptor,
    bool explicit_cast,
    EngineTypedValue* output_value,
    std::string* cast_category,
    std::string* refusal_reason,
    std::string* refusal_detail);
bool QowCompareCanonicalCollatedScalarsV1(
    const EngineTypedValue& left_value,
    const EngineTypedValue& right_value,
    const std::string& collation_uuid,
    EngineApiU64 resource_epoch,
    EngineApiU64 collation_epoch,
    const scratchbird::core::datatypes::DatatypeTextSeedAuthority& text_seed,
    int* comparison,
    std::string* refusal_detail);
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
bool QowCanonicalDescriptorU32FieldV1(
    const std::string& descriptor,
    const std::string& key,
    std::uint32_t* value);
bool QowBindCanonicalExpressionReferenceV1(
    const EngineBindExpressionRequest& request,
    EngineObjectReference* bound_reference,
    EngineDescriptor* bound_descriptor,
    std::string* refusal_reason,
    std::string* refusal_detail);

bool QowCanonicalTruthFromTypedValueV1(
    const EngineTypedValue& value,
    EngineSqlTruthValue* truth,
    std::string* refusal_detail);
bool QowCompareCanonicalNonCollatedScalarsV1(
    const EngineTypedValue& left_value,
    const EngineTypedValue& right_value,
    int* comparison,
    std::string* refusal_detail);
bool QowEvaluateCanonicalTypedExpressionV1(
    const EngineCanonicalExpressionEvaluationRequest& request,
    EngineCanonicalExpressionEvaluationResult* result,
    std::string* refusal_detail);
bool QowCanonicalExpressionConsumerPassesV1(
    EngineCanonicalExpressionConsumer consumer,
    EngineSqlTruthValue truth_value,
    bool* passes,
    std::string* refusal_detail);

const char* EngineSqlTruthValueName(EngineSqlTruthValue value) noexcept;
bool QowCanonicalTruthValueV1(EngineSqlTruthValue value) noexcept;
EngineSqlTruthValue QowSqlNotV1(EngineSqlTruthValue value) noexcept;
EngineSqlTruthValue QowSqlAndV1(EngineSqlTruthValue left,
                                EngineSqlTruthValue right) noexcept;
EngineSqlTruthValue QowSqlOrV1(EngineSqlTruthValue left,
                               EngineSqlTruthValue right) noexcept;
bool QowEvaluateCanonicalComparisonTruthV1(
    const EngineTypedValue& left_value,
    const EngineTypedValue& right_value,
    int comparison,
    EngineComparisonPredicateOperator operation,
    EngineSqlTruthValue* truth_value,
    std::string* refusal_detail);
bool QowEvaluateCanonicalNullPredicateV1(
    const EngineTypedValue& value,
    bool negate,
    EngineSqlTruthValue* truth_value,
    std::string* refusal_detail);
bool QowCompareCanonicalNonCollatedScalarsV1(
    const EngineTypedValue& left_value,
    const EngineTypedValue& right_value,
    int* comparison,
    std::string* refusal_detail);
bool QowMaterializeCanonicalTruthValueV1(
    EngineSqlTruthValue truth_value,
    const EngineDescriptor& result_descriptor,
    EngineTypedValue* output_value,
    std::string* refusal_detail);
bool QowApplyCanonicalNumericScalarV1(
    const EngineTypedValue& left_value,
    const EngineTypedValue& right_value,
    const EngineDescriptor& result_descriptor,
    scratchbird::core::datatypes::DatatypeNumericOperationKind operation,
    const scratchbird::core::datatypes::DatatypeNumericContext& context,
    EngineTypedValue* output_value,
    std::string* refusal_detail);
inline bool QowPredicateConsumerPassesV1(
    const EngineSqlTruthValue truth_value,
    const EnginePredicateConsumer consumer,
    bool* passes,
    std::string* refusal_detail) {
  if (passes == nullptr || refusal_detail == nullptr) return false;
  *passes = false;
  refusal_detail->clear();
  EngineCanonicalExpressionConsumer canonical_consumer =
      EngineCanonicalExpressionConsumer::unspecified;
  if (consumer == EnginePredicateConsumer::filter) {
    canonical_consumer = EngineCanonicalExpressionConsumer::filter;
  } else if (consumer == EnginePredicateConsumer::join_on) {
    canonical_consumer = EngineCanonicalExpressionConsumer::join;
  } else if (consumer == EnginePredicateConsumer::having) {
    canonical_consumer = EngineCanonicalExpressionConsumer::aggregate;
  } else if (consumer == EnginePredicateConsumer::qualify) {
    canonical_consumer = EngineCanonicalExpressionConsumer::window;
  } else {
    *refusal_detail = "predicate consumer or truth value is not bound";
    return false;
  }
  return QowCanonicalExpressionConsumerPassesV1(
      canonical_consumer, truth_value, passes, refusal_detail);
}

struct EngineCastValueRequest : EngineApiRequest {
  EngineTypedValue input_value;
  EngineDescriptor target_descriptor;
  bool explicit_cast = true;
};
struct EngineCastValueResult : EngineApiResult {
  EngineTypedValue value;
  std::string cast_category;
};
EngineCastValueResult EngineCastValue(const EngineCastValueRequest& request);

struct EngineCompareScalarValuesRequest : EngineApiRequest {
  EngineTypedValue left_value;
  EngineTypedValue right_value;
};
struct EngineCompareScalarValuesResult : EngineApiResult {
  int comparison = 0;
  EngineUuid collation_uuid;
  EngineApiU64 collation_epoch = 0;
};
EngineCompareScalarValuesResult EngineCompareScalarValues(
    const EngineCompareScalarValuesRequest& request);

struct EngineNormalizeTimezoneScalarRequest : EngineApiRequest {
  EngineTypedValue input_value;
};
struct EngineNormalizeTimezoneScalarResult : EngineApiResult {
  EngineTypedValue value;
  std::string timezone_identifier;
  int timezone_offset_minutes = 0;
  bool used_timezone_seed = false;
  EngineApiU64 timezone_epoch = 0;
};
EngineNormalizeTimezoneScalarResult EngineNormalizeTimezoneScalar(
    const EngineNormalizeTimezoneScalarRequest& request);

struct EngineExtractValueRequest : EngineApiRequest {
  EngineTypedValue input_value;
  std::string field;
};
struct EngineExtractValueResult : EngineApiResult {
  EngineTypedValue value;
};
EngineExtractValueResult EngineExtractValue(const EngineExtractValueRequest& request);

struct EngineSetOperationRequest : EngineApiRequest {
  std::string set_operation;
  EngineTypedValue left_set;
  EngineTypedValue right_set_or_value;
};
struct EngineSetOperationResult : EngineApiResult {
  EngineTypedValue value;
};
EngineSetOperationResult EngineSetOperation(const EngineSetOperationRequest& request);

struct EngineApplyNumericOperationRequest : EngineApiRequest {
  std::string numeric_operation;
  EngineTypedValue left_value;
  EngineTypedValue right_value;
  std::uint32_t precision = 38;
  std::uint32_t scale = 0;
  std::string rounding_mode;
  bool allow_special_values = false;
};
struct EngineApplyNumericOperationResult : EngineApiResult {
  EngineTypedValue value;
  int comparison = 0;
};
EngineApplyNumericOperationResult EngineApplyNumericOperation(const EngineApplyNumericOperationRequest& request);

struct EngineCanonicalizeDocumentValueRequest : EngineApiRequest {
  EngineTypedValue input_value;
  std::string reference_profile;
  bool allow_hstore_domain = false;
};
struct EngineCanonicalizeDocumentValueResult : EngineApiResult {
  EngineTypedValue value;
  std::string canonical_format;
};
EngineCanonicalizeDocumentValueResult EngineCanonicalizeDocumentValue(const EngineCanonicalizeDocumentValueRequest& request);

struct EngineEvaluateAdvancedDatatypeFamilyRequest : EngineApiRequest {
  EngineDescriptor descriptor;
  std::string operation_kind;
  std::string index_kind;
  std::string descriptor_profile;
  std::uint32_t vector_dimension = 0;
};
struct EngineEvaluateAdvancedDatatypeFamilyResult : EngineApiResult {
  std::string family;
  bool descriptor_supported = false;
  bool operation_supported = false;
  bool index_supported = false;
  bool optimizer_admitted = false;
  bool compare_supported = false;
  bool hash_supported = false;
  std::string canonical_descriptor_profile;
  std::vector<std::string> required_descriptor_fields;
  std::string compare_hash_refusal_detail;
  std::string optimizer_support_path;
};
EngineEvaluateAdvancedDatatypeFamilyResult EngineEvaluateAdvancedDatatypeFamily(
    const EngineEvaluateAdvancedDatatypeFamilyRequest& request);

struct EngineValidateDomainValueRequest : EngineApiRequest {
  EngineDescriptor domain_descriptor;
  EngineTypedValue input_value;
};
struct EngineValidateDomainValueResult : EngineApiResult {
  EngineTypedValue value;
};
EngineValidateDomainValueResult EngineValidateDomainValue(const EngineValidateDomainValueRequest& request);

struct EngineInvokeDomainMethodRequest : EngineApiRequest {
  EngineDescriptor domain_descriptor;
  EngineTypedValue input_value;
  std::string method_name;
};
struct EngineInvokeDomainMethodResult : EngineApiResult {
  EngineTypedValue value;
};
EngineInvokeDomainMethodResult EngineInvokeDomainMethod(const EngineInvokeDomainMethodRequest& request);

}  // namespace scratchbird::engine::internal_api
