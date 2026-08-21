// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"
#include "hash_digest.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic Refusal(std::string code,
                                    std::string detail = {}) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

bool DescriptorBatchCarrierIsExactDefault(const DescriptorBatch& batch) {
  const DescriptorBatch empty;
  return batch.columns.empty() &&
         batch.columns.capacity() == empty.columns.capacity() &&
         batch.rows.empty() && batch.rows.capacity() == empty.rows.capacity();
}

bool DescriptorOrderTermsCarrierIsExactDefault(
    const std::vector<CanonicalDescriptorOrderTerm>& order_terms) {
  const std::vector<CanonicalDescriptorOrderTerm> empty;
  return order_terms.empty() && order_terms.capacity() == empty.capacity();
}

bool StringCarrierIsExactDefault(const std::string& value) {
  const std::string empty;
  return value.empty() && value.capacity() == empty.capacity();
}

bool CanonicalExecutionMgaAuthorityCarrierIsExactDefault(
    const CanonicalExecutionMgaAuthority& authority) {
  const CanonicalExecutionMgaAuthority empty;
  const auto exact_empty_string = [](const std::string& value,
                                     const std::string& baseline) {
    return value.empty() && value.capacity() == baseline.capacity();
  };
  const auto& context = authority.statement_context;
  const auto& empty_context = empty.statement_context;
  return authority.origin == empty.origin && !authority.resolve_current &&
         PhysicalMgaStatementContextEqual(context, empty_context) &&
         exact_empty_string(context.statement_uuid,
                            empty_context.statement_uuid) &&
         exact_empty_string(context.owning_transaction_uuid,
                            empty_context.owning_transaction_uuid) &&
         exact_empty_string(context.statement_snapshot_uuid,
                            empty_context.statement_snapshot_uuid) &&
         exact_empty_string(context.statement_metadata_snapshot_uuid,
                            empty_context.statement_metadata_snapshot_uuid) &&
         exact_empty_string(context.snapshot_kind,
                            empty_context.snapshot_kind) &&
         exact_empty_string(context.statement_timestamp,
                            empty_context.statement_timestamp) &&
         context.active_excluded_local_transaction_ids.empty() &&
         context.active_excluded_local_transaction_ids.capacity() ==
             empty_context.active_excluded_local_transaction_ids.capacity() &&
         context.in_doubt_excluded_local_transaction_ids.empty() &&
         context.in_doubt_excluded_local_transaction_ids.capacity() ==
             empty_context.in_doubt_excluded_local_transaction_ids.capacity();
}

bool SortReceiptRequestCarriersAreExactDefault(
    const CanonicalDescriptorSortRequest& request) {
  const CanonicalDescriptorSortRequest empty;
  return TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) &&
         request.selected_physical_node_id ==
             empty.selected_physical_node_id &&
         DescriptorBatchCarrierIsExactDefault(request.input_batch) &&
         DescriptorOrderTermsCarrierIsExactDefault(request.order_terms) &&
         StringCarrierIsExactDefault(
             request.deterministic_tie_evidence_uuid) &&
         request.maximum_pair_comparisons ==
             empty.maximum_pair_comparisons &&
         CanonicalExecutionMgaAuthorityCarrierIsExactDefault(
             request.mga_authority);
}

bool CanonicalDescriptorBatchMemoryBytes(const DescriptorBatch& batch,
                                         std::uint64_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 1;
  for (const auto& row : batch.rows) {
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.binary_value.size();
    }
  }
  return true;
}

bool CanonicalDescriptorBatchRangeMemoryBytes(
    const DescriptorBatch& batch,
    const std::size_t first_row,
    const std::size_t row_count,
    std::uint64_t* bytes) {
  if (bytes == nullptr || first_row > batch.rows.size() ||
      row_count > batch.rows.size() - first_row) {
    return false;
  }
  *bytes = 1;
  for (std::size_t row_index = first_row;
       row_index < first_row + row_count; ++row_index) {
    for (const auto& value : batch.rows[row_index].values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.binary_value.size();
    }
  }
  return true;
}

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

class OrderTermBindingEncoder {
 public:
  OrderTermBindingEncoder(
      std::vector<scratchbird::core::platform::byte>* payload,
      const std::uint64_t maximum_bytes)
      : payload_(payload), maximum_bytes_(maximum_bytes) {}

  bool AppendUint64(const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      if (!AppendByte(static_cast<scratchbird::core::platform::byte>(
              value >> shift))) {
        return false;
      }
    }
    return true;
  }

  bool AppendBool(const bool value) {
    return AppendByte(value ? 1 : 0);
  }

  bool AppendString(const std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint64_t>::max() ||
        !AppendUint64(static_cast<std::uint64_t>(value.size()))) {
      return false;
    }
    for (const auto ch : value) {
      if (!AppendByte(
              static_cast<scratchbird::core::platform::byte>(ch))) {
        return false;
      }
    }
    return true;
  }

  std::uint64_t bytes() const { return bytes_; }

 private:
  bool AppendByte(const scratchbird::core::platform::byte value) {
    if (bytes_ >= maximum_bytes_ ||
        (payload_ != nullptr &&
         (bytes_ > std::numeric_limits<std::size_t>::max() ||
          bytes_ >= payload_->size()))) {
      return false;
    }
    if (payload_ != nullptr) {
      (*payload_)[static_cast<std::size_t>(bytes_)] = value;
    }
    ++bytes_;
    return true;
  }

  std::vector<scratchbird::core::platform::byte>* payload_;
  std::uint64_t maximum_bytes_;
  std::uint64_t bytes_{0};
};

bool EncodeOrderTermBindingFields(
    OrderTermBindingEncoder* encoder,
    const CanonicalDescriptorOrderTerm& term,
    const std::string_view ordering_property_uuid) {
  if (encoder == nullptr ||
      !encoder->AppendString(
          "scratchbird.order-term-binding-evidence.v1") ||
      !encoder->AppendString(ordering_property_uuid) ||
      !encoder->AppendUint64(term.column) ||
      !encoder->AppendUint64(term.expression_descriptor_id) ||
      !encoder->AppendUint64(static_cast<std::uint8_t>(term.direction)) ||
      !encoder->AppendUint64(
          static_cast<std::uint8_t>(term.null_placement)) ||
      !encoder->AppendString(term.collation_uuid) ||
      !encoder->AppendUint64(term.resource_epoch) ||
      !encoder->AppendUint64(term.collation_epoch) ||
      !encoder->AppendBool(term.text_seed.active) ||
      !encoder->AppendString(term.text_seed.seed_pack_name) ||
      !encoder->AppendString(term.text_seed.seed_pack_version) ||
      !encoder->AppendString(term.text_seed.charset_name) ||
      !encoder->AppendString(term.text_seed.collation_name) ||
      !encoder->AppendBool(
          term.text_seed.collation_case_insensitive) ||
      !encoder->AppendBool(
          term.text_seed.collation_accent_insensitive) ||
      !encoder->AppendUint64(term.timezone_epoch) ||
      !encoder->AppendBool(term.timezone_seed.active) ||
      !encoder->AppendString(term.timezone_seed.seed_pack_name) ||
      !encoder->AppendString(term.timezone_seed.seed_pack_version) ||
      !encoder->AppendString(term.timezone_seed.content_hash) ||
      !encoder->AppendUint64(term.timezone_seed.timezone_records) ||
      !encoder->AppendUint64(
          term.timezone_seed.timezone_transition_records) ||
      !encoder->AppendUint64(
          term.timezone_seed.timezone_leap_second_records) ||
      !encoder->AppendUint64(term.timezone_seed.timezone_names.size())) {
    return false;
  }
  return std::ranges::all_of(
      term.timezone_seed.timezone_names,
      [&](const auto& name) { return encoder->AppendString(name); });
}

std::string OrderTermBindingUuidFromSha256(
    const scratchbird::core::hash::Digest256& digest) {
  std::array<scratchbird::core::platform::byte, 16> bytes{};
  std::copy_n(digest.begin(), bytes.size(), bytes.begin());
  // UUIDv8 denotes a deterministic application-defined digest layout.
  bytes[6] = static_cast<scratchbird::core::platform::byte>(
      (bytes[6] & 0x0fU) | 0x80U);
  bytes[8] = static_cast<scratchbird::core::platform::byte>(
      (bytes[8] & 0x3fU) | 0x80U);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(36);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) {
      result.push_back('-');
    }
    result.push_back(kHex[bytes[index] >> 4]);
    result.push_back(kHex[bytes[index] & 0x0fU]);
  }
  return result;
}

