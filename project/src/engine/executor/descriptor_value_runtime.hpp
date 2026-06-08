// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

// SEARCH_KEY: SB_EXEC_DESCRIPTOR_VALUE_RUNTIME_AUTHORITY
// Descriptor-bound tuple/batch runtime used by the executor. SQL names and
// parser syntax are not authority here; descriptors and encoded values are.

struct ExecutorColumnDescriptor {
  std::string stable_name;
  scratchbird::engine::internal_api::EngineDescriptor descriptor;
  bool nullable = true;
};

struct DescriptorTuple {
  std::vector<scratchbird::engine::internal_api::EngineTypedValue> values;
};

struct DescriptorBatch {
  std::vector<ExecutorColumnDescriptor> columns;
  std::vector<DescriptorTuple> rows;
};

struct DescriptorRuntimeDiagnostic {
  bool ok = true;
  std::string diagnostic_code = "SB_EXECUTOR_OK";
  std::string detail;
  std::size_t row_index = 0;
  std::size_t column_index = 0;
};

struct Int64DecodeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  std::int64_t value = 0;

  bool ok() const { return diagnostic.ok; }
};

struct BoolDecodeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  bool value = false;

  bool ok() const { return diagnostic.ok; }
};

struct Real64DecodeResult {
  DescriptorRuntimeDiagnostic diagnostic;
  double value = 0.0;

  bool ok() const { return diagnostic.ok; }
};

enum class DescriptorExpressionOperator {
  kInt64Add,
  kInt64Subtract,
  kInt64Multiply,
  kInt64Divide,
  kInt64Equal,
  kInt64GreaterThan,
  kReal64Add,
  kReal64Subtract,
  kReal64Multiply,
  kReal64Divide,
  kReal64Equal,
  kReal64GreaterThan,
  kBoolAnd,
  kBoolOr,
  kTextConcat,
  kTextEqual,
};

enum class DescriptorComparisonOperator {
  kEqual,
  kGreaterThan,
};

enum class DescriptorDomainMaskKind {
  kNone,
  kNull,
  kFixedText,
  kRevealLast4,
};

struct DescriptorRuntimeVariable {
  std::string stable_name;
  scratchbird::engine::internal_api::EngineTypedValue value;
};

struct DescriptorRuntimeSetScope {
  std::vector<DescriptorRuntimeVariable> variables;
};

struct DescriptorDomainPolicy {
  std::string domain_stable_name;
  scratchbird::engine::internal_api::EngineDescriptor base_descriptor;
  bool nullable = true;
  std::optional<std::int64_t> min_int64;
  std::optional<std::int64_t> max_int64;
  std::optional<std::size_t> max_text_bytes;
  DescriptorDomainMaskKind mask_kind = DescriptorDomainMaskKind::kNone;
  std::string fixed_mask_text;
  std::string required_security_token;
};

scratchbird::engine::internal_api::EngineDescriptor MakeExecutorDescriptor(std::string canonical_type_name,
                                                                           std::string encoded_descriptor = {});
scratchbird::engine::internal_api::EngineTypedValue MakeExecutorValue(
    const scratchbird::engine::internal_api::EngineDescriptor& descriptor,
    std::string encoded_value,
    bool is_null = false);
DescriptorBatch MakeDescriptorBatch(std::vector<ExecutorColumnDescriptor> columns,
                                    std::vector<DescriptorTuple> rows);
std::string DescriptorFingerprint(const std::vector<ExecutorColumnDescriptor>& columns);
bool DescriptorMatches(const scratchbird::engine::internal_api::EngineDescriptor& expected,
                       const scratchbird::engine::internal_api::EngineDescriptor& actual);
