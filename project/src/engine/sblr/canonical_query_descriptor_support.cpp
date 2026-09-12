// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_descriptor_support.hpp"

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_DESCRIPTOR_SUPPORT_AUTHORITY
std::optional<std::string> ExactEncodedDescriptorField(
    const std::string_view descriptor,
    const std::string_view key) {
  const std::string prefix = std::string(key) + "=";
  std::optional<std::string> value;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const auto end = descriptor.find(';', start);
    const auto field = descriptor.substr(
        start, end == std::string_view::npos ? std::string_view::npos
                                             : end - start);
    if (field.starts_with(prefix)) {
      if (value.has_value()) return std::nullopt;
      value = std::string(field.substr(prefix.size()));
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  if (value.has_value() && value->empty()) return std::nullopt;
  return value;
}

exec::CanonicalResultNullability ResultNullability(
    const api::RelationalNullability nullability) {
  switch (nullability) {
    case api::RelationalNullability::kNonNull:
      return exec::CanonicalResultNullability::kNonNull;
    case api::RelationalNullability::kNullable:
      return exec::CanonicalResultNullability::kNullable;
    case api::RelationalNullability::kUnknown:
      return exec::CanonicalResultNullability::kUnknown;
  }
  return exec::CanonicalResultNullability::kUnknown;
}

bool SameExactEngineDescriptorV1(const api::EngineDescriptor& left,
                                 const api::EngineDescriptor& right) {
  return left == right;
}

bool SameExactRelationalTypeDescriptorV2(
    const api::RelationalTypeDescriptor& left,
    const api::RelationalTypeDescriptor& right) {
  return left.descriptor_id == right.descriptor_id &&
         left.descriptor_uuid == right.descriptor_uuid &&
         left.type_uuid == right.type_uuid &&
         left.nullability == right.nullability &&
         left.collation_uuid == right.collation_uuid &&
         left.timezone_profile_id == right.timezone_profile_id &&
         left.width == right.width && left.precision == right.precision &&
         left.scale == right.scale &&
         left.datatype_identity_authoritative ==
             right.datatype_identity_authoritative &&
         left.descriptor_generation == right.descriptor_generation &&
         left.type_generation == right.type_generation &&
         left.codec_id == right.codec_id &&
         left.codec_version == right.codec_version &&
         left.codec_generation == right.codec_generation &&
         left.statement_receipt_uuid == right.statement_receipt_uuid &&
         left.datatype_catalog_snapshot_uuid ==
             right.datatype_catalog_snapshot_uuid &&
         left.datatype_catalog_generation ==
             right.datatype_catalog_generation &&
         left.datatype_registry_generation ==
             right.datatype_registry_generation;
}

bool CanonicalQueryEngineDescriptorExactlyEqual(
    const api::EngineDescriptor& left,
    const api::EngineDescriptor& right) {
  return SameExactEngineDescriptorV1(left, right);
}

bool CanonicalQueryTypedValuePayloadExactlyEqual(
    const api::EngineTypedValue& left,
    const api::EngineTypedValue& right) {
  return left.encoded_value == right.encoded_value &&
         left.binary_value == right.binary_value &&
         left.is_null == right.is_null && left.state == right.state;
}

bool CanonicalQueryDescriptorTuplePayloadExactlyEqual(
    const exec::DescriptorTuple& left,
    const exec::DescriptorTuple& right) {
  if (left.values.size() != right.values.size()) return false;
  for (std::size_t index = 0; index < left.values.size(); ++index) {
    if (!CanonicalQueryTypedValuePayloadExactlyEqual(
            left.values[index], right.values[index])) {
      return false;
    }
  }
  return true;
}

bool CanonicalQueryDescriptorBatchesExactlyEqual(
    const exec::DescriptorBatch& left,
    const exec::DescriptorBatch& right) {
  if (left.columns.size() != right.columns.size() ||
      left.rows.size() != right.rows.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.columns.size(); ++index) {
    const auto& left_column = left.columns[index];
    const auto& right_column = right.columns[index];
    if (left_column.stable_name != right_column.stable_name ||
        left_column.nullable != right_column.nullable ||
        left_column.descriptor_id != right_column.descriptor_id ||
        !CanonicalQueryEngineDescriptorExactlyEqual(
            left_column.descriptor, right_column.descriptor)) {
      return false;
    }
  }
  for (std::size_t row_index = 0; row_index < left.rows.size(); ++row_index) {
    const auto& left_values = left.rows[row_index].values;
    const auto& right_values = right.rows[row_index].values;
    if (left_values.size() != right_values.size()) return false;
    for (std::size_t value_index = 0; value_index < left_values.size();
         ++value_index) {
      if (!CanonicalQueryEngineDescriptorExactlyEqual(
              left_values[value_index].descriptor,
              right_values[value_index].descriptor) ||
          !CanonicalQueryTypedValuePayloadExactlyEqual(
              left_values[value_index], right_values[value_index])) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace scratchbird::engine::sblr