std::string DescriptorField(const std::string& descriptor,
                            const std::string& key) {
  const std::string prefix = key + "=";
  std::string value;
  bool found = false;
  std::size_t start = 0;
  while (start <= descriptor.size()) {
    const auto end = descriptor.find(';', start);
    const auto field = descriptor.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (field.rfind(prefix, 0) == 0) {
      if (found) return {};
      value = field.substr(prefix.size());
      found = true;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return value;
}

void AppendEqualityKeyField(std::string* key,
                            const std::string_view field) {
  const auto size = static_cast<std::uint64_t>(field.size());
  for (unsigned shift = 0; shift < 64; shift += 8) {
    key->push_back(static_cast<char>(size >> shift));
  }
  key->append(field);
}

bool CheckedEqualityKeySizeAdd(std::size_t* total,
                               const std::size_t amount) {
  if (total == nullptr ||
      *total > std::numeric_limits<std::size_t>::max() - amount) {
    return false;
  }
  *total += amount;
  return true;
}

bool CheckedEqualityKeySizeMultiply(const std::size_t left,
                                    const std::size_t right,
                                    std::size_t* product) {
  if (product == nullptr ||
      (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)) {
    return false;
  }
  *product = left * right;
  return true;
}

bool AddEqualityKeyFramedFieldBound(std::size_t* total,
                                    const std::size_t field_bytes) {
  return CheckedEqualityKeySizeAdd(total, sizeof(std::uint64_t)) &&
         CheckedEqualityKeySizeAdd(total, field_bytes);
}

bool DescriptorU32(const std::string& descriptor,
                   const std::string& key,
                   std::uint32_t* value) {
  if (value == nullptr) return false;
  const auto field = DescriptorField(descriptor, key);
  if (field.empty() || (field.size() > 1 && field.front() == '0')) return false;
  std::uint64_t parsed = 0;
  for (const auto ch : field) {
    if (ch < '0' || ch > '9') return false;
    parsed = parsed * 10u + static_cast<unsigned>(ch - '0');
    if (parsed > std::numeric_limits<std::uint32_t>::max()) return false;
  }
  *value = static_cast<std::uint32_t>(parsed);
  return true;
}

bool BindDescriptorRuntimeNumericRequest(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const scratchbird::core::datatypes::CanonicalTypeId type_id,
    const scratchbird::core::datatypes::DatatypeNumericOperationKind operation,
    scratchbird::core::datatypes::DatatypeNumericOperationRequest* request,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (request == nullptr || refusal_detail == nullptr) return false;
  *request = {};
  request->operation = operation;
  request->type_id = type_id;
  request->left.type_id = type_id;
  request->left.encoded_value = value.encoded_value;
  request->context.precision = 38;
  request->context.scale = 0;
  if (type_id == dt::CanonicalTypeId::decimal ||
      type_id == dt::CanonicalTypeId::decimal_float) {
    std::uint32_t precision = 0;
    std::uint32_t scale = 0;
    if (!DescriptorU32(value.descriptor.encoded_descriptor, "precision",
                       &precision) ||
        !DescriptorU32(value.descriptor.encoded_descriptor, "scale",
                       &scale)) {
      *refusal_detail =
          "numeric descriptor precision or scale is not bound";
      return false;
    }
    request->context.precision = precision;
    request->context.scale = scale;
  } else {
    std::uint32_t width = 0;
    if (!DescriptorU32(value.descriptor.encoded_descriptor, "width", &width) ||
        width != 128) {
      *refusal_detail = "128-bit numeric descriptor width is invalid";
      return false;
    }
  }
  return true;
}

bool CanonicalizeDescriptorRuntimeNumericValue(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const scratchbird::core::datatypes::CanonicalTypeId type_id,
    std::string* canonical_value,
    bool* is_nan,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (canonical_value == nullptr || is_nan == nullptr ||
      refusal_detail == nullptr) {
    return false;
  }
  canonical_value->clear();
  *is_nan = false;
  const auto exponent_position = value.encoded_value.find_first_of("eE");
  if (exponent_position != std::string::npos) {
    auto position = exponent_position + 1;
    bool negative_exponent = false;
    if (position < value.encoded_value.size() &&
        (value.encoded_value[position] == '+' ||
         value.encoded_value[position] == '-')) {
      negative_exponent = value.encoded_value[position] == '-';
      ++position;
    }
    const std::uint64_t exponent_bound =
        type_id == dt::CanonicalTypeId::real128
            ? 5000U
            : (value.encoded_value.size() >
                       std::numeric_limits<std::uint64_t>::max() - 38U
                   ? std::numeric_limits<std::uint64_t>::max()
                   : static_cast<std::uint64_t>(
                         value.encoded_value.size()) +
                         38U);
    std::uint64_t exponent = 0;
    for (; position < value.encoded_value.size(); ++position) {
      const auto byte = value.encoded_value[position];
      if (byte < '0' || byte > '9') break;
      if (exponent > exponent_bound / 10U) {
        exponent = exponent_bound + 1U;
        break;
      }
      exponent = exponent * 10U + static_cast<unsigned>(byte - '0');
      if (exponent > exponent_bound) break;
    }
    if ((!negative_exponent || type_id == dt::CanonicalTypeId::real128) &&
        exponent > exponent_bound) {
      *refusal_detail =
          "numeric exponent exceeds the descriptor runtime allocation domain";
      return false;
    }
  }
  dt::DatatypeNumericOperationRequest request;
  if (!BindDescriptorRuntimeNumericRequest(
          value, type_id, dt::DatatypeNumericOperationKind::canonicalize,
          &request, refusal_detail)) {
    return false;
  }
  const auto canonical = dt::ApplyNumericOperation(request);
  if (!canonical.ok()) {
    *refusal_detail = canonical.diagnostic.diagnostic_code;
    return false;
  }
  *canonical_value = canonical.value.encoded_value;
  if (*canonical_value == "NaN" || *canonical_value == "sNaN") {
    // SQL aggregate equality has one deterministic unordered numeric class.
    // The order comparator places that class after every ordered value, while
    // the equality key deliberately erases quiet/signaling spelling drift.
    *canonical_value = "NaN";
    *is_nan = true;
    return true;
  }

  request.operation = dt::DatatypeNumericOperationKind::compare;
  request.left.encoded_value = *canonical_value;
  request.right.type_id = type_id;
  request.right.encoded_value = "0";
  const auto compared_to_zero = dt::ApplyNumericOperation(request);
  if (!compared_to_zero.ok()) {
    *refusal_detail = compared_to_zero.diagnostic.diagnostic_code;
    return false;
  }
  if (compared_to_zero.comparison == 0) {
    // Normalize decimal scale spellings and every signed-zero spelling to the
    // single equality representative used by the comparator.
    *canonical_value = "0";
  }
  return true;
}

bool TextSeedAbsent(
    const scratchbird::core::datatypes::DatatypeTextSeedAuthority& seed) {
  return !seed.active && seed.seed_pack_name.empty() &&
         seed.seed_pack_version.empty() && seed.charset_name.empty() &&
         seed.collation_name.empty() &&
         !seed.collation_case_insensitive &&
         !seed.collation_accent_insensitive;
}

bool TimezoneSeedAbsent(
    const scratchbird::core::datatypes::TimezoneSeedAuthority& seed) {
  return !seed.active && seed.seed_pack_name.empty() &&
         seed.seed_pack_version.empty() && seed.content_hash.empty() &&
         seed.timezone_records == 0 &&
         seed.timezone_transition_records == 0 &&
         seed.timezone_leap_second_records == 0 &&
         seed.timezone_names.empty();
}

bool CompareOrderValues(
    const scratchbird::engine::internal_api::EngineTypedValue& left,
    const scratchbird::engine::internal_api::EngineTypedValue& right,
    const CanonicalDescriptorOrderTerm& term,
    int* comparison,
    std::string* refusal_detail) {
  namespace dt = scratchbird::core::datatypes;
  if (comparison == nullptr || refusal_detail == nullptr) return false;
  *comparison = 0;
  refusal_detail->clear();
  const auto type_id =
      dt::CanonicalTypeIdFromStableName(left.descriptor.canonical_type_name);
  if (type_id == dt::CanonicalTypeId::unknown ||
      left.descriptor.canonical_type_name !=
          right.descriptor.canonical_type_name) {
    *refusal_detail = "order operands do not share a supported scalar encoding";
    return false;
  }
  const auto canonical_state = [](const auto& value) {
    if (value.state ==
        scratchbird::engine::internal_api::EngineValueState::sql_null) {
      return value.is_null && value.encoded_value.empty() &&
             value.binary_value.empty();
    }
    return value.state ==
               scratchbird::engine::internal_api::EngineValueState::value &&
           !value.is_null;
  };
  if (!canonical_state(left) || !canonical_state(right)) {
    *refusal_detail =
        "order operand carries a malformed NULL or non-value sentinel";
    return false;
  }
  const bool carries_binary_payload =
      !left.binary_value.empty() || !right.binary_value.empty();
  if (carries_binary_payload && type_id != dt::CanonicalTypeId::binary) {
    *refusal_detail =
        "order operand carries binary payload for a non-binary type";
    return false;
  }
  const auto null_ordering =
      term.null_placement == CanonicalDescriptorNullPlacement::first
          ? dt::DatatypeNullOrdering::nulls_first
          : dt::DatatypeNullOrdering::nulls_last;
  const bool has_null =
      left.state ==
          scratchbird::engine::internal_api::EngineValueState::sql_null ||
      right.state ==
          scratchbird::engine::internal_api::EngineValueState::sql_null;
  std::string left_encoded = left.encoded_value;
  std::string right_encoded = right.encoded_value;
  if (!has_null && type_id == dt::CanonicalTypeId::binary) {
    if (!left.binary_value.empty()) {
      left_encoded.assign(
          reinterpret_cast<const char*>(left.binary_value.data()),
          left.binary_value.size());
    }
    if (!right.binary_value.empty()) {
      right_encoded.assign(
          reinterpret_cast<const char*>(right.binary_value.data()),
          right.binary_value.size());
    }
  }
  const auto timezone_profile = DescriptorField(
      left.descriptor.encoded_descriptor, "timezone_profile_id");
  bool timezone_normalized = false;
  std::pair<std::int64_t, std::uint64_t> left_timezone_key;
  std::pair<std::int64_t, std::uint64_t> right_timezone_key;
  if (!has_null && !timezone_profile.empty()) {
    const auto normalize = [&](const auto& value,
                               auto* comparable_key) {
      dt::ReferenceTemporalWireProfileRequest request;
      request.reference_engine = "scratchbird_native";
      request.reference_type_or_family =
          value.descriptor.canonical_type_name;
      request.wire_profile = timezone_profile;
      request.encoded_value = value.encoded_value;
      request.timezone_seed = term.timezone_seed;
      const auto normalized = dt::ValidateReferenceTemporalWireProfile(
          request);
      if (!normalized.ok() || normalized.canonical_type_id != type_id) {
        *refusal_detail = normalized.diagnostic.diagnostic_code.empty()
                              ? "timezone order normalization refused"
                              : normalized.diagnostic.diagnostic_code;
        return false;
      }
      if (!normalized.comparable_utc_key_available) {
        *refusal_detail =
            "named-zone order requires a resolved transition instant";
        return false;
      }
      *comparable_key = {
          normalized.comparable_utc_whole_seconds,
          normalized.comparable_fractional_picoseconds};
      return true;
    };
    if (!normalize(left, &left_timezone_key) ||
        !normalize(right, &right_timezone_key)) {
      return false;
    }
    timezone_normalized = true;
  }
  if (has_null) {
    dt::DatatypeComparisonRequest request;
    request.left.type_id = type_id;
    request.left.is_null =
        left.state ==
        scratchbird::engine::internal_api::EngineValueState::sql_null;
    request.right.type_id = type_id;
    request.right.is_null =
        right.state ==
        scratchbird::engine::internal_api::EngineValueState::sql_null;
    request.null_ordering = null_ordering;
    const auto compared = dt::CompareDatatypeValues(request);
    if (!compared.ok()) {
      *refusal_detail = compared.diagnostic.diagnostic_code;
      return false;
    }
    *comparison = compared.comparison;
  } else {
    const bool runtime_numeric =
        type_id == dt::CanonicalTypeId::decimal ||
        type_id == dt::CanonicalTypeId::decimal_float ||
        type_id == dt::CanonicalTypeId::int128 ||
        type_id == dt::CanonicalTypeId::uint128 ||
        type_id == dt::CanonicalTypeId::real128;
    if (runtime_numeric) {
      std::string left_canonical;
      std::string right_canonical;
      bool left_nan = false;
      bool right_nan = false;
      if (!CanonicalizeDescriptorRuntimeNumericValue(
              left, type_id, &left_canonical, &left_nan, refusal_detail) ||
          !CanonicalizeDescriptorRuntimeNumericValue(
              right, type_id, &right_canonical, &right_nan, refusal_detail)) {
        return false;
      }
      if (left_nan || right_nan) {
        *comparison = left_nan == right_nan ? 0 : (left_nan ? 1 : -1);
      } else {
        dt::DatatypeNumericOperationRequest request;
        if (!BindDescriptorRuntimeNumericRequest(
                left, type_id, dt::DatatypeNumericOperationKind::compare,
                &request, refusal_detail)) {
          return false;
        }
        request.left.encoded_value = std::move(left_canonical);
        request.right.type_id = type_id;
        request.right.encoded_value = std::move(right_canonical);
        const auto numeric = dt::ApplyNumericOperation(request);
        if (!numeric.ok()) {
          *refusal_detail = numeric.diagnostic.diagnostic_code;
          return false;
        }
        *comparison = numeric.comparison;
      }
    } else {
      if (timezone_normalized) {
        *comparison = left_timezone_key < right_timezone_key
                          ? -1
                          : (right_timezone_key < left_timezone_key ? 1 : 0);
        *comparison = *comparison < 0 ? -1 : (*comparison > 0 ? 1 : 0);
        if (term.direction ==
            CanonicalDescriptorOrderDirection::descending) {
          *comparison = -*comparison;
        }
        return true;
      }
      const auto validate = [type_id](const std::string& encoded_value) {
        dt::DatatypeCastRequest request;
        request.value.type_id = type_id;
        request.value.encoded_value = encoded_value;
        request.target_type_id = type_id;
        request.explicit_cast = true;
        return dt::CastDatatypeValue(request);
      };
      const auto left_checked = validate(left_encoded);
      const auto right_checked = validate(right_encoded);
      if (!left_checked.ok() || !right_checked.ok()) {
        *refusal_detail = "order operand encoding is invalid";
        return false;
      }
      dt::DatatypeComparisonRequest request;
      request.left = left_checked.value;
      request.right = right_checked.value;
      request.null_ordering = null_ordering;
      if (type_id == dt::CanonicalTypeId::character) {
        request.case_insensitive_character_compare =
            term.text_seed.collation_case_insensitive;
        request.text_seed = term.text_seed;
      }
      if (type_id == dt::CanonicalTypeId::bfloat16 ||
          type_id == dt::CanonicalTypeId::real16 ||
          type_id == dt::CanonicalTypeId::real32 ||
          type_id == dt::CanonicalTypeId::real64) {
        dt::DatatypeSortKeyRequest left_sort_request;
        left_sort_request.value = left_checked.value;
        left_sort_request.null_ordering = null_ordering;
        dt::DatatypeSortKeyRequest right_sort_request;
        right_sort_request.value = right_checked.value;
        right_sort_request.null_ordering = null_ordering;
        const auto left_key = dt::MakeDatatypeSortKey(left_sort_request);
        const auto right_key = dt::MakeDatatypeSortKey(right_sort_request);
        if (!left_key.ok() || !right_key.ok()) {
          *refusal_detail = !left_key.ok()
                                ? left_key.diagnostic.diagnostic_code
                                : right_key.diagnostic.diagnostic_code;
          return false;
        }
        *comparison = left_key.sort_key < right_key.sort_key
                          ? -1
                          : (left_key.sort_key > right_key.sort_key ? 1 : 0);
      } else {
        const auto compared = dt::CompareDatatypeValues(request);
        if (!compared.ok()) {
          *refusal_detail = compared.diagnostic.diagnostic_code;
          return false;
        }
        *comparison = compared.comparison;
      }
    }
  }
  *comparison = *comparison < 0 ? -1 : (*comparison > 0 ? 1 : 0);
  if (!has_null &&
      term.direction == CanonicalDescriptorOrderDirection::descending) {
    *comparison = -*comparison;
  }
  return true;
}

}  // namespace

static DescriptorRuntimeDiagnostic ValidateCanonicalDescriptorOrderTermFields(
    const CanonicalDescriptorOrderTerm& term,
    const scratchbird::engine::internal_api::EngineDescriptor& descriptor,
    const std::uint32_t descriptor_id) {
  namespace dt = scratchbird::core::datatypes;
  if (term.expression_descriptor_id == 0 ||
      term.expression_descriptor_id != descriptor_id) {
    return Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                   "order expression descriptor handle is unresolved");
  }
  if ((term.direction != CanonicalDescriptorOrderDirection::ascending &&
       term.direction != CanonicalDescriptorOrderDirection::descending) ||
      (term.null_placement != CanonicalDescriptorNullPlacement::first &&
       term.null_placement != CanonicalDescriptorNullPlacement::last)) {
    return Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                   "order direction or NULL placement is invalid");
  }
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      descriptor.canonical_type_name);
  if (type_id == dt::CanonicalTypeId::unknown) {
    return Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                   "order expression type is unknown");
  }
  if (type_id == dt::CanonicalTypeId::character) {
    const auto descriptor_collation = DescriptorField(
        descriptor.encoded_descriptor, "collation_uuid");
    const auto& seed = term.text_seed;
    if (!IsCanonicalUuid(term.collation_uuid) ||
        descriptor_collation != term.collation_uuid ||
        term.resource_epoch == 0 || term.collation_epoch == 0 ||
        !seed.active || seed.seed_pack_name.empty() ||
        seed.seed_pack_version.empty() || seed.charset_name.empty() ||
        seed.collation_name.empty()) {
      return Refusal(
          "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
          "character order lacks bound collation resource authority");
    }
    if (term.timezone_epoch != 0 ||
        !TimezoneSeedAbsent(term.timezone_seed)) {
      return Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                     "character order term carries timezone authority");
    }
  } else if ((type_id == dt::CanonicalTypeId::time ||
              type_id == dt::CanonicalTypeId::timestamp) &&
             !DescriptorField(descriptor.encoded_descriptor,
                              "timezone_profile_id")
                  .empty()) {
    const auto timezone_profile = DescriptorField(
        descriptor.encoded_descriptor, "timezone_profile_id");
    const bool profile_matches_type =
        (type_id == dt::CanonicalTypeId::timestamp &&
         timezone_profile == "timestamp_timezone_profile") ||
        (type_id == dt::CanonicalTypeId::time &&
         timezone_profile == "time_timezone_profile");
    const auto& seed = term.timezone_seed;
    if (!profile_matches_type || !term.collation_uuid.empty() ||
        term.resource_epoch == 0 || term.timezone_epoch == 0 ||
        term.collation_epoch != 0 || !TextSeedAbsent(term.text_seed) ||
        !seed.active || seed.seed_pack_name.empty() ||
        seed.seed_pack_version.empty() || seed.content_hash.empty() ||
        seed.timezone_records == 0 || seed.timezone_names.empty()) {
      return Refusal(
          "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
          "temporal order lacks bound timezone resource authority");
    }
  } else if (!term.collation_uuid.empty() || term.resource_epoch != 0 ||
             term.collation_epoch != 0 || !TextSeedAbsent(term.text_seed) ||
             term.timezone_epoch != 0 ||
             !TimezoneSeedAbsent(term.timezone_seed)) {
    return Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                   "order term carries authority for a different datatype");
  }
  return {};
}