DescriptorRuntimeDiagnostic ValidateDescriptorBatch(const DescriptorBatch& batch);
std::optional<std::size_t> FindColumnByStableName(const DescriptorBatch& batch, const std::string& stable_name);
DescriptorBatch ProjectDescriptorBatch(const DescriptorBatch& input, const std::vector<std::size_t>& columns);
DescriptorBatch FilterDescriptorInt64GreaterThan(const DescriptorBatch& input,
                                                 std::size_t column,
                                                 std::int64_t threshold,
                                                 DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch FilterDescriptorBatchByComparison(
    const DescriptorBatch& input,
    std::size_t column,
    DescriptorComparisonOperator op,
    const scratchbird::engine::internal_api::EngineTypedValue& bound_value,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch SortDescriptorBatchByColumn(const DescriptorBatch& input,
                                            std::size_t column,
                                            bool ascending,
                                            DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch LimitOffsetDescriptorBatch(const DescriptorBatch& input,
                                           std::size_t limit,
                                           std::size_t offset);
DescriptorBatch SetUnionDistinctDescriptorBatch(const DescriptorBatch& left,
                                                const DescriptorBatch& right,
                                                DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch SetIntersectDistinctDescriptorBatch(const DescriptorBatch& left,
                                                    const DescriptorBatch& right,
                                                    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch SetExceptDistinctDescriptorBatch(const DescriptorBatch& left,
                                                 const DescriptorBatch& right,
                                                 DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch JoinDescriptorBatchesOnInt64(const DescriptorBatch& left,
                                             const DescriptorBatch& right,
                                             std::size_t left_column,
                                             std::size_t right_column,
                                             DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch JoinDescriptorBatchesOnEqual(const DescriptorBatch& left,
                                             const DescriptorBatch& right,
                                             std::size_t left_column,
                                             std::size_t right_column,
                                             DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch AggregateDescriptorCountByInt64(const DescriptorBatch& input,
                                                std::size_t group_column,
                                                std::string count_stable_name,
                                                DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch AggregateDescriptorCountByKey(const DescriptorBatch& input,
                                              std::size_t group_column,
                                              std::string count_stable_name,
                                              DescriptorRuntimeDiagnostic* diagnostic = nullptr);
DescriptorBatch WindowDescriptorRowNumberByInt64(const DescriptorBatch& input,
                                                 std::size_t order_column,
                                                 std::string row_number_stable_name,
                                                 bool ascending,
                                                 DescriptorRuntimeDiagnostic* diagnostic = nullptr);
scratchbird::engine::internal_api::EngineTypedValue EvaluateDescriptorExpression(
    DescriptorExpressionOperator op,
    const scratchbird::engine::internal_api::EngineTypedValue& left,
    const scratchbird::engine::internal_api::EngineTypedValue& right,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
scratchbird::engine::internal_api::EngineTypedValue EvaluateDescriptorCoalesce(
    const std::vector<scratchbird::engine::internal_api::EngineTypedValue>& values,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
scratchbird::engine::internal_api::EngineTypedValue CastDescriptorValue(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const scratchbird::engine::internal_api::EngineDescriptor& target_descriptor,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
scratchbird::engine::internal_api::EngineTypedValue ExtractDescriptorField(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const std::string& field_name,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
void SetDescriptorRuntimeVariable(DescriptorRuntimeSetScope* scope,
                                  std::string stable_name,
                                  scratchbird::engine::internal_api::EngineTypedValue value);
std::optional<scratchbird::engine::internal_api::EngineTypedValue> GetDescriptorRuntimeVariable(
    const DescriptorRuntimeSetScope& scope,
    const std::string& stable_name);
DescriptorRuntimeDiagnostic ValidateDescriptorDomainValue(
    const DescriptorDomainPolicy& policy,
    const scratchbird::engine::internal_api::EngineTypedValue& value);
scratchbird::engine::internal_api::EngineTypedValue ApplyDescriptorDomainMask(
    const DescriptorDomainPolicy& policy,
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const std::string& security_token,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
scratchbird::engine::internal_api::EngineTypedValue EvaluateDescriptorDomainMethod(
    const DescriptorDomainPolicy& policy,
    const std::string& method_name,
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const std::string& security_token,
    DescriptorRuntimeDiagnostic* diagnostic = nullptr);
Int64DecodeResult DecodeInt64Value(const scratchbird::engine::internal_api::EngineTypedValue& value);
BoolDecodeResult DecodeBoolValue(const scratchbird::engine::internal_api::EngineTypedValue& value);
Real64DecodeResult DecodeReal64Value(const scratchbird::engine::internal_api::EngineTypedValue& value);
scratchbird::engine::internal_api::EngineTypedValue EncodeInt64Value(std::int64_t value);
scratchbird::engine::internal_api::EngineTypedValue EncodeBoolValue(bool value);
scratchbird::engine::internal_api::EngineTypedValue EncodeReal64Value(double value);
scratchbird::engine::internal_api::EngineTypedValue EncodeTextValue(std::string value);

}  // namespace scratchbird::engine::executor
