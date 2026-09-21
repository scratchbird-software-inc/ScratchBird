// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "relational_descriptor_codec.hpp"
#include "core/datatypes/canonical_utf8.hpp"
#include "core/uuid/uuid.hpp"

#include <algorithm>
#include <array>
#include <string_view>
#include <type_traits>

namespace scratchbird::engine::sblr {
namespace {
using Descriptor = internal_api::RelationalTypeDescriptor;
using Uuid = core::platform::Uuid;
using Bytes = std::vector<std::uint8_t>;
constexpr std::uint16_t kAuthority = 1, kCollation = 2, kTimezone = 4;
constexpr std::uint16_t kWidth = 8, kPrecision = 16, kScale = 32;
bool TextValid(std::string_view value, std::size_t maximum) noexcept {
  return !value.empty() && value.size() <= maximum &&
      core::datatypes::ValidateCanonicalUtf8(
          reinterpret_cast<const std::uint8_t*>(value.data()), value.size()) &&
      std::ranges::none_of(value, [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}
void Put(Bytes& output, std::size_t offset, std::uint64_t value, unsigned size) {
  for (unsigned index = 0; index < size; ++index)
    output[offset + index] = static_cast<std::uint8_t>(value >> (8 * index));
}
std::uint64_t Get(const std::uint8_t* data, std::size_t offset, unsigned size) {
  std::uint64_t value = 0;
  for (unsigned index = 0; index < size; ++index)
    value |= std::uint64_t(data[offset + index]) << (8 * index);
  return value;
}
Uuid GetUuid(const std::uint8_t* data, std::size_t offset) {
  Uuid uuid;
  std::copy_n(data + offset, 16, uuid.bytes.begin());
  return uuid;
}
void PutUuid(Bytes& output, std::size_t offset, const Uuid& uuid) {
  std::copy(uuid.bytes.begin(), uuid.bytes.end(), output.begin() + offset);
}
bool OrderingTermValid(const internal_api::RelationalPropertyOrderingTerm& term) noexcept {
  const auto direction = static_cast<unsigned>(term.direction);
  const auto placement = static_cast<unsigned>(term.null_placement);
  return term.expression_id != 0 && direction >= 1 && direction <= 2 && placement >= 1 && placement <= 2 &&
      (term.collation_uuid.is_nil() || core::uuid::IsEngineIdentityUuid(term.collation_uuid));
}
void PutOrderingTerm(Bytes& output, std::size_t offset, const internal_api::RelationalPropertyOrderingTerm& term) {
  Put(output, offset, term.expression_id, 4);
  Put(output, offset + 4, static_cast<unsigned>(term.direction), 1);
  Put(output, offset + 5, static_cast<unsigned>(term.null_placement), 1);
  Put(output, offset + 6, term.collation_uuid.is_nil() ? 0 : 1, 2);
  PutUuid(output, offset + 8, term.collation_uuid);
}
bool GetOrderingTerm(const std::uint8_t* data, std::size_t offset,
                       internal_api::RelationalPropertyOrderingTerm* output) {
  internal_api::RelationalPropertyOrderingTerm term;
  term.expression_id = Get(data, offset, 4);
  term.direction = static_cast<internal_api::RelationalPropertySortDirection>(data[offset + 4]);
  term.null_placement = static_cast<internal_api::RelationalPropertyNullPlacement>(data[offset + 5]);
  term.collation_uuid = GetUuid(data, offset + 8);
  if (Get(data, offset + 6, 2) != (term.collation_uuid.is_nil() ? 0u : 1u) || !OrderingTermValid(term)) return false;
  if (output) *output = term;
  return true;
}
bool WindowBoundValid(const std::optional<internal_api::RelationalWindowFrameBoundRecord>& bound) noexcept {
  return !bound || (static_cast<unsigned>(bound->bound_kind) >= 1 &&
      static_cast<unsigned>(bound->bound_kind) <= 5 &&
      (!bound->offset_expression_id || *bound->offset_expression_id != 0));
}
void PutWindowBound(Bytes& output, std::size_t offset,
                       const std::optional<internal_api::RelationalWindowFrameBoundRecord>& bound) {
  if (!bound) return;
  Put(output, offset, static_cast<unsigned>(bound->bound_kind), 1);
  Put(output, offset + 1, bound->offset_expression_id ? 1 : 0, 1);
  Put(output, offset + 4, bound->offset_expression_id.value_or(0), 4);
}
bool GetWindowBound(const std::uint8_t* data, std::size_t offset, bool present,
                       std::optional<internal_api::RelationalWindowFrameBoundRecord>* output) {
  if (!present) return Get(data, offset, 8) == 0;
  if (data[offset] < 1 || data[offset] > 5 || data[offset + 1] > 1 || Get(data, offset + 2, 2) ||
      (data[offset + 1] == 1) != (Get(data, offset + 4, 4) != 0)) return false;
  if (output) {
    internal_api::RelationalWindowFrameBoundRecord bound;
    bound.bound_kind = static_cast<internal_api::RelationalWindowFrameBoundKind>(data[offset]);
    if (data[offset + 1]) bound.offset_expression_id = Get(data, offset + 4, 4);
    *output = bound;
  }
  return true;
}
}

namespace {
bool ReadRowPatternVariable(const std::uint8_t* data, std::size_t size, std::size_t* consumed,
                            internal_api::RelationalRowPatternVariableRecord* output) {
  if (size < 20) return false;
  const auto name_size = Get(data, 0, 4), maximum = Get(data, 8, 4), define = Get(data, 12, 4);
  const auto flags = data[16];
  if (!name_size || name_size > size - 20 || (flags & ~15u) || Get(data, 17, 3) ||
      (!(flags & 1) && maximum != 0) || ((flags & 2) != 0) != (define != 0) ||
      !core::datatypes::ValidateCanonicalUtf8(data + 20, name_size)) return false;
  *consumed = 20 + name_size;
  if (output) {
    internal_api::RelationalRowPatternVariableRecord decoded;
    decoded.canonical_name_key.assign(reinterpret_cast<const char*>(data + 20), name_size);
    decoded.minimum_occurrences = Get(data, 4, 4);
    if (flags & 1) decoded.maximum_occurrences = maximum;
    if (flags & 2) decoded.define_expression_id = define;
    decoded.reluctant = (flags & 4) != 0; decoded.define_always_true = (flags & 8) != 0;
    *output = std::move(decoded);
  }
  return true;
}
}

bool ValidateRelationalRowPatternV1(const internal_api::RelationalRowPatternRecord& value) noexcept {
  if (!value.pattern_id || !value.relation_node_id ||
      static_cast<unsigned>(value.rows_per_match) < 1 || static_cast<unsigned>(value.rows_per_match) > 2 ||
      static_cast<unsigned>(value.after_match_skip) < 1 || static_cast<unsigned>(value.after_match_skip) > 4) return false;
  auto remaining = kRelationalRowPatternMaximumBytesV1 - kRelationalRowPatternFixedBytesV1;
  if (value.skip_target_key) {
    if (value.skip_target_key->size() > remaining ||
        !core::datatypes::ValidateCanonicalUtf8(reinterpret_cast<const std::uint8_t*>(value.skip_target_key->data()),
                                               value.skip_target_key->size())) return false;
    remaining -= value.skip_target_key->size();
  }
  if (value.partition_expression_ids.size() > remaining / 4 ||
      std::ranges::find(value.partition_expression_ids, 0) != value.partition_expression_ids.end()) return false;
  remaining -= value.partition_expression_ids.size() * 4;
  if (value.ordering_terms.size() > remaining / 24 || !std::ranges::all_of(value.ordering_terms, OrderingTermValid)) return false;
  remaining -= value.ordering_terms.size() * 24;
  if (value.measure_expression_ids.size() > remaining / 4 ||
      std::ranges::find(value.measure_expression_ids, 0) != value.measure_expression_ids.end()) return false;
  remaining -= value.measure_expression_ids.size() * 4;
  if (value.variables.size() > remaining / 20) return false;
  for (const auto& variable : value.variables) {
    if (remaining < 20 || variable.canonical_name_key.empty() || variable.canonical_name_key.size() > remaining - 20 ||
        (variable.define_expression_id && !*variable.define_expression_id) ||
        !core::datatypes::ValidateCanonicalUtf8(reinterpret_cast<const std::uint8_t*>(variable.canonical_name_key.data()),
                                               variable.canonical_name_key.size())) return false;
    remaining -= 20 + variable.canonical_name_key.size();
  }
  return true;
}

bool EncodeRelationalRowPatternV1(const internal_api::RelationalRowPatternRecord& value, Bytes* output) {
  if (!output || !ValidateRelationalRowPatternV1(value)) return false;
  const auto skip_size = value.skip_target_key ? value.skip_target_key->size() : 0;
  std::size_t size = kRelationalRowPatternFixedBytesV1 + skip_size + value.partition_expression_ids.size() * 4 +
      value.ordering_terms.size() * 24 + value.measure_expression_ids.size() * 4;
  for (const auto& variable : value.variables) size += 20 + variable.canonical_name_key.size();
  Bytes encoded(size, 0);
  Put(encoded, 0, 1, 2);
  Put(encoded, 2, (value.stable_row_identity_tie_break_allowed ? 1 : 0) | (value.skip_target_key ? 2 : 0), 2);
  Put(encoded, 4, value.pattern_id, 4); Put(encoded, 8, value.relation_node_id, 4);
  Put(encoded, 12, value.partition_expression_ids.size(), 4); Put(encoded, 16, value.ordering_terms.size(), 4);
  Put(encoded, 20, value.variables.size(), 4); Put(encoded, 24, value.measure_expression_ids.size(), 4);
  Put(encoded, 28, skip_size, 4); Put(encoded, 32, value.maximum_partition_rows, 4);
  Put(encoded, 36, value.maximum_active_states, 4); Put(encoded, 40, value.maximum_output_rows, 4);
  encoded[44] = static_cast<std::uint8_t>(value.rows_per_match); encoded[45] = static_cast<std::uint8_t>(value.after_match_skip);
  auto cursor = kRelationalRowPatternFixedBytesV1;
  if (value.skip_target_key) std::copy(value.skip_target_key->begin(), value.skip_target_key->end(), encoded.begin() + cursor);
  cursor += skip_size;
  for (const auto id : value.partition_expression_ids) { Put(encoded, cursor, id, 4); cursor += 4; }
  for (const auto& term : value.ordering_terms) { PutOrderingTerm(encoded, cursor, term); cursor += 24; }
  for (const auto& variable : value.variables) {
    Put(encoded, cursor, variable.canonical_name_key.size(), 4); Put(encoded, cursor + 4, variable.minimum_occurrences, 4);
    Put(encoded, cursor + 8, variable.maximum_occurrences.value_or(0), 4);
    Put(encoded, cursor + 12, variable.define_expression_id.value_or(0), 4);
    Put(encoded, cursor + 16, (variable.maximum_occurrences ? 1 : 0) | (variable.define_expression_id ? 2 : 0) |
        (variable.reluctant ? 4 : 0) | (variable.define_always_true ? 8 : 0), 1);
    cursor += 20;
    std::copy(variable.canonical_name_key.begin(), variable.canonical_name_key.end(), encoded.begin() + cursor);
    cursor += variable.canonical_name_key.size();
  }
  for (const auto id : value.measure_expression_ids) { Put(encoded, cursor, id, 4); cursor += 4; }
  output->swap(encoded);
  return true;
}

bool DecodeRelationalRowPatternV1(const std::uint8_t* data, std::size_t size,
                                  internal_api::RelationalRowPatternRecord* output) {
  if (!data || !output || size < kRelationalRowPatternFixedBytesV1 || size > kRelationalRowPatternMaximumBytesV1 ||
      Get(data, 0, 2) != 1 || !Get(data, 4, 4) || !Get(data, 8, 4) || Get(data, 46, 2) ||
      data[44] < 1 || data[44] > 2 || data[45] < 1 || data[45] > 4) return false;
  const auto flags = Get(data, 2, 2), partitions = Get(data, 12, 4), orders = Get(data, 16, 4);
  const auto variables = Get(data, 20, 4), measures = Get(data, 24, 4), skip_size = Get(data, 28, 4);
  auto remaining = size - kRelationalRowPatternFixedBytesV1;
  if ((flags & ~3u) || skip_size > remaining || (!(flags & 2) && skip_size != 0) ||
      !core::datatypes::ValidateCanonicalUtf8(data + kRelationalRowPatternFixedBytesV1, skip_size)) return false;
  remaining -= skip_size;
  if (partitions > remaining / 4) return false;
  remaining -= partitions * 4;
  if (orders > remaining / 24) return false;
  remaining -= orders * 24;
  if (measures > remaining / 4) return false;
  remaining -= measures * 4;
  if (variables > remaining / 20) return false;
  auto cursor = kRelationalRowPatternFixedBytesV1 + skip_size;
  for (std::uint64_t i = 0; i < partitions; ++i, cursor += 4) if (!Get(data, cursor, 4)) return false;
  for (std::uint64_t i = 0; i < orders; ++i, cursor += 24) if (!GetOrderingTerm(data, cursor, nullptr)) return false;
  const auto variable_start = cursor;
  for (std::uint64_t i = 0; i < variables; ++i) {
    std::size_t consumed = 0;
    if (!ReadRowPatternVariable(data + cursor, remaining, &consumed, nullptr)) return false;
    cursor += consumed; remaining -= consumed;
  }
  if (remaining != 0) return false;
  const auto measure_start = cursor;
  for (std::uint64_t i = 0; i < measures; ++i, cursor += 4) if (!Get(data, cursor, 4)) return false;
  internal_api::RelationalRowPatternRecord decoded;
  decoded.pattern_id = Get(data, 4, 4); decoded.relation_node_id = Get(data, 8, 4);
  decoded.stable_row_identity_tie_break_allowed = (flags & 1) != 0;
  decoded.rows_per_match = static_cast<internal_api::RelationalRowPatternRowsPerMatch>(data[44]);
  decoded.after_match_skip = static_cast<internal_api::RelationalRowPatternAfterMatchSkip>(data[45]);
  decoded.maximum_partition_rows = Get(data, 32, 4); decoded.maximum_active_states = Get(data, 36, 4);
  decoded.maximum_output_rows = Get(data, 40, 4);
  if (flags & 2) decoded.skip_target_key.emplace(reinterpret_cast<const char*>(data + kRelationalRowPatternFixedBytesV1), skip_size);
  decoded.partition_expression_ids.reserve(partitions); decoded.ordering_terms.reserve(orders);
  decoded.variables.reserve(variables); decoded.measure_expression_ids.reserve(measures);
  cursor = kRelationalRowPatternFixedBytesV1 + skip_size;
  for (std::uint64_t i = 0; i < partitions; ++i, cursor += 4) decoded.partition_expression_ids.push_back(Get(data, cursor, 4));
  for (std::uint64_t i = 0; i < orders; ++i, cursor += 24) {
    internal_api::RelationalPropertyOrderingTerm term; GetOrderingTerm(data, cursor, &term); decoded.ordering_terms.push_back(term);
  }
  cursor = variable_start;
  for (std::uint64_t i = 0; i < variables; ++i) {
    std::size_t consumed = 0; internal_api::RelationalRowPatternVariableRecord variable;
    ReadRowPatternVariable(data + cursor, measure_start - cursor, &consumed, &variable);
    decoded.variables.push_back(std::move(variable)); cursor += consumed;
  }
  for (std::uint64_t i = 0; i < measures; ++i, cursor += 4) decoded.measure_expression_ids.push_back(Get(data, cursor, 4));
  static_assert(std::is_nothrow_move_assignable_v<internal_api::RelationalRowPatternRecord>);
  *output = std::move(decoded);
  return true;
}

bool ValidateRelationalPropertyV1(const internal_api::RelationalPropertyRecord& value) noexcept {
  const auto optional_uuid = [](const Uuid& id) {
    return id.is_nil() || core::uuid::IsEngineIdentityUuid(id);
  };
  if (!core::uuid::IsEngineIdentityUuid(value.property_uuid) || !value.origin_node_id ||
      static_cast<unsigned>(value.property_kind) < 1 || static_cast<unsigned>(value.property_kind) > 14 ||
      static_cast<unsigned>(value.distribution_kind) > 6 || static_cast<unsigned>(value.materialization_kind) > 3 ||
      static_cast<unsigned>(value.rewindability_kind) > 3 || static_cast<unsigned>(value.locality_kind) > 5 ||
      !optional_uuid(value.window_frame_descriptor_uuid) || !optional_uuid(value.locality_uuid) ||
      !optional_uuid(value.security_visibility_context_uuid)) return false;
  auto remaining = kRelationalPropertyMaximumBytesV1 - kRelationalPropertyFixedBytesV1;
  if (value.expression_ids.size() > remaining / 4 ||
      std::ranges::find(value.expression_ids, 0) != value.expression_ids.end()) return false;
  remaining -= value.expression_ids.size() * 4;
  if (value.ordering_terms.size() > remaining / 24 ||
      !std::ranges::all_of(value.ordering_terms, OrderingTermValid)) return false;
  remaining -= value.ordering_terms.size() * 24;
  return value.dependency_property_uuids.size() <= remaining / 16 &&
      std::ranges::all_of(value.dependency_property_uuids, [](const Uuid& id) {
        return core::uuid::IsEngineIdentityUuid(id);
      });
}

bool EncodeRelationalPropertyV1(const internal_api::RelationalPropertyRecord& value, Bytes* output) {
  if (!output || !ValidateRelationalPropertyV1(value)) return false;
  Bytes encoded(kRelationalPropertyFixedBytesV1 + value.expression_ids.size() * 4 +
      value.ordering_terms.size() * 24 + value.dependency_property_uuids.size() * 16, 0);
  Put(encoded, 0, 1, 2);
  Put(encoded, 2, (value.window_frame_descriptor_uuid.is_nil() ? 0 : 1) |
      (value.locality_uuid.is_nil() ? 0 : 2) | (value.security_visibility_context_uuid.is_nil() ? 0 : 4), 2);
  Put(encoded, 4, value.origin_node_id, 4); PutUuid(encoded, 8, value.property_uuid);
  encoded[24] = static_cast<std::uint8_t>(value.property_kind);
  encoded[25] = static_cast<std::uint8_t>(value.distribution_kind);
  encoded[26] = static_cast<std::uint8_t>(value.materialization_kind);
  encoded[27] = static_cast<std::uint8_t>(value.rewindability_kind);
  encoded[28] = static_cast<std::uint8_t>(value.locality_kind);
  Put(encoded, 32, value.expression_ids.size(), 4); Put(encoded, 36, value.ordering_terms.size(), 4);
  Put(encoded, 40, value.dependency_property_uuids.size(), 4);
  PutUuid(encoded, 48, value.window_frame_descriptor_uuid); PutUuid(encoded, 64, value.locality_uuid);
  PutUuid(encoded, 80, value.security_visibility_context_uuid); Put(encoded, 96, value.security_visibility_generation, 8);
  auto cursor = kRelationalPropertyFixedBytesV1;
  for (const auto id : value.expression_ids) { Put(encoded, cursor, id, 4); cursor += 4; }
  for (const auto& term : value.ordering_terms) { PutOrderingTerm(encoded, cursor, term); cursor += 24; }
  for (const auto& id : value.dependency_property_uuids) { PutUuid(encoded, cursor, id); cursor += 16; }
  output->swap(encoded);
  return true;
}

bool DecodeRelationalPropertyV1(const std::uint8_t* data, std::size_t size,
                                internal_api::RelationalPropertyRecord* output) {
  if (!data || !output || size < kRelationalPropertyFixedBytesV1 || size > kRelationalPropertyMaximumBytesV1 ||
      Get(data, 0, 2) != 1 || !Get(data, 4, 4) || Get(data, 29, 3) || Get(data, 44, 4) ||
      data[24] < 1 || data[24] > 14 || data[25] > 6 || data[26] > 3 || data[27] > 3 || data[28] > 5) return false;
  const auto flags = Get(data, 2, 2), expressions = Get(data, 32, 4), orders = Get(data, 36, 4), dependencies = Get(data, 40, 4);
  if ((flags & ~7u) || !core::uuid::IsEngineIdentityUuid(GetUuid(data, 8))) return false;
  for (unsigned index = 0; index < 3; ++index) {
    const auto id = GetUuid(data, 48 + index * 16);
    if ((flags & (1u << index)) ? !core::uuid::IsEngineIdentityUuid(id) : !id.is_nil()) return false;
  }
  auto remaining = size - kRelationalPropertyFixedBytesV1;
  if (expressions > remaining / 4) return false;
  remaining -= expressions * 4;
  if (orders > remaining / 24) return false;
  remaining -= orders * 24;
  if (dependencies > remaining / 16 || dependencies * 16 != remaining) return false;
  auto cursor = kRelationalPropertyFixedBytesV1;
  for (std::uint64_t i = 0; i < expressions; ++i, cursor += 4) if (!Get(data, cursor, 4)) return false;
  for (std::uint64_t i = 0; i < orders; ++i, cursor += 24) if (!GetOrderingTerm(data, cursor, nullptr)) return false;
  for (std::uint64_t i = 0; i < dependencies; ++i, cursor += 16)
    if (!core::uuid::IsEngineIdentityUuid(GetUuid(data, cursor))) return false;
  internal_api::RelationalPropertyRecord decoded;
  decoded.property_uuid = GetUuid(data, 8); decoded.origin_node_id = Get(data, 4, 4);
  decoded.property_kind = static_cast<internal_api::RelationalPropertyKind>(data[24]);
  decoded.distribution_kind = static_cast<internal_api::RelationalPropertyDistributionKind>(data[25]);
  decoded.materialization_kind = static_cast<internal_api::RelationalPropertyMaterializationKind>(data[26]);
  decoded.rewindability_kind = static_cast<internal_api::RelationalPropertyRewindabilityKind>(data[27]);
  decoded.locality_kind = static_cast<internal_api::RelationalPropertyLocalityKind>(data[28]);
  decoded.window_frame_descriptor_uuid = GetUuid(data, 48); decoded.locality_uuid = GetUuid(data, 64);
  decoded.security_visibility_context_uuid = GetUuid(data, 80); decoded.security_visibility_generation = Get(data, 96, 8);
  decoded.expression_ids.reserve(expressions); decoded.ordering_terms.reserve(orders);
  decoded.dependency_property_uuids.reserve(dependencies);
  cursor = kRelationalPropertyFixedBytesV1;
  for (std::uint64_t i = 0; i < expressions; ++i, cursor += 4) decoded.expression_ids.push_back(Get(data, cursor, 4));
  for (std::uint64_t i = 0; i < orders; ++i, cursor += 24) {
    internal_api::RelationalPropertyOrderingTerm term; GetOrderingTerm(data, cursor, &term); decoded.ordering_terms.push_back(term);
  }
  for (std::uint64_t i = 0; i < dependencies; ++i, cursor += 16) decoded.dependency_property_uuids.push_back(GetUuid(data, cursor));
  static_assert(std::is_nothrow_move_assignable_v<internal_api::RelationalPropertyRecord>);
  *output = std::move(decoded);
  return true;
}

bool ValidateRelationalWindowDefinitionV1(const internal_api::RelationalWindowDefinitionRecord& value) noexcept {
  if (!value.window_id || !value.relation_node_id ||
      (value.inherited_window_id && !*value.inherited_window_id) ||
      (value.frame_unit && (static_cast<unsigned>(*value.frame_unit) < 1 || static_cast<unsigned>(*value.frame_unit) > 3)) ||
      static_cast<unsigned>(value.exclusion) < 1 || static_cast<unsigned>(value.exclusion) > 4 ||
      !WindowBoundValid(value.frame_start) || !WindowBoundValid(value.frame_end)) return false;
  auto remaining = kRelationalWindowDefinitionMaximumBytesV1 - kRelationalWindowDefinitionFixedBytesV1;
  if (value.canonical_name_key) {
    if (value.canonical_name_key->size() > remaining ||
        !core::datatypes::ValidateCanonicalUtf8(reinterpret_cast<const std::uint8_t*>(value.canonical_name_key->data()),
                                               value.canonical_name_key->size())) return false;
    remaining -= value.canonical_name_key->size();
  }
  if (value.partition_expression_ids.size() > remaining / 4 ||
      std::ranges::find(value.partition_expression_ids, 0) != value.partition_expression_ids.end()) return false;
  remaining -= value.partition_expression_ids.size() * 4;
  return value.ordering_terms.size() <= remaining / 24 && std::ranges::all_of(value.ordering_terms, OrderingTermValid);
}

bool EncodeRelationalWindowDefinitionV1(const internal_api::RelationalWindowDefinitionRecord& value, Bytes* output) {
  if (!output || !ValidateRelationalWindowDefinitionV1(value)) return false;
  const auto name_size = value.canonical_name_key ? value.canonical_name_key->size() : 0;
  Bytes encoded(kRelationalWindowDefinitionFixedBytesV1 + name_size + value.partition_expression_ids.size() * 4 +
                value.ordering_terms.size() * 24, 0);
  Put(encoded, 0, 1, 2);
  Put(encoded, 2, (value.canonical_name_key ? 1 : 0) | (value.inherited_window_id ? 2 : 0) |
      (value.frame_unit ? 4 : 0) | (value.frame_start ? 8 : 0) | (value.frame_end ? 16 : 0), 2);
  Put(encoded, 4, value.window_id, 4); Put(encoded, 8, value.relation_node_id, 4);
  Put(encoded, 12, value.inherited_window_id.value_or(0), 4);
  Put(encoded, 16, value.partition_expression_ids.size(), 4); Put(encoded, 20, value.ordering_terms.size(), 4);
  Put(encoded, 24, name_size, 4); Put(encoded, 28, value.frame_unit ? static_cast<unsigned>(*value.frame_unit) : 0, 1);
  Put(encoded, 29, static_cast<unsigned>(value.exclusion), 1);
  PutWindowBound(encoded, 32, value.frame_start); PutWindowBound(encoded, 40, value.frame_end);
  auto cursor = kRelationalWindowDefinitionFixedBytesV1;
  if (value.canonical_name_key) std::copy(value.canonical_name_key->begin(), value.canonical_name_key->end(), encoded.begin() + cursor);
  cursor += name_size;
  for (const auto id : value.partition_expression_ids) { Put(encoded, cursor, id, 4); cursor += 4; }
  for (const auto& term : value.ordering_terms) { PutOrderingTerm(encoded, cursor, term); cursor += 24; }
  output->swap(encoded);
  return true;
}

bool DecodeRelationalWindowDefinitionV1(const std::uint8_t* data, std::size_t size,
                                        internal_api::RelationalWindowDefinitionRecord* output) {
  if (!data || !output || size < kRelationalWindowDefinitionFixedBytesV1 || size > kRelationalWindowDefinitionMaximumBytesV1 ||
      Get(data, 0, 2) != 1 || !Get(data, 4, 4) || !Get(data, 8, 4) || Get(data, 30, 2)) return false;
  const auto flags = Get(data, 2, 2), partitions = Get(data, 16, 4), orders = Get(data, 20, 4), name_size = Get(data, 24, 4);
  if ((flags & ~31u) || ((flags & 2) != 0) != (Get(data, 12, 4) != 0) ||
      ((flags & 4) ? (data[28] < 1 || data[28] > 3) : data[28] != 0) || data[29] < 1 || data[29] > 4 ||
      !GetWindowBound(data, 32, flags & 8, nullptr) || !GetWindowBound(data, 40, flags & 16, nullptr)) return false;
  auto remaining = size - kRelationalWindowDefinitionFixedBytesV1;
  if (name_size > remaining || (!(flags & 1) && name_size != 0) ||
      !core::datatypes::ValidateCanonicalUtf8(data + kRelationalWindowDefinitionFixedBytesV1, name_size)) return false;
  remaining -= name_size;
  if (partitions > remaining / 4) return false;
  remaining -= partitions * 4;
  if (orders > remaining / 24 || orders * 24 != remaining) return false;
  auto cursor = kRelationalWindowDefinitionFixedBytesV1 + name_size;
  for (std::uint64_t i = 0; i < partitions; ++i, cursor += 4) if (!Get(data, cursor, 4)) return false;
  for (std::uint64_t i = 0; i < orders; ++i, cursor += 24) if (!GetOrderingTerm(data, cursor, nullptr)) return false;
  internal_api::RelationalWindowDefinitionRecord decoded;
  decoded.window_id = Get(data, 4, 4); decoded.relation_node_id = Get(data, 8, 4);
  if (flags & 1) decoded.canonical_name_key = std::string(reinterpret_cast<const char*>(data + kRelationalWindowDefinitionFixedBytesV1), name_size);
  if (flags & 2) decoded.inherited_window_id = Get(data, 12, 4);
  if (flags & 4) decoded.frame_unit = static_cast<internal_api::RelationalWindowFrameUnit>(data[28]);
  decoded.exclusion = static_cast<internal_api::RelationalWindowFrameExclusion>(data[29]);
  GetWindowBound(data, 32, flags & 8, &decoded.frame_start); GetWindowBound(data, 40, flags & 16, &decoded.frame_end);
  decoded.partition_expression_ids.reserve(partitions); decoded.ordering_terms.reserve(orders);
  cursor = kRelationalWindowDefinitionFixedBytesV1 + name_size;
  for (std::uint64_t i = 0; i < partitions; ++i, cursor += 4) decoded.partition_expression_ids.push_back(Get(data, cursor, 4));
  for (std::uint64_t i = 0; i < orders; ++i, cursor += 24) {
    internal_api::RelationalPropertyOrderingTerm term; GetOrderingTerm(data, cursor, &term); decoded.ordering_terms.push_back(term);
  }
  static_assert(std::is_nothrow_move_assignable_v<internal_api::RelationalWindowDefinitionRecord>);
  *output = std::move(decoded);
  return true;
}

bool ValidateRelationalWindowInvocationV1(const internal_api::RelationalWindowInvocationRecord& value) noexcept {
  if (!value.invocation_id || !value.relation_node_id || !value.function_expression_id ||
      !value.window_definition_id || !value.result_descriptor_id || !value.function_abi_version ||
      !core::uuid::IsEngineIdentityUuid(value.function_uuid) || !TextValid(value.builtin_id, 256)) return false;
  const auto remaining = kRelationalWindowInvocationMaximumBytesV1 -
      kRelationalWindowInvocationFixedBytesV1 - value.builtin_id.size();
  return !value.output_name_utf8.empty() && value.output_name_utf8.size() <= remaining &&
      core::datatypes::ValidateCanonicalUtf8(reinterpret_cast<const std::uint8_t*>(value.output_name_utf8.data()),
                                             value.output_name_utf8.size()) &&
      value.argument_expression_ids.size() <= (remaining - value.output_name_utf8.size()) / 4 &&
      std::ranges::find(value.argument_expression_ids, 0) == value.argument_expression_ids.end();
}

bool EncodeRelationalWindowInvocationV1(const internal_api::RelationalWindowInvocationRecord& value, Bytes* output) {
  if (!output || !ValidateRelationalWindowInvocationV1(value)) return false;
  Bytes encoded(kRelationalWindowInvocationFixedBytesV1 + value.builtin_id.size() +
      value.output_name_utf8.size() + value.argument_expression_ids.size() * 4, 0);
  Put(encoded, 0, 1, 2); Put(encoded, 4, value.invocation_id, 4);
  Put(encoded, 8, value.relation_node_id, 4); Put(encoded, 12, value.function_expression_id, 4);
  Put(encoded, 16, value.window_definition_id, 4); Put(encoded, 20, value.result_descriptor_id, 4);
  Put(encoded, 24, value.function_abi_version, 2); Put(encoded, 28, value.builtin_id.size(), 4);
  Put(encoded, 32, value.output_name_utf8.size(), 4); Put(encoded, 36, value.argument_expression_ids.size(), 4);
  PutUuid(encoded, 40, value.function_uuid);
  auto cursor = kRelationalWindowInvocationFixedBytesV1;
  std::copy(value.builtin_id.begin(), value.builtin_id.end(), encoded.begin() + cursor);
  cursor += value.builtin_id.size();
  std::copy(value.output_name_utf8.begin(), value.output_name_utf8.end(), encoded.begin() + cursor);
  cursor += value.output_name_utf8.size();
  for (const auto id : value.argument_expression_ids) { Put(encoded, cursor, id, 4); cursor += 4; }
  output->swap(encoded);
  return true;
}

bool DecodeRelationalWindowInvocationV1(const std::uint8_t* data, std::size_t size,
                                        internal_api::RelationalWindowInvocationRecord* output) {
  if (!data || !output || size < kRelationalWindowInvocationFixedBytesV1 ||
      size > kRelationalWindowInvocationMaximumBytesV1 || Get(data, 0, 2) != 1 ||
      Get(data, 2, 2) || Get(data, 26, 2) || !Get(data, 4, 4) || !Get(data, 8, 4) ||
      !Get(data, 12, 4) || !Get(data, 16, 4) || !Get(data, 20, 4) || !Get(data, 24, 2) ||
      !core::uuid::IsEngineIdentityUuid(GetUuid(data, 40))) return false;
  const auto builtin_size = Get(data, 28, 4), output_size = Get(data, 32, 4), count = Get(data, 36, 4);
  auto remaining = size - kRelationalWindowInvocationFixedBytesV1;
  if (builtin_size > remaining || !TextValid(
          {reinterpret_cast<const char*>(data + kRelationalWindowInvocationFixedBytesV1),
           static_cast<std::size_t>(builtin_size)}, 256)) return false;
  remaining -= builtin_size;
  if (!output_size || output_size > remaining ||
      !core::datatypes::ValidateCanonicalUtf8(data + kRelationalWindowInvocationFixedBytesV1 + builtin_size, output_size)) return false;
  remaining -= output_size;
  if (count > remaining / 4 || count * 4 != remaining) return false;
  auto cursor = kRelationalWindowInvocationFixedBytesV1 + builtin_size + output_size;
  for (std::uint64_t index = 0; index < count; ++index, cursor += 4)
    if (!Get(data, cursor, 4)) return false;
  internal_api::RelationalWindowInvocationRecord decoded;
  decoded.invocation_id = Get(data, 4, 4); decoded.relation_node_id = Get(data, 8, 4);
  decoded.function_expression_id = Get(data, 12, 4); decoded.window_definition_id = Get(data, 16, 4);
  decoded.result_descriptor_id = Get(data, 20, 4); decoded.function_abi_version = Get(data, 24, 2);
  decoded.function_uuid = GetUuid(data, 40);
  decoded.builtin_id.assign(reinterpret_cast<const char*>(data + kRelationalWindowInvocationFixedBytesV1), builtin_size);
  decoded.output_name_utf8.assign(reinterpret_cast<const char*>(data + kRelationalWindowInvocationFixedBytesV1 + builtin_size), output_size);
  decoded.argument_expression_ids.reserve(count);
  cursor = kRelationalWindowInvocationFixedBytesV1 + builtin_size + output_size;
  for (std::uint64_t index = 0; index < count; ++index, cursor += 4)
    decoded.argument_expression_ids.push_back(Get(data, cursor, 4));
  static_assert(std::is_nothrow_move_assignable_v<internal_api::RelationalWindowInvocationRecord>);
  *output = std::move(decoded);
  return true;
}

bool ValidateRelationalNodeBindingV1(const RelationalNodeBindingRecord& value) noexcept {
  if (!value.node_id || !TextValid(value.semantic_variant_id, 256)) return false;
  std::size_t remaining = kRelationalNodeBindingMaximumBytesV1 -
      kRelationalNodeBindingFixedBytesV1 - value.semantic_variant_id.size();
  if (value.bound_expression_ids.size() > remaining / 4 ||
      std::ranges::find(value.bound_expression_ids, 0) != value.bound_expression_ids.end()) return false;
  remaining -= value.bound_expression_ids.size() * 4;
  for (const auto* identities : {&value.required_object_uuids, &value.required_property_uuids,
                                  &value.delivered_property_uuids}) {
    if (identities->size() > remaining / 16 ||
        !std::ranges::all_of(*identities, core::uuid::IsEngineIdentityUuid)) return false;
    remaining -= identities->size() * 16;
  }
  return true;
}

bool EncodeRelationalNodeBindingV1(const RelationalNodeBindingRecord& value, Bytes* output) {
  if (!output || !ValidateRelationalNodeBindingV1(value)) return false;
  Bytes encoded(kRelationalNodeBindingFixedBytesV1 + value.semantic_variant_id.size() +
      value.bound_expression_ids.size() * 4 + (value.required_object_uuids.size() +
      value.required_property_uuids.size() + value.delivered_property_uuids.size()) * 16, 0);
  Put(encoded, 0, 1, 2); Put(encoded, 4, value.node_id, 4);
  Put(encoded, 8, value.semantic_variant_id.size(), 4);
  Put(encoded, 12, value.bound_expression_ids.size(), 4);
  Put(encoded, 16, value.required_object_uuids.size(), 4);
  Put(encoded, 20, value.required_property_uuids.size(), 4);
  Put(encoded, 24, value.delivered_property_uuids.size(), 4);
  auto cursor = kRelationalNodeBindingFixedBytesV1;
  std::copy(value.semantic_variant_id.begin(), value.semantic_variant_id.end(), encoded.begin() + cursor);
  cursor += value.semantic_variant_id.size();
  for (const auto handle : value.bound_expression_ids) { Put(encoded, cursor, handle, 4); cursor += 4; }
  for (const auto* identities : {&value.required_object_uuids, &value.required_property_uuids,
                                  &value.delivered_property_uuids})
    for (const auto& identity : *identities) { PutUuid(encoded, cursor, identity); cursor += 16; }
  output->swap(encoded);
  return true;
}

bool DecodeRelationalNodeBindingV1(const std::uint8_t* data, std::size_t size,
                                   RelationalNodeBindingRecord* output) {
  if (!output || !data || size < kRelationalNodeBindingFixedBytesV1 ||
      size > kRelationalNodeBindingMaximumBytesV1 || Get(data, 0, 2) != 1 ||
      Get(data, 2, 2) || Get(data, 28, 4) || !Get(data, 4, 4)) return false;
  const auto variant_size = Get(data, 8, 4);
  const auto expressions = Get(data, 12, 4);
  const std::array<std::uint64_t, 3> counts{Get(data, 16, 4), Get(data, 20, 4), Get(data, 24, 4)};
  auto remaining = size - kRelationalNodeBindingFixedBytesV1;
  if (variant_size > remaining || !TextValid(
          {reinterpret_cast<const char*>(data + kRelationalNodeBindingFixedBytesV1),
           static_cast<std::size_t>(variant_size)}, 256)) return false;
  remaining -= variant_size;
  if (expressions > remaining / 4) return false;
  remaining -= expressions * 4;
  for (const auto count : counts) {
    if (count > remaining / 16) return false;
    remaining -= count * 16;
  }
  if (remaining) return false;
  auto cursor = kRelationalNodeBindingFixedBytesV1 + static_cast<std::size_t>(variant_size);
  for (std::uint64_t index = 0; index < expressions; ++index, cursor += 4)
    if (!Get(data, cursor, 4)) return false;
  for (const auto count : counts)
    for (std::uint64_t index = 0; index < count; ++index, cursor += 16)
      if (!core::uuid::IsEngineIdentityUuid(GetUuid(data, cursor))) return false;

  RelationalNodeBindingRecord decoded;
  decoded.node_id = Get(data, 4, 4);
  decoded.semantic_variant_id.assign(reinterpret_cast<const char*>(data + kRelationalNodeBindingFixedBytesV1), variant_size);
  cursor = kRelationalNodeBindingFixedBytesV1 + variant_size;
  decoded.bound_expression_ids.reserve(expressions);
  for (std::uint64_t index = 0; index < expressions; ++index, cursor += 4)
    decoded.bound_expression_ids.push_back(Get(data, cursor, 4));
  unsigned role = 0;
  for (auto* identities : {&decoded.required_object_uuids, &decoded.required_property_uuids,
                             &decoded.delivered_property_uuids}) {
    identities->reserve(counts[role]);
    for (std::uint64_t index = 0; index < counts[role]; ++index, cursor += 16)
      identities->push_back(GetUuid(data, cursor));
    ++role;
  }
  static_assert(std::is_nothrow_move_assignable_v<RelationalNodeBindingRecord>);
  *output = std::move(decoded);
  return true;
}

bool ValidateRelationalTypeDescriptorV1(const Descriptor& value) noexcept {
  if (value.descriptor_id == 0 ||
      !core::uuid::IsEngineIdentityUuid(value.descriptor_uuid) ||
      !core::uuid::IsEngineIdentityUuid(value.type_uuid) ||
      (value.collation_uuid && !core::uuid::IsEngineIdentityUuid(*value.collation_uuid)) ||
      (value.timezone_profile_id && !TextValid(*value.timezone_profile_id, 1024)) ||
      (value.scale && (!value.precision || *value.scale > *value.precision))) return false;
  const auto nullability = static_cast<std::uint8_t>(value.nullability);
  if (nullability < 1 || nullability > 3) return false;
  if (value.datatype_identity_authoritative) {
    return nullability != 3 && value.descriptor_generation != 0 &&
        value.type_generation != 0 && value.codec_generation != 0 &&
        value.datatype_catalog_generation != 0 && value.datatype_registry_generation != 0 &&
        value.codec_version != 0 && TextValid(value.codec_id, 256) &&
        core::uuid::IsEngineIdentityUuid(value.statement_receipt_uuid) &&
        core::uuid::IsEngineIdentityUuid(value.datatype_catalog_snapshot_uuid);
  }
  return value.descriptor_generation == 0 && value.type_generation == 0 &&
      value.codec_generation == 0 && value.datatype_catalog_generation == 0 &&
      value.datatype_registry_generation == 0 && value.codec_version == 0 &&
      value.codec_id.empty() && value.statement_receipt_uuid.is_nil() &&
      value.datatype_catalog_snapshot_uuid.is_nil();
}

bool EncodeRelationalTypeDescriptorV1(const Descriptor& value, Bytes* output) {
  if (output == nullptr || !ValidateRelationalTypeDescriptorV1(value)) return false;
  const std::size_t timezone_size = value.timezone_profile_id ? value.timezone_profile_id->size() : 0;
  Bytes encoded(kRelationalDescriptorFixedBytesV1 + value.codec_id.size() + timezone_size, 0);
  Put(encoded, 0, 1, 2);
  const std::uint16_t flags = (value.datatype_identity_authoritative ? kAuthority : 0) |
      (value.collation_uuid ? kCollation : 0) | (value.timezone_profile_id ? kTimezone : 0) |
      (value.width ? kWidth : 0) | (value.precision ? kPrecision : 0) | (value.scale ? kScale : 0);
  Put(encoded, 2, flags, 2);
  Put(encoded, 4, value.descriptor_id, 4);
  PutUuid(encoded, 8, value.descriptor_uuid);
  PutUuid(encoded, 24, value.type_uuid);
  if (value.collation_uuid) PutUuid(encoded, 40, *value.collation_uuid);
  PutUuid(encoded, 56, value.statement_receipt_uuid);
  PutUuid(encoded, 72, value.datatype_catalog_snapshot_uuid);
  Put(encoded, 88, value.descriptor_generation, 8);
  Put(encoded, 96, value.type_generation, 8);
  Put(encoded, 104, value.codec_generation, 8);
  Put(encoded, 112, value.datatype_catalog_generation, 8);
  Put(encoded, 120, value.datatype_registry_generation, 8);
  Put(encoded, 128, value.codec_version, 2);
  Put(encoded, 130, static_cast<std::uint8_t>(value.nullability), 1);
  Put(encoded, 132, value.width.value_or(0), 4);
  Put(encoded, 136, value.precision.value_or(0), 4);
  Put(encoded, 140, value.scale.value_or(0), 4);
  Put(encoded, 144, value.codec_id.size(), 4);
  Put(encoded, 148, timezone_size, 4);
  auto cursor = std::copy(value.codec_id.begin(), value.codec_id.end(), encoded.begin() + 152);
  if (value.timezone_profile_id)
    std::copy(value.timezone_profile_id->begin(), value.timezone_profile_id->end(), cursor);
  output->swap(encoded);
  return true;
}

bool DecodeRelationalTypeDescriptorV1(const std::uint8_t* data, std::size_t size, Descriptor* output) {
  if (output == nullptr || data == nullptr || size < kRelationalDescriptorFixedBytesV1 ||
      size > kRelationalDescriptorMaximumBytesV1 || Get(data, 0, 2) != 1 || data[131] != 0) return false;
  const auto flags = Get(data, 2, 2);
  const auto codec_size = Get(data, 144, 4), timezone_size = Get(data, 148, 4);
  if ((flags & ~std::uint64_t{63}) != 0 || codec_size > 256 || timezone_size > 1024 ||
      size != 152 + codec_size + timezone_size ||
      (!(flags & kTimezone) && timezone_size != 0) ||
      (!(flags & kCollation) && !GetUuid(data, 40).is_nil()) ||
      (!(flags & kWidth) && Get(data, 132, 4) != 0) ||
      (!(flags & kPrecision) && Get(data, 136, 4) != 0) ||
      (!(flags & kScale) && Get(data, 140, 4) != 0)) return false;
  Descriptor decoded;
  decoded.descriptor_id = static_cast<std::uint32_t>(Get(data, 4, 4));
  decoded.descriptor_uuid = GetUuid(data, 8);
  decoded.type_uuid = GetUuid(data, 24);
  if (flags & kCollation) decoded.collation_uuid = GetUuid(data, 40);
  decoded.statement_receipt_uuid = GetUuid(data, 56);
  decoded.datatype_catalog_snapshot_uuid = GetUuid(data, 72);
  decoded.descriptor_generation = Get(data, 88, 8);
  decoded.type_generation = Get(data, 96, 8);
  decoded.codec_generation = Get(data, 104, 8);
  decoded.datatype_catalog_generation = Get(data, 112, 8);
  decoded.datatype_registry_generation = Get(data, 120, 8);
  decoded.codec_version = static_cast<std::uint16_t>(Get(data, 128, 2));
  decoded.nullability = static_cast<internal_api::RelationalNullability>(data[130]);
  decoded.datatype_identity_authoritative = (flags & kAuthority) != 0;
  if (flags & kWidth) decoded.width = static_cast<std::uint32_t>(Get(data, 132, 4));
  if (flags & kPrecision) decoded.precision = static_cast<std::uint32_t>(Get(data, 136, 4));
  if (flags & kScale) decoded.scale = static_cast<std::uint32_t>(Get(data, 140, 4));
  decoded.codec_id.assign(reinterpret_cast<const char*>(data + 152), static_cast<std::size_t>(codec_size));
  if (flags & kTimezone)
    decoded.timezone_profile_id.emplace(reinterpret_cast<const char*>(data + 152 + codec_size),
                                        static_cast<std::size_t>(timezone_size));
  if (!ValidateRelationalTypeDescriptorV1(decoded)) return false;
  static_assert(std::is_nothrow_move_assignable_v<Descriptor>);
  *output = std::move(decoded);
  return true;
}
bool ValidateRelationalExpressionV1(const internal_api::RelationalExpressionRecord& value) noexcept {
  const auto kind = static_cast<std::uint8_t>(value.expression_kind);
  if (value.expression_id == 0 || value.result_descriptor_id == 0 || kind < 1 || kind > 7 ||
      value.literal_typed_value_v1 || value.parameter_typed_value_v1 || value.contextual_text_literal_v2 ||
      (value.function_uuid && !core::uuid::IsEngineIdentityUuid(*value.function_uuid)) ||
      (value.bound_name_uuid && !core::uuid::IsEngineIdentityUuid(*value.bound_name_uuid)) ||
      (value.literal_kind && (static_cast<std::uint8_t>(*value.literal_kind) < 1 ||
                              static_cast<std::uint8_t>(*value.literal_kind) > 12)) ||
      (value.operator_name && !TextValid(*value.operator_name, 256)) ||
      value.child_expression_ids.size() > (kRelationalExpressionMaximumBytesV1 - 60) / 4 ||
      std::ranges::any_of(value.child_expression_ids, [](auto id) { return id == 0; })) return false;
  auto remaining = kRelationalExpressionMaximumBytesV1 - 60 - value.child_expression_ids.size() * 4;
  const auto operator_size = value.operator_name ? value.operator_name->size() : 0;
  if (operator_size > remaining) return false;
  remaining -= operator_size;
  return !value.literal_or_parameter_ref || value.literal_or_parameter_ref->size() <= remaining;
}

bool EncodeRelationalExpressionV1(const internal_api::RelationalExpressionRecord& value, Bytes* output) {
  if (output == nullptr || !ValidateRelationalExpressionV1(value)) return false;
  const auto operator_size = value.operator_name ? value.operator_name->size() : 0;
  const auto reference_size = value.literal_or_parameter_ref ? value.literal_or_parameter_ref->size() : 0;
  Bytes encoded(60 + value.child_expression_ids.size() * 4 + operator_size + reference_size, 0);
  const auto flags = (value.function_uuid ? 1u : 0u) | (value.bound_name_uuid ? 2u : 0u) |
      (value.literal_kind ? 4u : 0u) | (value.operator_name ? 8u : 0u) |
      (value.literal_or_parameter_ref ? 16u : 0u);
  Put(encoded, 0, 1, 2);
  Put(encoded, 2, flags, 2);
  Put(encoded, 4, value.expression_id, 4);
  encoded[8] = static_cast<std::uint8_t>(value.expression_kind);
  if (value.literal_kind) encoded[9] = static_cast<std::uint8_t>(*value.literal_kind);
  Put(encoded, 12, value.result_descriptor_id, 4);
  Put(encoded, 16, value.child_expression_ids.size(), 4);
  Put(encoded, 20, operator_size, 4);
  Put(encoded, 24, reference_size, 4);
  if (value.function_uuid) PutUuid(encoded, 28, *value.function_uuid);
  if (value.bound_name_uuid) PutUuid(encoded, 44, *value.bound_name_uuid);
  std::size_t offset = 60;
  for (const auto child : value.child_expression_ids) { Put(encoded, offset, child, 4); offset += 4; }
  if (value.operator_name) {
    std::copy(value.operator_name->begin(), value.operator_name->end(), encoded.begin() + offset);
    offset += operator_size;
  }
  if (value.literal_or_parameter_ref)
    std::copy(value.literal_or_parameter_ref->begin(), value.literal_or_parameter_ref->end(), encoded.begin() + offset);
  *output = std::move(encoded);
  return true;
}

bool DecodeRelationalExpressionV1(const std::uint8_t* data, std::size_t size,
                                  internal_api::RelationalExpressionRecord* output) {
  if (data == nullptr || output == nullptr || size < 60 || size > kRelationalExpressionMaximumBytesV1 ||
      Get(data, 0, 2) != 1 || Get(data, 10, 2) != 0) return false;
  const auto flags = Get(data, 2, 2);
  const auto children = Get(data, 16, 4), operator_size = Get(data, 20, 4), reference_size = Get(data, 24, 4);
  if ((flags & ~31u) != 0 || children > (size - 60) / 4 || operator_size > 256 ||
      60 + children * 4 + operator_size + reference_size != size ||
      (!(flags & 4u) && data[9] != 0) || (!(flags & 8u) && operator_size != 0) ||
      (!(flags & 16u) && reference_size != 0)) return false;
  const auto function = GetUuid(data, 28), name = GetUuid(data, 44);
  if ((flags & 1u) ? !core::uuid::IsEngineIdentityUuid(function) : !function.is_nil()) return false;
  if ((flags & 2u) ? !core::uuid::IsEngineIdentityUuid(name) : !name.is_nil()) return false;
  if (Get(data, 4, 4) == 0 || Get(data, 12, 4) == 0 || data[8] < 1 || data[8] > 7 ||
      ((flags & 4u) && (data[9] < 1 || data[9] > 12))) return false;
  const auto operator_offset = static_cast<std::size_t>(60 + children * 4);
  if ((flags & 8u) && !TextValid(std::string_view(reinterpret_cast<const char*>(data + operator_offset),
                                                operator_size), 256)) return false;
  for (std::size_t index = 0; index < children; ++index)
    if (Get(data, 60 + index * 4, 4) == 0) return false;
  internal_api::RelationalExpressionRecord decoded;
  decoded.expression_id = static_cast<std::uint32_t>(Get(data, 4, 4));
  decoded.expression_kind = static_cast<internal_api::RelationalExpressionKind>(data[8]);
  decoded.result_descriptor_id = static_cast<std::uint32_t>(Get(data, 12, 4));
  if (flags & 1u) decoded.function_uuid = function;
  if (flags & 2u) decoded.bound_name_uuid = name;
  if (flags & 4u) decoded.literal_kind = static_cast<internal_api::RelationalLiteralKind>(data[9]);
  decoded.child_expression_ids.reserve(static_cast<std::size_t>(children));
  for (std::size_t index = 0; index < children; ++index)
    decoded.child_expression_ids.push_back(static_cast<std::uint32_t>(Get(data, 60 + index * 4, 4)));
  if (flags & 8u) decoded.operator_name.emplace(reinterpret_cast<const char*>(data + operator_offset), operator_size);
  if (flags & 16u) decoded.literal_or_parameter_ref.emplace(
      reinterpret_cast<const char*>(data + operator_offset + operator_size), reference_size);
  static_assert(std::is_nothrow_move_assignable_v<internal_api::RelationalExpressionRecord>);
  *output = std::move(decoded);
  return true;
}
}  // namespace scratchbird::engine::sblr