DescriptorRuntimeDiagnostic ValidateCanonicalDescriptorOrderTerm(
    const CanonicalDescriptorOrderTerm& term,
    const ExecutorColumnDescriptor& column) {
  return ValidateCanonicalDescriptorOrderTermFields(
      term, column.descriptor, column.descriptor_id);
}

bool PlanCanonicalDescriptorOrderTermBindingEvidenceWorkspace(
    const CanonicalDescriptorOrderTerm& term,
    const std::string_view ordering_property_uuid,
    std::uint64_t* workspace_bytes) {
  constexpr std::uint64_t kUuidOutputBytes = 36;
  if (workspace_bytes == nullptr ||
      !IsCanonicalUuid(ordering_property_uuid) ||
      term.column > std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }
  *workspace_bytes = 0;
  OrderTermBindingEncoder planner(
      nullptr, std::numeric_limits<std::uint64_t>::max());
  if (!EncodeOrderTermBindingFields(&planner, term,
                                    ordering_property_uuid) ||
      planner.bytes() > std::numeric_limits<std::uint64_t>::max() -
                            kUuidOutputBytes) {
    return false;
  }
  *workspace_bytes = planner.bytes() + kUuidOutputBytes;
  return true;
}

std::string ComputeCanonicalDescriptorOrderTermBindingEvidenceUuid(
    const CanonicalDescriptorOrderTerm& term,
    const std::string_view ordering_property_uuid,
    const std::uint64_t maximum_workspace_bytes,
    std::uint64_t* actual_workspace_bytes) {
  namespace core_hash = scratchbird::core::hash;
  namespace platform = scratchbird::core::platform;
  constexpr std::uint64_t kUuidOutputBytes = 36;
  if (actual_workspace_bytes == nullptr) return {};
  *actual_workspace_bytes = 0;
  std::uint64_t planned_workspace_bytes = 0;
  if (!PlanCanonicalDescriptorOrderTermBindingEvidenceWorkspace(
          term, ordering_property_uuid, &planned_workspace_bytes) ||
      planned_workspace_bytes > maximum_workspace_bytes ||
      planned_workspace_bytes < kUuidOutputBytes ||
      planned_workspace_bytes - kUuidOutputBytes >
          std::numeric_limits<std::size_t>::max()) {
    return {};
  }
  const auto payload_bytes = planned_workspace_bytes - kUuidOutputBytes;
  try {
    std::vector<platform::byte> payload(
        static_cast<std::size_t>(payload_bytes));
    OrderTermBindingEncoder encoder(&payload, payload_bytes);
    if (!EncodeOrderTermBindingFields(&encoder, term,
                                      ordering_property_uuid) ||
        encoder.bytes() != payload_bytes) {
      return {};
    }
    const auto digest = core_hash::ComputeSha256Digest(payload);
    if (!digest.ok()) return {};
    auto result = OrderTermBindingUuidFromSha256(digest.digest);
    if (result.size() != kUuidOutputBytes) return {};
    *actual_workspace_bytes = planned_workspace_bytes;
    return result;
  } catch (const std::bad_alloc&) {
    return {};
  } catch (const std::length_error&) {
    return {};
  }
}

