// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_QUERY_EXPRESSION_API
struct EngineBindExpressionRequest : EngineApiRequest {};
struct EngineBindExpressionResult : EngineApiResult {};
EngineBindExpressionResult EngineBindExpression(const EngineBindExpressionRequest& request);

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