CanonicalDescriptorOrderComparisonResult CompareCanonicalDescriptorOrderValues(
    const scratchbird::engine::internal_api::EngineTypedValue& left,
    const scratchbird::engine::internal_api::EngineTypedValue& right,
    const CanonicalDescriptorOrderTerm& term) {
  CanonicalDescriptorOrderComparisonResult result;
  std::string detail;
  if (!CompareOrderValues(left, right, term, &result.comparison, &detail)) {
    result = {};
    result.diagnostic = Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                                std::move(detail));
  }
  return result;
}

CanonicalDescriptorEqualityKeyPlan PlanCanonicalDescriptorEqualityKey(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const CanonicalDescriptorOrderTerm& term) {
  namespace dt = scratchbird::core::datatypes;
  CanonicalDescriptorEqualityKeyPlan plan;
  plan.diagnostic = ValidateCanonicalDescriptorOrderTermFields(
      term, value.descriptor, term.expression_descriptor_id);
  if (!plan.diagnostic.ok) return plan;

  std::size_t retained = 0;
  const auto add_field = [&](const std::size_t bytes) {
    return AddEqualityKeyFramedFieldBound(&retained, bytes);
  };
  const auto add_exact_metadata = [&]() {
    return add_field(
               std::string_view("scratchbird.descriptor-equality-key.v1")
                   .size()) &&
           add_field(value.descriptor.descriptor_uuid.canonical.size()) &&
           add_field(value.descriptor.canonical_type_name.size()) &&
           add_field(value.descriptor.encoded_descriptor.size()) &&
           add_field(term.collation_uuid.size()) &&
           add_field(20) && add_field(20) && add_field(20) &&
           add_field(term.text_seed.seed_pack_name.size()) &&
           add_field(term.text_seed.seed_pack_version.size()) &&
           add_field(term.text_seed.charset_name.size()) &&
           add_field(term.text_seed.collation_name.size()) &&
           add_field(term.timezone_seed.seed_pack_name.size()) &&
           add_field(term.timezone_seed.seed_pack_version.size()) &&
           add_field(term.timezone_seed.content_hash.size());
  };
  if (!add_exact_metadata()) {
    plan.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
        "equality-key authority metadata size overflowed");
    return plan;
  }
  if (value.state ==
      scratchbird::engine::internal_api::EngineValueState::sql_null) {
    if (!add_field(std::string_view("sql-null").size())) {
      plan.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "NULL equality-key size overflowed");
      return plan;
    }
    plan.retained_key_bytes = retained;
    plan.peak_workspace_bytes = retained;
    return plan;
  }
  if (!add_field(std::string_view("value").size())) {
    plan.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
        "equality-key value discriminator size overflowed");
    return plan;
  }

  const auto timezone_profile = DescriptorField(
      value.descriptor.encoded_descriptor, "timezone_profile_id");
  if (!timezone_profile.empty()) {
    // int64 UTC seconds and uint64 fractional picoseconds are rendered as at
    // most 20 decimal bytes each.
    if (!add_field(20) || !add_field(20)) {
      plan.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "temporal equality-key size overflowed");
      return plan;
    }
    std::size_t temporal_authority_bytes = timezone_profile.size();
    if (!CheckedEqualityKeySizeAdd(
            &temporal_authority_bytes,
            value.descriptor.canonical_type_name.size()) ||
        !CheckedEqualityKeySizeAdd(
            &temporal_authority_bytes,
            term.timezone_seed.seed_pack_name.size()) ||
        !CheckedEqualityKeySizeAdd(
            &temporal_authority_bytes,
            term.timezone_seed.seed_pack_version.size()) ||
        !CheckedEqualityKeySizeAdd(
            &temporal_authority_bytes,
            term.timezone_seed.content_hash.size())) {
      plan.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "temporal equality authority size overflowed");
      return plan;
    }
    for (const auto& name : term.timezone_seed.timezone_names) {
      if (!CheckedEqualityKeySizeAdd(&temporal_authority_bytes,
                                     sizeof(std::string)) ||
          !CheckedEqualityKeySizeAdd(&temporal_authority_bytes,
                                     name.size())) {
        plan.diagnostic = Refusal(
            "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
            "temporal equality authority name size overflowed");
        return plan;
      }
    }
    // The wire validator carries the request, normalized text, parsed-zone
    // text, and returned result concurrently. These named carriage layers
    // bound their source and authority copies before key retention.
    constexpr std::size_t kTemporalWireValueCarriageLayers = 6;
    constexpr std::size_t kTemporalWireAuthorityCarriageLayers = 3;
    std::size_t temporal_value_workspace = 0;
    std::size_t temporal_authority_workspace = 0;
    std::size_t peak = retained;
    if (!CheckedEqualityKeySizeMultiply(
            value.encoded_value.size(), kTemporalWireValueCarriageLayers,
            &temporal_value_workspace) ||
        !CheckedEqualityKeySizeMultiply(
            temporal_authority_bytes,
            kTemporalWireAuthorityCarriageLayers,
            &temporal_authority_workspace) ||
        !CheckedEqualityKeySizeAdd(&peak, temporal_value_workspace) ||
        !CheckedEqualityKeySizeAdd(&peak, temporal_authority_workspace)) {
      plan.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "temporal equality workspace size overflowed");
      return plan;
    }
    plan.retained_key_bytes = retained;
    plan.peak_workspace_bytes = peak;
    return plan;
  }

  const auto type_id = dt::CanonicalTypeIdFromStableName(
      value.descriptor.canonical_type_name);
  const std::size_t payload_bytes =
      type_id == dt::CanonicalTypeId::binary && !value.binary_value.empty()
          ? value.binary_value.size()
          : value.encoded_value.size();
  std::size_t doubled_payload = 0;
  if (!CheckedEqualityKeySizeMultiply(payload_bytes, 2,
                                      &doubled_payload)) {
    plan.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
        "equality-key payload size overflowed");
    return plan;
  }
  std::size_t sort_key_bound = 0;
  std::size_t numeric_backend_workspace_bound = 0;
  if (type_id == dt::CanonicalTypeId::character) {
    sort_key_bound = doubled_payload;
    if (!CheckedEqualityKeySizeAdd(&sort_key_bound, 32) ||
        !CheckedEqualityKeySizeAdd(
            &sort_key_bound, term.text_seed.seed_pack_name.size()) ||
        !CheckedEqualityKeySizeAdd(
            &sort_key_bound, term.text_seed.seed_pack_version.size()) ||
        !CheckedEqualityKeySizeAdd(
            &sort_key_bound, term.text_seed.charset_name.size()) ||
        !CheckedEqualityKeySizeAdd(
            &sort_key_bound, term.text_seed.collation_name.size())) {
      plan.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "character equality-key size overflowed");
      return plan;
    }
  } else if (type_id == dt::CanonicalTypeId::int8 ||
             type_id == dt::CanonicalTypeId::int16 ||
             type_id == dt::CanonicalTypeId::int32 ||
             type_id == dt::CanonicalTypeId::int64 ||
             type_id == dt::CanonicalTypeId::int128 ||
             type_id == dt::CanonicalTypeId::uint8 ||
             type_id == dt::CanonicalTypeId::uint16 ||
             type_id == dt::CanonicalTypeId::uint32 ||
             type_id == dt::CanonicalTypeId::uint64 ||
             type_id == dt::CanonicalTypeId::uint128) {
    sort_key_bound = 44;
  } else if (type_id == dt::CanonicalTypeId::decimal ||
             type_id == dt::CanonicalTypeId::decimal_float) {
    // The descriptor-bound numeric backend canonicalizes these families to
    // at most their admitted source width plus bounded sign/scale/exponent
    // syntax.
    sort_key_bound = payload_bytes;
    if (!CheckedEqualityKeySizeAdd(&sort_key_bound, 128)) {
      plan.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "runtime numeric equality-key size overflowed");
      return plan;
    }
  } else if (type_id == dt::CanonicalTypeId::real128) {
    sort_key_bound = payload_bytes;
    if (!CheckedEqualityKeySizeAdd(&sort_key_bound, 192)) {
      plan.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "real128 equality-key size overflowed");
      return plan;
    }
    // Binary128 admits decimal exponents through roughly 5,000. The runtime
    // preflight above rejects larger source exponents before the numeric
    // backend can materialize an exponent-sized integer.
    numeric_backend_workspace_bound = 10032;
  } else if (type_id == dt::CanonicalTypeId::bfloat16 ||
             type_id == dt::CanonicalTypeId::real16 ||
             type_id == dt::CanonicalTypeId::real32 ||
             type_id == dt::CanonicalTypeId::real64) {
    // Ordered finite negative decimals carry a fixed 10,000-byte fractional
    // complement; the payload term covers positive/canonical text growth.
    sort_key_bound = payload_bytes;
    if (!CheckedEqualityKeySizeAdd(&sort_key_bound, 10032)) {
      plan.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "numeric equality-key size overflowed");
      return plan;
    }
  } else if (type_id == dt::CanonicalTypeId::date ||
             type_id == dt::CanonicalTypeId::time ||
             type_id == dt::CanonicalTypeId::timestamp ||
             type_id == dt::CanonicalTypeId::interval) {
    sort_key_bound = payload_bytes;
    if (!CheckedEqualityKeySizeAdd(&sort_key_bound, 3)) {
      plan.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "temporal equality-key size overflowed");
      return plan;
    }
  } else {
    sort_key_bound = doubled_payload;
    if (!CheckedEqualityKeySizeAdd(&sort_key_bound, 3)) {
      plan.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "descriptor equality-key size overflowed");
      return plan;
    }
  }
  if (!add_field(sort_key_bound)) {
    plan.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
        "retained equality-key size overflowed");
    return plan;
  }
  // CastDatatypeValue and MakeDatatypeSortKey retain five source-carriage
  // layers at their deepest point; sort-key expression construction may
  // concurrently retain the returned key plus two construction temporaries.
  constexpr std::size_t kDatatypeSourceCarriageLayers = 5;
  constexpr std::size_t kSortKeyConstructionLayers = 3;
  std::size_t source_workspace = 0;
  std::size_t sort_workspace = 0;
  std::size_t peak = retained;
  if (!CheckedEqualityKeySizeMultiply(
          payload_bytes, kDatatypeSourceCarriageLayers,
          &source_workspace) ||
      !CheckedEqualityKeySizeMultiply(
          sort_key_bound, kSortKeyConstructionLayers,
          &sort_workspace) ||
      !CheckedEqualityKeySizeAdd(&peak, source_workspace) ||
      !CheckedEqualityKeySizeAdd(&peak, sort_workspace) ||
      !CheckedEqualityKeySizeAdd(&peak,
                                 numeric_backend_workspace_bound)) {
    plan.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
        "equality-key workspace size overflowed");
    return plan;
  }
  plan.retained_key_bytes = retained;
  plan.peak_workspace_bytes = peak;
  return plan;
}

CanonicalDescriptorEqualityKeyResult MakeCanonicalDescriptorEqualityKey(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const CanonicalDescriptorOrderTerm& term) {
  namespace dt = scratchbird::core::datatypes;
  CanonicalDescriptorEqualityKeyResult result;
  const auto plan = PlanCanonicalDescriptorEqualityKey(value, term);
  if (!plan.diagnostic.ok) {
    result.diagnostic = plan.diagnostic;
    return result;
  }
  const auto self = CompareCanonicalDescriptorOrderValues(value, value, term);
  if (!self.diagnostic.ok || self.comparison != 0) {
    result.diagnostic = self.diagnostic.ok
                            ? Refusal(
                                  "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
                                  "equality-key operand is not reflexive")
                            : self.diagnostic;
    return result;
  }

  std::string key;
  key.reserve(plan.retained_key_bytes);
  AppendEqualityKeyField(&key, "scratchbird.descriptor-equality-key.v1");
  AppendEqualityKeyField(&key,
                         value.descriptor.descriptor_uuid.canonical);
  AppendEqualityKeyField(&key, value.descriptor.canonical_type_name);
  AppendEqualityKeyField(&key, value.descriptor.encoded_descriptor);
  AppendEqualityKeyField(&key, term.collation_uuid);
  AppendEqualityKeyField(&key, std::to_string(term.resource_epoch));
  AppendEqualityKeyField(&key, std::to_string(term.collation_epoch));
  AppendEqualityKeyField(&key, std::to_string(term.timezone_epoch));
  AppendEqualityKeyField(&key, term.text_seed.seed_pack_name);
  AppendEqualityKeyField(&key, term.text_seed.seed_pack_version);
  AppendEqualityKeyField(&key, term.text_seed.charset_name);
  AppendEqualityKeyField(&key, term.text_seed.collation_name);
  AppendEqualityKeyField(&key, term.timezone_seed.seed_pack_name);
  AppendEqualityKeyField(&key, term.timezone_seed.seed_pack_version);
  AppendEqualityKeyField(&key, term.timezone_seed.content_hash);

  if (value.state ==
      scratchbird::engine::internal_api::EngineValueState::sql_null) {
    AppendEqualityKeyField(&key, "sql-null");
    if (key.size() > plan.retained_key_bytes ||
        key.capacity() > plan.retained_key_bytes) {
      result.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "NULL equality key exceeded its allocation plan");
      return result;
    }
    result.equality_key = std::move(key);
    return result;
  }
  AppendEqualityKeyField(&key, "value");
  const auto type_id = dt::CanonicalTypeIdFromStableName(
      value.descriptor.canonical_type_name);
  const auto timezone_profile = DescriptorField(
      value.descriptor.encoded_descriptor, "timezone_profile_id");
  if (!timezone_profile.empty()) {
    dt::ReferenceTemporalWireProfileRequest request;
    request.reference_engine = "scratchbird_native";
    request.reference_type_or_family =
        value.descriptor.canonical_type_name;
    request.wire_profile = timezone_profile;
    request.encoded_value = value.encoded_value;
    request.timezone_seed = term.timezone_seed;
    const auto normalized = dt::ValidateReferenceTemporalWireProfile(request);
    if (!normalized.ok() || normalized.canonical_type_id != type_id ||
        !normalized.comparable_utc_key_available) {
      result.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          !normalized.ok()
              ? normalized.diagnostic.diagnostic_code
              : "named-zone equality requires a resolved transition instant");
      return result;
    }
    AppendEqualityKeyField(
        &key, std::to_string(normalized.comparable_utc_whole_seconds));
    AppendEqualityKeyField(
        &key,
        std::to_string(normalized.comparable_fractional_picoseconds));
    if (key.size() > plan.retained_key_bytes ||
        key.capacity() > plan.retained_key_bytes) {
      result.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "temporal equality key exceeded its allocation plan");
      return result;
    }
    result.equality_key = std::move(key);
    return result;
  }

  const bool runtime_numeric =
      type_id == dt::CanonicalTypeId::decimal ||
      type_id == dt::CanonicalTypeId::decimal_float ||
      type_id == dt::CanonicalTypeId::int128 ||
      type_id == dt::CanonicalTypeId::uint128 ||
      type_id == dt::CanonicalTypeId::real128;
  if (runtime_numeric) {
    std::string canonical_value;
    bool is_nan = false;
    std::string refusal_detail;
    if (!CanonicalizeDescriptorRuntimeNumericValue(
            value, type_id, &canonical_value, &is_nan, &refusal_detail)) {
      result.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          std::move(refusal_detail));
      return result;
    }
    AppendEqualityKeyField(&key, canonical_value);
    if (key.size() > plan.retained_key_bytes ||
        key.capacity() > plan.retained_key_bytes) {
      result.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
          "numeric equality key exceeded its allocation plan");
      return result;
    }
    result.equality_key = std::move(key);
    return result;
  }

  std::string encoded_value = value.encoded_value;
  if (type_id == dt::CanonicalTypeId::binary &&
      !value.binary_value.empty()) {
    encoded_value.assign(
        reinterpret_cast<const char*>(value.binary_value.data()),
        value.binary_value.size());
  }
  dt::DatatypeCastRequest cast_request;
  cast_request.value.type_id = type_id;
  cast_request.value.encoded_value = std::move(encoded_value);
  cast_request.value.is_null = false;
  cast_request.target_type_id = type_id;
  cast_request.explicit_cast = true;
  const auto checked = dt::CastDatatypeValue(cast_request);
  if (!checked.ok()) {
    result.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
        checked.diagnostic.diagnostic_code);
    return result;
  }
  dt::DatatypeSortKeyRequest sort_request;
  sort_request.value = checked.value;
  sort_request.null_ordering = dt::DatatypeNullOrdering::nulls_first;
  if (type_id == dt::CanonicalTypeId::character) {
    sort_request.case_insensitive_character_compare =
        term.text_seed.collation_case_insensitive;
    sort_request.text_seed = term.text_seed;
  }
  const auto sorted = dt::MakeDatatypeSortKey(sort_request);
  if (!sorted.ok()) {
    result.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
        sorted.diagnostic.diagnostic_code);
    return result;
  }
  AppendEqualityKeyField(&key, sorted.sort_key);
  if (key.size() > plan.retained_key_bytes ||
      key.capacity() > plan.retained_key_bytes) {
    result.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-EQUALITY-KEY-REFUSAL-V1",
        "descriptor equality key exceeded its allocation plan");
    return result;
  }
  result.equality_key = std::move(key);
  return result;
}

// QOW-SOURCE-QRY-007-SORT-LIMIT-V1
// First canonical implementation in this module: a typed LIMIT/OFFSET node.
// Typed ORDER BY terms are deliberately left to QRY-010; this entry does not
// fall back to the legacy one-integer-key sorter.
namespace {
enum class DescriptorLimitExecutionRoute : std::uint8_t {
  limit = 1,
  fetch_first_rows_only,
};

constexpr std::string_view DescriptorLimitImplementationId(
    const DescriptorLimitExecutionRoute route) {
  switch (route) {
    case DescriptorLimitExecutionRoute::limit:
      return "limit.typed.v1";
    case DescriptorLimitExecutionRoute::fetch_first_rows_only:
      return "fetch.native.rows-only.v1";
  }
  return {};
}

CanonicalDescriptorLimitResult ExecuteCanonicalDescriptorLimitBound(
    const CanonicalDescriptorLimitRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const DescriptorLimitExecutionRoute execution_route,
    const bool borrowed_execution_carriers) {
  CanonicalDescriptorLimitResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(request.input_batch))) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-SORT-LIMIT-PHYSICAL-ROUTE-V1",
        "descriptor limit request carries conflicting owned execution carriers"));
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  const auto expected_implementation_id =
      DescriptorLimitImplementationId(execution_route);
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          execution_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kLimit ||
      selected_node->implementation_id != expected_implementation_id ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-SORT-LIMIT-PHYSICAL-ROUTE-V1",
        "descriptor LIMIT/FETCH requires its selected root canonical "
        "implementation"));
  }
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr ||
      selected_node->output_descriptor_ids !=
          input_node->output_descriptor_ids) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "limit schema does not preserve input handles"));
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      execution_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));

  const auto row_count = execution_input_batch.rows.size();
  const auto offset = request.offset > row_count
                          ? row_count
                          : static_cast<std::size_t>(request.offset);
  const auto remaining = row_count - offset;
  const auto take = request.limit > remaining
                        ? remaining
                        : static_cast<std::size_t>(request.limit);

  std::uint64_t input_memory_bytes = 0;
  std::uint64_t output_memory_bytes = 0;
  if (!CanonicalDescriptorBatchMemoryBytes(execution_input_batch,
                                           &input_memory_bytes) ||
      !CanonicalDescriptorBatchRangeMemoryBytes(
          execution_input_batch, offset, take, &output_memory_bytes) ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required > execution_dag.memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "limit memory grant or runtime payload accounting is invalid"));
  }
  auto remaining_memory_bytes = selected_node->memory_bytes_required;
  const auto charge = [&](const std::uint64_t bytes) {
    if (bytes > remaining_memory_bytes) return false;
    remaining_memory_bytes -= bytes;
    return true;
  };
  if (!charge(input_memory_bytes) || !charge(output_memory_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "limit runtime materialization exceeds the selected node memory grant"));
  }

  result.output_batch.columns = execution_input_batch.columns;
  result.output_batch.rows.reserve(take);
  result.output_batch.rows.insert(
      result.output_batch.rows.end(),
      execution_input_batch.rows.begin() +
          static_cast<std::ptrdiff_t>(offset),
      execution_input_batch.rows.begin() +
          static_cast<std::ptrdiff_t>(offset + take));

  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);
  result.diagnostic = {};
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalDescriptorLimitResult ExecuteCanonicalDescriptorLimit(
    const CanonicalDescriptorLimitRequest& request) {
  return ExecuteCanonicalDescriptorLimitBound(
      request, request.physical_dag, request.input_batch,
      DescriptorLimitExecutionRoute::limit, false);
}

CanonicalDescriptorLimitResult ExecuteCanonicalDescriptorLimit(
    const CanonicalDescriptorLimitRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalDescriptorLimitBound(
      request, borrowed_execution_dag, borrowed_input_batch,
      DescriptorLimitExecutionRoute::limit, true);
}

// QOW-SOURCE-QRY-010-FETCH-TOP-PROFILE-V1
// The native SBSQL development profile admits only FETCH FIRST <bound count>
// ROWS ONLY.  WITH TIES and donor TOP variants stay explicit refusals rather
// than silently degrading to an ordinary limit.
namespace {
CanonicalDescriptorFetchProfileResult
ExecuteCanonicalDescriptorFetchProfileBound(
    const CanonicalDescriptorFetchProfileRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const bool borrowed_execution_carriers) {
  CanonicalDescriptorFetchProfileResult result;
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(request.input_batch))) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-010-FETCH-TOP-PROFILE-REFUSAL-V1";
    result.diagnostic.detail =
        "FETCH request carries conflicting owned execution carriers";
    return result;
  }
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!entry_authority.ok) {
    result.diagnostic = entry_authority;
    return result;
  }
  if (request.form !=
          CanonicalFetchTopProfileForm::fetch_first_rows_only ||
      !request.row_count_is_bound) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-010-FETCH-TOP-PROFILE-REFUSAL-V1";
    result.diagnostic.detail =
        "only bound native FETCH FIRST ROWS ONLY is admitted";
    return result;
  }

  CanonicalDescriptorLimitRequest limit_request;
  limit_request.selected_physical_node_id =
      request.selected_physical_node_id;
  limit_request.limit = request.row_count;
  limit_request.offset = request.offset;
  limit_request.mga_authority = request.mga_authority;
  auto limited = ExecuteCanonicalDescriptorLimitBound(
      limit_request, execution_dag, execution_input_batch,
      DescriptorLimitExecutionRoute::fetch_first_rows_only, true);
  if (limited.diagnostic.ok &&
      !PhysicalMgaStatementContextEqual(
          limited.mga_statement_context,
          request.mga_authority.statement_context)) {
    limited.diagnostic.ok = false;
    limited.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-010-FETCH-TOP-MGA-V1";
    limited.diagnostic.detail =
        "FETCH nested limit returned a different MGA statement context";
  }
  if (limited.diagnostic.ok) {
    const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
        request.mga_authority, execution_dag);
    if (!result_authority.ok) limited.diagnostic = result_authority;
  }
  if (!limited.diagnostic.ok) {
    result.diagnostic = std::move(limited.diagnostic);
    return result;
  }
  result.diagnostic = std::move(limited.diagnostic);
  result.output_batch = std::move(limited.output_batch);
  result.selected_plan_uuid = std::move(limited.selected_plan_uuid);
  result.executed_physical_node_id = limited.executed_physical_node_id;
  result.causal_counter_id = limited.causal_counter_id;
  if (result.diagnostic.ok) {
    result.mga_statement_context = request.mga_authority.statement_context;
  }
  return result;
}
}  // namespace

CanonicalDescriptorFetchProfileResult ExecuteCanonicalDescriptorFetchProfile(
    const CanonicalDescriptorFetchProfileRequest& request) {
  return ExecuteCanonicalDescriptorFetchProfileBound(
      request, request.physical_dag, request.input_batch, false);
}

CanonicalDescriptorFetchProfileResult ExecuteCanonicalDescriptorFetchProfile(
    const CanonicalDescriptorFetchProfileRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalDescriptorFetchProfileBound(
      request, borrowed_execution_dag, borrowed_input_batch, true);
}

// QOW-SOURCE-QRY-010-DISTINCT-COMPOSITION-V1
// Query DISTINCT is the optimizer's distinct-aggregate physical operation.
// Equality is evaluated over every projected descriptor using the same typed
// comparator, SQL NULL equality, and catalog-bound collation authority as
// canonical ordering. Every value validates before duplicate representatives
// are materialized, so a later duplicate cannot hide malformed input.
namespace {
CanonicalDescriptorDistinctResult ExecuteCanonicalDescriptorDistinctBound(
    const CanonicalDescriptorDistinctRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_input_batch,
    const std::vector<CanonicalDescriptorOrderTerm>& execution_equality_terms,
    const bool borrowed_execution_carriers,
    const bool borrowed_equality_terms) {
  CanonicalDescriptorDistinctResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    return result;
  };
  const auto distinct_refusal = [&](std::string detail) {
    return refuse(Refusal("QOW-DIAG-QRY-010-DISTINCT-REFUSAL-V1",
                          std::move(detail)));
  };

  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(request.input_batch))) {
    return distinct_refusal(
        "query DISTINCT request carries conflicting owned execution carriers");
  }
  if (borrowed_equality_terms &&
      !DescriptorOrderTermsCarrierIsExactDefault(request.equality_terms)) {
    return distinct_refusal(
        "query DISTINCT request carries conflicting owned equality terms");
  }

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!authority_validation.ok) return refuse(authority_validation);

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          execution_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kAggregate ||
      selected_node->implementation_id !=
          "aggregate.query-distinct.typed.v1" ||
      selected_node->input_physical_node_ids.size() != 1) {
    return distinct_refusal(
        "query DISTINCT requires one selected root distinct-aggregate node");
  }
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr ||
      selected_node->output_descriptor_ids !=
          input_node->output_descriptor_ids) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "query DISTINCT does not preserve its input schema"));
  }
  auto validation = ValidateCanonicalDescriptorBatch(
      execution_input_batch, input_node->output_descriptor_ids);
  if (!validation.ok) return refuse(std::move(validation));
  if (execution_equality_terms.size() !=
          execution_input_batch.columns.size() ||
      execution_equality_terms.empty() ||
      request.maximum_value_comparisons == 0) {
    return distinct_refusal(
        "query DISTINCT requires one bounded equality term per output column");
  }

  std::uint64_t input_memory_bytes = 0;
  if (!CanonicalDescriptorBatchMemoryBytes(execution_input_batch,
                                           &input_memory_bytes) ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required > execution_dag.memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max()) ||
      execution_input_batch.columns.size() >
          std::numeric_limits<std::uint64_t>::max() /
              sizeof(std::uint8_t) ||
      execution_input_batch.rows.size() >
          std::numeric_limits<std::uint64_t>::max() /
              sizeof(std::size_t)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "query DISTINCT memory grant or runtime payload accounting is invalid"));
  }
  auto remaining_memory_bytes = selected_node->memory_bytes_required;
  const auto charge = [&](const std::uint64_t bytes) {
    if (bytes > remaining_memory_bytes) return false;
    remaining_memory_bytes -= bytes;
    return true;
  };
  const auto coverage_memory_bytes =
      static_cast<std::uint64_t>(execution_input_batch.columns.size()) *
      sizeof(std::uint8_t);
  const auto representative_memory_bytes =
      static_cast<std::uint64_t>(execution_input_batch.rows.size()) *
      sizeof(std::size_t);
  if (!charge(input_memory_bytes) || !charge(coverage_memory_bytes) ||
      !charge(representative_memory_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "query DISTINCT runtime state exceeds the selected node memory grant"));
  }

  // A byte per descriptor makes the admitted coverage carrier deterministic;
  // vector<bool> has implementation-defined packed storage and cannot support
  // an exact producer-memory receipt.
  std::vector<std::uint8_t> covered(execution_input_batch.columns.size(), 0);
  for (const auto& term : execution_equality_terms) {
    if (term.column >= execution_input_batch.columns.size() ||
        covered[term.column] ||
        term.direction != CanonicalDescriptorOrderDirection::ascending ||
        term.null_placement != CanonicalDescriptorNullPlacement::first) {
      return distinct_refusal(
          "query DISTINCT equality term coverage or canonical form is invalid");
    }
    const auto term_validation = ValidateCanonicalDescriptorOrderTerm(
        term, execution_input_batch.columns[term.column]);
    if (!term_validation.ok) return distinct_refusal(term_validation.detail);
    covered[term.column] = true;
  }

  std::size_t comparison_count = 0;
  const auto equal_rows = [&](const DescriptorTuple& left,
                              const DescriptorTuple& right,
                              bool* equal,
                              std::string* detail) {
    if (equal == nullptr || detail == nullptr) return false;
    *equal = true;
    for (const auto& term : execution_equality_terms) {
      if (comparison_count >= request.maximum_value_comparisons) {
        *detail = "query DISTINCT value comparison bound was exceeded";
        return false;
      }
      ++comparison_count;
      const auto compared = CompareCanonicalDescriptorOrderValues(
          left.values[term.column], right.values[term.column], term);
      if (!compared.diagnostic.ok) {
        *detail = compared.diagnostic.detail;
        return false;
      }
      if (compared.comparison != 0) {
        *equal = false;
        return true;
      }
    }
    return true;
  };

  // Validate every typed value independently before deciding membership.
  for (const auto& row : execution_input_batch.rows) {
    bool equal = false;
    std::string detail;
    if (!equal_rows(row, row, &equal, &detail) || !equal) {
      return distinct_refusal(detail.empty()
                                  ? "query DISTINCT self comparison failed"
                                  : std::move(detail));
    }
  }

  std::vector<std::size_t> representative_rows(
      execution_input_batch.rows.size(), 0);
  std::size_t representative_count = 0;
  std::size_t eliminated = 0;
  for (std::size_t row = 0; row < execution_input_batch.rows.size(); ++row) {
    bool duplicate = false;
    for (std::size_t representative_index = 0;
         representative_index < representative_count;
         ++representative_index) {
      const auto representative =
          representative_rows[representative_index];
      bool equal = false;
      std::string detail;
      if (!equal_rows(execution_input_batch.rows[row],
                      execution_input_batch.rows[representative], &equal,
                      &detail)) {
        return distinct_refusal(std::move(detail));
      }
      if (equal) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      ++eliminated;
    } else {
      representative_rows[representative_count++] = row;
    }
  }

  DescriptorBatch output;
  output.columns = execution_input_batch.columns;
  std::uint64_t output_memory_bytes = 1;
  for (std::size_t representative_index = 0;
       representative_index < representative_count;
       ++representative_index) {
    const auto row = representative_rows[representative_index];
    for (const auto& value : execution_input_batch.rows[row].values) {
      if (value.encoded_value.size() >
              std::numeric_limits<std::uint64_t>::max() -
                  output_memory_bytes) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "query DISTINCT output payload accounting overflowed"));
      }
      output_memory_bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::uint64_t>::max() -
              output_memory_bytes) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "query DISTINCT output payload accounting overflowed"));
      }
      output_memory_bytes += value.binary_value.size();
    }
  }
  if (!charge(output_memory_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "query DISTINCT output exceeds the selected node memory grant"));
  }
  output.rows.reserve(representative_count);
  for (std::size_t representative_index = 0;
       representative_index < representative_count;
       ++representative_index) {
    const auto row = representative_rows[representative_index];
    output.rows.push_back(execution_input_batch.rows[row]);
  }
  validation = ValidateCanonicalDescriptorBatch(
      output, selected_node->output_descriptor_ids);
  if (!validation.ok) return refuse(std::move(validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.eliminated_duplicate_row_count = eliminated;
  result.value_comparison_count = comparison_count;
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalDescriptorDistinctResult ExecuteCanonicalDescriptorDistinct(
    const CanonicalDescriptorDistinctRequest& request) {
  return ExecuteCanonicalDescriptorDistinctBound(
      request, request.physical_dag, request.input_batch,
      request.equality_terms, false, false);
}

CanonicalDescriptorDistinctResult ExecuteCanonicalDescriptorDistinct(
    const CanonicalDescriptorDistinctRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalDescriptorDistinctBound(
      request, borrowed_execution_dag, borrowed_input_batch,
      request.equality_terms, true, false);
}

CanonicalDescriptorDistinctResult ExecuteCanonicalDescriptorDistinct(
    const CanonicalDescriptorDistinctRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch,
    const std::vector<CanonicalDescriptorOrderTerm>& borrowed_equality_terms) {
  return ExecuteCanonicalDescriptorDistinctBound(
      request, borrowed_execution_dag, borrowed_input_batch,
      borrowed_equality_terms, true, true);
}

// QOW-SOURCE-QRY-010-V1
// Execute an already-bound physical ORDER BY node.  Every descriptor handle,
// collation authority, operand encoding, and row-pair comparison is validated
// before the first output row is materialized.
namespace {
CanonicalDescriptorSortResult ExecuteCanonicalDescriptorSortBound(
    const TypedPhysicalNodeDag& execution_dag,
    const CanonicalExecutionMgaAuthority& execution_mga_authority,
    const std::uint64_t selected_physical_node_id,
    const std::size_t maximum_pair_comparisons,
    const DescriptorBatch& execution_input_batch,
    const DescriptorBatch& execution_order_batch,
    const std::vector<CanonicalDescriptorOrderTerm>& execution_order_terms,
    const std::string& execution_deterministic_tie_evidence_uuid,
    const bool separate_order_key_batch) {
  CanonicalDescriptorSortResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  const auto order_refusal = [&](std::string detail) {
    return refuse(Refusal("QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
                          std::move(detail)));
  };

  const TypedPhysicalNodeDag* physical_dag = &execution_dag;
  const CanonicalExecutionMgaAuthority* mga_authority =
      &execution_mga_authority;
  const DescriptorBatch* input_batch = &execution_input_batch;
  const DescriptorBatch* order_batch = &execution_order_batch;
  const std::vector<CanonicalDescriptorOrderTerm>* order_terms =
      &execution_order_terms;
  const std::string* deterministic_tie_evidence_uuid =
      &execution_deterministic_tie_evidence_uuid;

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      *mga_authority, *physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  const std::string_view expected_implementation_id =
      separate_order_key_batch ? "sort.typed.expression-row.v1"
                               : "sort.typed.terms.v1";
  for (const auto& node : physical_dag->nodes) {
    if (node.physical_node_id == selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_physical_node_id == 0 ||
      selected_physical_node_id != physical_dag->root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kSort ||
      selected_node->implementation_id != expected_implementation_id ||
      selected_node->input_physical_node_ids.size() != 1) {
    return order_refusal(
        "descriptor order requires one selected root canonical sort "
        "implementation");
  }
  for (const auto& node : physical_dag->nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr ||
      selected_node->output_descriptor_ids !=
          input_node->output_descriptor_ids) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "sort schema does not preserve input handles"));
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      *input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  if (separate_order_key_batch) {
    if (order_batch->rows.size() != input_batch->rows.size()) {
      return order_refusal(
          "materialized order-key cardinality differs from the input");
    }
    std::vector<std::uint32_t> order_descriptor_ids;
    order_descriptor_ids.reserve(order_batch->columns.size());
    for (const auto& column : order_batch->columns) {
      order_descriptor_ids.push_back(column.descriptor_id);
    }
    auto order_validation = ValidateCanonicalDescriptorBatch(
        *order_batch, order_descriptor_ids);
    if (!order_validation.ok) return refuse(std::move(order_validation));
  }
  if (order_terms->empty() ||
      !IsCanonicalUuid(*deterministic_tie_evidence_uuid) ||
      *deterministic_tie_evidence_uuid ==
          "00000000-0000-0000-0000-000000000000") {
    return order_refusal(
        "bound order terms and deterministic tie evidence are required");
  }

  std::unordered_set<std::string> forbidden_tie_identities;
  forbidden_tie_identities.insert(physical_dag->selected_plan_uuid);
  forbidden_tie_identities.insert(selected_node->selected_alternative_uuid);
  forbidden_tie_identities.insert(selected_node->executor_capability_uuid);
  forbidden_tie_identities.insert(selected_node->cost_vector_uuid);
  forbidden_tie_identities.insert(
      selected_node->retained_cost.cost_vector_uuid);
  forbidden_tie_identities.insert(selected_node->transformation_uuid);
  forbidden_tie_identities.insert(
      selected_node->required_property_uuids.begin(),
      selected_node->required_property_uuids.end());
  forbidden_tie_identities.insert(
      selected_node->delivered_property_uuids.begin(),
      selected_node->delivered_property_uuids.end());
  forbidden_tie_identities.insert(
      selected_node->enforced_property_uuids.begin(),
      selected_node->enforced_property_uuids.end());
  for (const auto& term : *order_terms) {
    if (term.column >= order_batch->columns.size()) {
      return order_refusal("order term column is outside the input schema");
    }
    const auto& column = order_batch->columns[term.column];
    const auto validation =
        ValidateCanonicalDescriptorOrderTerm(term, column);
    if (!validation.ok) {
      return order_refusal(validation.detail);
    }
    const auto type_uuid =
        DescriptorField(column.descriptor.encoded_descriptor, "type_uuid");
    if (!IsCanonicalUuid(type_uuid)) {
      return order_refusal("ordered column type identity is unresolved");
    }
    forbidden_tie_identities.insert(
        column.descriptor.descriptor_uuid.canonical);
    forbidden_tie_identities.insert(type_uuid);
    if (!term.collation_uuid.empty()) {
      forbidden_tie_identities.insert(term.collation_uuid);
    }
  }
  if (forbidden_tie_identities.contains(
          *deterministic_tie_evidence_uuid)) {
    return order_refusal(
        "deterministic tie evidence aliases an ordering authority identity");
  }

  const auto row_count = input_batch->rows.size();
  if (maximum_pair_comparisons == 0 ||
      (row_count != 0 &&
       row_count >
           std::numeric_limits<std::size_t>::max() / row_count)) {
    return order_refusal("order comparison resource bound overflowed");
  }
  const auto matrix_size = row_count * row_count;
  if (matrix_size > maximum_pair_comparisons) {
    return order_refusal("order comparison resource bound was exceeded");
  }

  std::uint64_t input_memory_bytes = 0;
  std::uint64_t order_memory_bytes = 0;
  if (!CanonicalDescriptorBatchMemoryBytes(*input_batch,
                                           &input_memory_bytes) ||
      (separate_order_key_batch &&
       !CanonicalDescriptorBatchMemoryBytes(*order_batch,
                                            &order_memory_bytes)) ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required > physical_dag->memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max()) ||
      row_count > std::numeric_limits<std::uint64_t>::max() /
                      sizeof(std::size_t)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "sort memory grant or runtime payload accounting is invalid"));
  }
  auto remaining_memory_bytes = selected_node->memory_bytes_required;
  const auto charge = [&](const std::uint64_t bytes) {
    if (bytes > remaining_memory_bytes) return false;
    remaining_memory_bytes -= bytes;
    return true;
  };
  const auto matrix_memory_bytes = static_cast<std::uint64_t>(matrix_size);
  const auto row_order_memory_bytes =
      static_cast<std::uint64_t>(row_count) * sizeof(std::size_t);
  if (!charge(input_memory_bytes) || !charge(input_memory_bytes) ||
      (separate_order_key_batch && !charge(order_memory_bytes)) ||
      !charge(matrix_memory_bytes) || !charge(row_order_memory_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "sort runtime materialization exceeds the selected node memory grant"));
  }

  std::vector<std::int8_t> comparisons(matrix_size, 0);
  for (std::size_t row = 0; row < row_count; ++row) {
    for (const auto& term : *order_terms) {
      const auto& value = order_batch->rows[row].values[term.column];
      const auto compared =
          CompareCanonicalDescriptorOrderValues(value, value, term);
      if (!compared.diagnostic.ok) {
        return order_refusal("order operand refusal: " +
                             compared.diagnostic.detail);
      }
    }
  }
  for (std::size_t left = 0; left < row_count; ++left) {
    for (std::size_t right = left + 1; right < row_count; ++right) {
      int comparison = 0;
      for (const auto& term : *order_terms) {
        const auto& left_value =
            order_batch->rows[left].values[term.column];
        const auto& right_value =
            order_batch->rows[right].values[term.column];
        const auto compared = CompareCanonicalDescriptorOrderValues(
            left_value, right_value, term);
        if (!compared.diagnostic.ok) {
          return order_refusal("order comparison refusal: " +
                               compared.diagnostic.detail);
        }
        comparison = compared.comparison;
        if (comparison != 0) break;
      }
      comparisons[left * row_count + right] =
          static_cast<std::int8_t>(comparison);
      comparisons[right * row_count + left] =
          static_cast<std::int8_t>(-comparison);
    }
  }

  std::vector<std::size_t> row_order(row_count);
  std::iota(row_order.begin(), row_order.end(), 0);
  std::sort(row_order.begin(), row_order.end(),
            [&](const std::size_t left, const std::size_t right) {
              const auto comparison =
                  comparisons[left * row_count + right];
              return comparison < 0 || (comparison == 0 && left < right);
            });

  result.output_batch.columns = input_batch->columns;
  result.output_batch.rows.reserve(row_count);
  for (const auto row : row_order) {
    result.output_batch.rows.push_back(input_batch->rows[row]);
  }
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      *mga_authority, *physical_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.selected_plan_uuid = physical_dag->selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = mga_authority->statement_context;
  return result;
}
}  // namespace

CanonicalDescriptorSortResult ExecuteCanonicalDescriptorSort(
    const CanonicalDescriptorSortRequest& request) {
  const TypedPhysicalNodeDag* physical_dag = &request.physical_dag;
  const CanonicalExecutionMgaAuthority* mga_authority =
      &request.mga_authority;
  std::uint64_t selected_physical_node_id =
      request.selected_physical_node_id;
  std::size_t maximum_pair_comparisons =
      request.maximum_pair_comparisons;
  const DescriptorBatch* input_batch = &request.input_batch;
  const DescriptorBatch* order_batch = &request.input_batch;
  const std::vector<CanonicalDescriptorOrderTerm>* order_terms =
      &request.order_terms;
  const std::string* deterministic_tie_evidence_uuid =
      &request.deterministic_tie_evidence_uuid;
  bool separate_order_key_batch = false;
  if (request.order_key_receipt != nullptr) {
    const auto& receipt = *request.order_key_receipt;
    if (!SortReceiptRequestCarriersAreExactDefault(request) ||
        !receipt.exact_current_revalidated_before_issue_ ||
        receipt.borrowed_execution_carriers_ ||
        receipt.physical_dag_.nodes.empty() ||
        receipt.selected_physical_node_id_ == 0 ||
        receipt.maximum_pair_comparisons_ == 0 ||
        receipt.maximum_order_key_batch_bytes_ == 0 ||
        receipt.ordering_property_uuid_.empty()) {
      CanonicalDescriptorSortResult result;
      result.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
          "expression order-key receipt does not bind this execution");
      return result;
    }
    physical_dag = &receipt.physical_dag_;
    mga_authority = &receipt.mga_authority_;
    selected_physical_node_id = receipt.selected_physical_node_id_;
    maximum_pair_comparisons = receipt.maximum_pair_comparisons_;
    input_batch = &receipt.input_batch_;
    order_batch = &receipt.order_key_batch_;
    order_terms = &receipt.order_terms_;
    deterministic_tie_evidence_uuid =
        &receipt.deterministic_tie_evidence_uuid_;
    separate_order_key_batch = true;
  }
  return ExecuteCanonicalDescriptorSortBound(
      *physical_dag, *mga_authority, selected_physical_node_id,
      maximum_pair_comparisons, *input_batch, *order_batch, *order_terms,
      *deterministic_tie_evidence_uuid, separate_order_key_batch);
}

CanonicalDescriptorSortResult ExecuteCanonicalDescriptorSort(
    const CanonicalDescriptorSortRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch) {
  if (request.order_key_receipt != nullptr) {
    const auto& receipt = *request.order_key_receipt;
    if (!SortReceiptRequestCarriersAreExactDefault(request) ||
        !receipt.exact_current_revalidated_before_issue_ ||
        !receipt.borrowed_execution_carriers_ ||
        !TypedPhysicalNodeDagCarrierIsExactDefault(receipt.physical_dag_) ||
        !DescriptorBatchCarrierIsExactDefault(receipt.input_batch_) ||
        receipt.selected_physical_node_id_ == 0 ||
        receipt.maximum_pair_comparisons_ == 0 ||
        receipt.maximum_order_key_batch_bytes_ == 0 ||
        receipt.ordering_property_uuid_.empty()) {
      CanonicalDescriptorSortResult result;
      result.diagnostic = Refusal(
          "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
          "borrowed expression order-key receipt does not bind this execution");
      return result;
    }
    return ExecuteCanonicalDescriptorSortBound(
        borrowed_execution_dag, receipt.mga_authority_,
        receipt.selected_physical_node_id_,
        receipt.maximum_pair_comparisons_, borrowed_input_batch,
        receipt.order_key_batch_, receipt.order_terms_,
        receipt.deterministic_tie_evidence_uuid_, true);
  }
  if (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
      !DescriptorBatchCarrierIsExactDefault(request.input_batch) ||
      request.order_key_receipt != nullptr) {
    CanonicalDescriptorSortResult result;
    result.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
        "descriptor order request carries conflicting owned execution carriers");
    return result;
  }
  return ExecuteCanonicalDescriptorSortBound(
      borrowed_execution_dag, request.mga_authority,
      request.selected_physical_node_id, request.maximum_pair_comparisons,
      borrowed_input_batch, borrowed_input_batch, request.order_terms,
      request.deterministic_tie_evidence_uuid, false);
}

CanonicalDescriptorSortResult ExecuteCanonicalDescriptorSort(
    const CanonicalDescriptorSortRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch,
    const std::vector<CanonicalDescriptorOrderTerm>& borrowed_order_terms,
    const std::string& borrowed_deterministic_tie_evidence_uuid) {
  if (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
      !DescriptorBatchCarrierIsExactDefault(request.input_batch) ||
      !DescriptorOrderTermsCarrierIsExactDefault(request.order_terms) ||
      !StringCarrierIsExactDefault(
          request.deterministic_tie_evidence_uuid) ||
      request.order_key_receipt != nullptr) {
    CanonicalDescriptorSortResult result;
    result.diagnostic = Refusal(
        "QOW-DIAG-QRY-010-ORDER-REFUSAL-V1",
        "descriptor order request carries conflicting owned execution or semantic carriers");
    return result;
  }
  return ExecuteCanonicalDescriptorSortBound(
      borrowed_execution_dag, request.mga_authority,
      request.selected_physical_node_id, request.maximum_pair_comparisons,
      borrowed_input_batch, borrowed_input_batch, borrowed_order_terms,
      borrowed_deterministic_tie_evidence_uuid, false);
}

}  // namespace scratchbird::engine::executor
