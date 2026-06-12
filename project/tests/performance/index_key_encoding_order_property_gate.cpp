// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_key_encoding.hpp"
#include "index_ordered_access.hpp"
#include "page_extent_summary.hpp"
#include "sorted_bulk_index_build.hpp"
#include "time_range_summary_pruning.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

namespace {

enum class LogicalKind {
  signed_i64,
  text,
  uuid_v7
};

struct LogicalComponent {
  LogicalKind logical_kind = LogicalKind::text;
  idx::IndexKeyComponentKind index_kind = idx::IndexKeyComponentKind::scalar;
  idx::IndexKeySortDirection direction = idx::IndexKeySortDirection::ascending;
  idx::IndexKeyNullPlacement null_placement = idx::IndexKeyNullPlacement::nulls_last;
  bool is_null = false;
  bool case_folded = false;
  std::int64_t signed_value = 0;
  std::string text_value;
  platform::TypedUuid uuid_value;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "index_key_encoding_order_property_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

int Sign(int value) {
  return (value > 0) - (value < 0);
}

platform::TypedUuid TypedUuid(platform::UuidKind kind, platform::byte seed) {
  platform::TypedUuid typed;
  typed.kind = kind;
  for (std::size_t i = 0; i < typed.value.bytes.size(); ++i) {
    typed.value.bytes[i] = static_cast<platform::byte>(seed + i + 1);
  }
  typed.value.bytes[6] = static_cast<platform::byte>((typed.value.bytes[6] & 0x0f) | 0x70);
  typed.value.bytes[8] = static_cast<platform::byte>((typed.value.bytes[8] & 0x3f) | 0x80);
  return typed;
}

platform::TypedUuid UuidV7(platform::UuidKind kind,
                           platform::u64 millis,
                           platform::byte suffix) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(kind, generated.value);
  Require(typed.ok(), "typed uuidv7 creation failed");
  return typed.value;
}

std::vector<platform::byte> EncodeSignedI64Payload(std::int64_t value) {
  const auto sortable = static_cast<std::uint64_t>(value) ^ 0x8000000000000000ull;
  std::vector<platform::byte> out(8);
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(7 - i)] =
        static_cast<platform::byte>((sortable >> (i * 8)) & 0xffu);
  }
  return out;
}

std::vector<platform::byte> Bytes(std::string_view value) {
  return {value.begin(), value.end()};
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

idx::IndexKeyEncodingComponent Component(const LogicalComponent& logical,
                                         platform::u32 ordinal) {
  idx::IndexKeyEncodingComponent component;
  component.kind = logical.index_kind;
  component.ordinal = ordinal;
  component.type_descriptor_uuid =
      TypedUuid(platform::UuidKind::object, static_cast<platform::byte>(0x20 + ordinal));
  component.sort_direction = logical.direction;
  component.null_placement = logical.null_placement;
  component.is_null = logical.is_null;
  component.case_folded = logical.case_folded;
  if (logical.is_null) {
    return component;
  }
  switch (logical.logical_kind) {
    case LogicalKind::signed_i64:
      component.payload = EncodeSignedI64Payload(logical.signed_value);
      break;
    case LogicalKind::text:
      component.payload = Bytes(logical.text_value);
      break;
    case LogicalKind::uuid_v7:
      component.kind = idx::IndexKeyComponentKind::uuid_v7;
      component.uuid_v7_value = logical.uuid_value;
      break;
  }
  return component;
}

int NullRank(const LogicalComponent& component) {
  if (!component.is_null) {
    return 1;
  }
  return component.null_placement == idx::IndexKeyNullPlacement::nulls_first ? 0 : 2;
}

int CompareValue(const LogicalComponent& left, const LogicalComponent& right) {
  switch (left.logical_kind) {
    case LogicalKind::signed_i64:
      return left.signed_value < right.signed_value ? -1 :
             (right.signed_value < left.signed_value ? 1 : 0);
    case LogicalKind::text: {
      const auto l = left.case_folded ? LowerAscii(left.text_value) : left.text_value;
      const auto r = right.case_folded ? LowerAscii(right.text_value) : right.text_value;
      return l < r ? -1 : (r < l ? 1 : 0);
    }
    case LogicalKind::uuid_v7:
      return Sign(uuid::CompareUuid128(left.uuid_value.value, right.uuid_value.value));
  }
  return 0;
}

int LogicalCompare(const std::vector<LogicalComponent>& left,
                   const std::vector<LogicalComponent>& right) {
  Require(left.size() == right.size(), "logical compare shape mismatch");
  for (std::size_t i = 0; i < left.size(); ++i) {
    int cmp = NullRank(left[i]) - NullRank(right[i]);
    if (cmp == 0 && !left[i].is_null && !right[i].is_null) {
      cmp = CompareValue(left[i], right[i]);
      if (left[i].direction == idx::IndexKeySortDirection::descending) {
        cmp = -cmp;
      }
    }
    if (cmp != 0) {
      return Sign(cmp);
    }
  }
  return 0;
}

std::vector<idx::IndexKeyEncodingComponent> Components(
    const std::vector<LogicalComponent>& logical) {
  std::vector<idx::IndexKeyEncodingComponent> components;
  for (std::size_t i = 0; i < logical.size(); ++i) {
    components.push_back(Component(logical[i], static_cast<platform::u32>(i)));
  }
  return components;
}

idx::IndexKeySemanticProfile StableProfile() {
  idx::IndexKeySemanticProfile profile;
  profile.profile_id = "index_key_encoding_order_property_gate";
  profile.bytewise_stable = true;
  return profile;
}

void RequirePair(const std::vector<LogicalComponent>& left,
                 const std::vector<LogicalComponent>& right,
                 std::string_view label) {
  const auto expected = LogicalCompare(left, right);
  const auto encoded_left = idx::EncodeIndexKey(Components(left), StableProfile());
  const auto encoded_right = idx::EncodeIndexKey(Components(right), StableProfile());
  Require(encoded_left.ok() && encoded_right.ok(), label);
  Require(idx::IsOrderPreservingIndexKeyEncoding(
              std::string_view(reinterpret_cast<const char*>(encoded_left.encoded.data()),
                               encoded_left.encoded.size())),
          "encoded key did not use order-preserving marker");
  const auto compare =
      idx::CompareEncodedIndexKeys(encoded_left.encoded, encoded_right.encoded);
  Require(compare.ok(), label);
  Require(Sign(compare.comparison) == expected, label);
  const int direct_lex =
      std::lexicographical_compare(encoded_left.encoded.begin(), encoded_left.encoded.end(),
                                   encoded_right.encoded.begin(), encoded_right.encoded.end()) ? -1 :
      (std::lexicographical_compare(encoded_right.encoded.begin(), encoded_right.encoded.end(),
                                    encoded_left.encoded.begin(), encoded_left.encoded.end()) ? 1 : 0);
  Require(Sign(direct_lex) == expected, "direct encoded byte order mismatch");
}

LogicalComponent Number(std::int64_t value,
                        idx::IndexKeySortDirection direction =
                            idx::IndexKeySortDirection::ascending) {
  LogicalComponent component;
  component.logical_kind = LogicalKind::signed_i64;
  component.index_kind = idx::IndexKeyComponentKind::scalar;
  component.signed_value = value;
  component.direction = direction;
  return component;
}

LogicalComponent Text(std::string value,
                      idx::IndexKeySortDirection direction =
                          idx::IndexKeySortDirection::ascending,
                      bool case_folded = false) {
  LogicalComponent component;
  component.logical_kind = LogicalKind::text;
  component.index_kind = case_folded ? idx::IndexKeyComponentKind::collation_key
                                     : idx::IndexKeyComponentKind::text_token;
  component.text_value = std::move(value);
  component.direction = direction;
  component.case_folded = case_folded;
  return component;
}

LogicalComponent ExpressionText(std::string value) {
  auto component = Text(std::move(value));
  component.index_kind = idx::IndexKeyComponentKind::expression;
  return component;
}

LogicalComponent Null(idx::IndexKeyNullPlacement placement,
                      idx::IndexKeySortDirection direction =
                          idx::IndexKeySortDirection::ascending) {
  LogicalComponent component;
  component.logical_kind = LogicalKind::text;
  component.index_kind = idx::IndexKeyComponentKind::scalar;
  component.is_null = true;
  component.null_placement = placement;
  component.direction = direction;
  return component;
}

LogicalComponent V7(platform::u64 millis,
                    platform::byte suffix,
                    idx::IndexKeySortDirection direction =
                        idx::IndexKeySortDirection::ascending) {
  LogicalComponent component;
  component.logical_kind = LogicalKind::uuid_v7;
  component.index_kind = idx::IndexKeyComponentKind::uuid_v7;
  component.uuid_value = UuidV7(platform::UuidKind::row, millis, suffix);
  component.direction = direction;
  return component;
}

void TargetedRepresentativePairs() {
  RequirePair({Null(idx::IndexKeyNullPlacement::nulls_first)}, {Text("a")},
              "nulls first did not order before non-null");
  RequirePair({Null(idx::IndexKeyNullPlacement::nulls_last)}, {Text("a")},
              "nulls last did not order after non-null");
  RequirePair({Number(-10)}, {Number(4)}, "signed ascending numeric mismatch");
  RequirePair({Number(-10, idx::IndexKeySortDirection::descending)}, {Number(4, idx::IndexKeySortDirection::descending)},
              "signed descending numeric mismatch");
  RequirePair({Text("aa")}, {Text("b")}, "variable-length aa/b text mismatch");
  RequirePair({Text("a")}, {Text("aa")}, "prefix-ish text mismatch");
  RequirePair({Text("Alpha", idx::IndexKeySortDirection::ascending, true)},
              {Text("alpha", idx::IndexKeySortDirection::ascending, true)},
              "case-folded equality mismatch");
  RequirePair({V7(1710000000100ull, 0x01)}, {V7(1710000000200ull, 0x01)},
              "uuidv7 payload ordering mismatch");
  RequirePair({Text("tenant-1"), Number(7)}, {Text("tenant-1"), Number(9)},
              "composite key ordering mismatch");
  RequirePair({ExpressionText("expr:0007")}, {ExpressionText("expr:0010")},
              "expression-like key ordering mismatch");
}

void DeterministicFuzzPairs() {
  std::mt19937_64 rng(0x1fc004005006ull);
  for (int i = 0; i < 400; ++i) {
    const auto a = static_cast<std::int64_t>(rng() % 200000) - 100000;
    const auto b = static_cast<std::int64_t>(rng() % 200000) - 100000;
    RequirePair({Number(a), Text("k" + std::to_string(rng() % 97))},
                {Number(b), Text("k" + std::to_string(rng() % 97))},
                "numeric/text composite fuzz mismatch");
    const std::string left = std::string((rng() % 4) + 1, static_cast<char>('a' + (rng() % 4)));
    const std::string right = std::string((rng() % 4) + 1, static_cast<char>('a' + (rng() % 4)));
    RequirePair({Text(left, idx::IndexKeySortDirection::descending)},
                {Text(right, idx::IndexKeySortDirection::descending)},
                "descending text fuzz mismatch");
  }
}

std::string AsString(const std::vector<platform::byte>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool StartsWithBytes(const std::vector<platform::byte>& value,
                     const std::vector<platform::byte>& prefix) {
  return value.size() >= prefix.size() &&
         std::equal(prefix.begin(), prefix.end(), value.begin());
}

void PrefixBoundsAreGeneratedAndOrderSafe() {
  const auto prefix_components = Components({Text(std::string("a\0", 2))});
  const auto matcher =
      idx::BuildEncodedPrefixMatcher(prefix_components, StableProfile());
  Require(matcher.ok(), "encoded prefix matcher builder failed");
  Require(!matcher.matcher_prefix.empty(), "prefix matcher bytes missing");
  Require(matcher.lower_bound == matcher.matcher_prefix,
          "prefix lower bound did not match matcher prefix");
  Require(!matcher.upper_bound_unbounded && !matcher.upper_bound.empty(),
          "prefix upper bound was unexpectedly unbounded");

  const auto lower =
      idx::BuildEncodedPrefixLowerBound(prefix_components, StableProfile());
  Require(lower.ok() && lower.lower_bound == matcher.lower_bound,
          "prefix lower-bound builder drifted from matcher");
  const auto upper =
      idx::BuildEncodedPrefixUpperBound(prefix_components, StableProfile());
  Require(upper.ok() && upper.upper_bound == matcher.upper_bound,
          "prefix upper-bound builder drifted from matcher");

  const auto matching = idx::EncodeIndexKey(
      Components({Text(std::string("a\0z", 3))}), StableProfile());
  const auto outside = idx::EncodeIndexKey(
      Components({Text(std::string("a\1", 2))}), StableProfile());
  Require(matching.ok() && outside.ok(), "prefix target encoding failed");
  Require(StartsWithBytes(matching.encoded, matcher.matcher_prefix),
          "generated matcher did not match escaped zero prefix");
  Require(!StartsWithBytes(outside.encoded, matcher.matcher_prefix),
          "generated matcher accepted outside prefix");

  const auto lower_compare =
      idx::CompareEncodedIndexKeys(matcher.lower_bound, matching.encoded);
  const auto upper_compare =
      idx::CompareEncodedIndexKeys(matching.encoded, matcher.upper_bound);
  Require(lower_compare.ok() && lower_compare.comparison <= 0,
          "prefix lower bound sorted after matching key");
  Require(upper_compare.ok() && upper_compare.comparison < 0,
          "prefix upper bound did not sort after matching key");
}

void RefusalAndUnsafeLegacyGuards() {
  const std::vector<platform::byte> legacy = {'S', 'B', 'K', '1', 1, 0, 0, 0};
  const auto legacy_compare = idx::CompareEncodedIndexKeys(legacy, legacy);
  Require(!legacy_compare.ok() &&
              legacy_compare.diagnostic.diagnostic_code ==
                  "SB-INDEX-KEY-COMPARE-UNSAFE-LEGACY-ENVELOPE",
          "legacy SBK1 compare was not refused exactly");

  auto reference_raw = Component(Text("raw"), 0);
  reference_raw.kind = idx::IndexKeyComponentKind::reference_raw;
  const auto reference_raw_result = idx::EncodeIndexKey({reference_raw}, StableProfile());
  Require(!reference_raw_result.ok() &&
              reference_raw_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-KEY-ENCODING-REFERENCE-RAW-REFUSED",
          "reference raw key was not refused exactly");

  auto reference_nulls = Component(Null(idx::IndexKeyNullPlacement::reference_profile_default), 0);
  const auto reference_nulls_result = idx::EncodeIndexKey({reference_nulls}, StableProfile());
  Require(!reference_nulls_result.ok() &&
              reference_nulls_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-KEY-ENCODING-REFERENCE-NULLS-REFUSED",
          "reference profile default null placement was not refused exactly");

  auto unstable = StableProfile();
  unstable.bytewise_stable = false;
  const auto unstable_result = idx::EncodeIndexKey(Components({Text("a")}), unstable);
  Require(!unstable_result.ok() &&
              unstable_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-KEY-ENCODING-UNSTABLE-PROFILE",
          "unstable profile was not refused exactly");

  idx::PageExtentSummaryScalarBoundary boundary;
  boundary.scalar_type_key = "time";
  boundary.encoded_min = AsString(legacy);
  boundary.encoded_max = AsString(legacy);
  boundary.min_present = true;
  boundary.max_present = true;
  Require(!idx::PageExtentSummaryBoundaryValid(boundary, 1, 0),
          "page extent summary trusted unsafe legacy key bytes");

  idx::TimeRangeSummaryDescriptor descriptor;
  descriptor.range.page_count = 1;
  descriptor.row_count = 1;
  descriptor.time_scalar_type_key = "time";
  descriptor.encoded_min_time = AsString(legacy);
  descriptor.encoded_max_time = AsString(legacy);
  descriptor.min_time_present = true;
  descriptor.max_time_present = true;
  Require(!idx::TimeRangeSummaryDescriptorBoundsValid(descriptor),
          "time range summary descriptor trusted unsafe legacy key bytes");

  idx::TimeRangeSummaryPruneRequest prune;
  prune.summaries.push_back(descriptor);
  prune.predicate.lower_present = true;
  prune.predicate.encoded_lower_time = AsString(legacy);
  const auto prune_plan = idx::PlanTimeRangeSummaryPrune(prune);
  Require(!prune_plan.ok() &&
              prune_plan.diagnostic.diagnostic_code ==
                  "INDEX.TIME_RANGE_SUMMARY.UNSAFE_LEGACY_KEY_EXACT_FALLBACK",
          "time range prune predicate did not refuse unsafe legacy key bytes");

  idx::SortedBulkIndexBuildRequest bulk;
  bulk.metadata.index_uuid = TypedUuid(platform::UuidKind::object, 0x90);
  bulk.metadata.table_uuid = TypedUuid(platform::UuidKind::object, 0x91);
  bulk.rows.push_back({AsString(legacy), "row-1", "version-1", "payload", 0});
  const auto bulk_result = idx::BuildSortedExactBulkIndex(bulk);
  Require(!bulk_result.ok() &&
              bulk_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-SORTED-BULK-UNSAFE-LEGACY-KEY",
          "sorted bulk build trusted unsafe legacy key bytes");
}

void OrderedAccessPlansUseFreshEncoding() {
  idx::OrderedAccessRequest range;
  range.family = idx::IndexFamily::btree;
  range.intent = idx::OrderedAccessIntent::range;
  range.semantic_profile = StableProfile();
  range.allow_fallback_sort = true;
  range.lower_bound.kind = idx::OrderedBoundKind::inclusive;
  range.lower_bound.components = Components({Text("a")});
  range.upper_bound.kind = idx::OrderedBoundKind::inclusive;
  range.upper_bound.components = Components({Text("aa")});
  const auto range_plan = idx::PlanOrderedBTreeAccess(range);
  Require(range_plan.ok(), "ordered range plan did not admit fresh encoded bounds");
  Require(idx::IsOrderPreservingIndexKeyEncoding(
              std::string_view(reinterpret_cast<const char*>(range_plan.lower_key.encoded.data()),
                               range_plan.lower_key.encoded.size())) &&
              idx::IsOrderPreservingIndexKeyEncoding(
                  std::string_view(reinterpret_cast<const char*>(range_plan.upper_key.encoded.data()),
                                   range_plan.upper_key.encoded.size())),
          "ordered range plan did not use SBKO bounds");

  idx::OrderedAccessRequest prefix = range;
  prefix.intent = idx::OrderedAccessIntent::prefix;
  prefix.lower_bound.kind = idx::OrderedBoundKind::prefix;
  prefix.upper_bound.kind = idx::OrderedBoundKind::unbounded;
  const auto prefix_plan = idx::PlanOrderedBTreeAccess(prefix);
  Require(prefix_plan.ok() && prefix_plan.prefix_exact,
          "ordered prefix plan did not use fresh encoded prefix bounds");
  Require(prefix_plan.prefix_lower_bound_generated,
          "ordered prefix plan did not record generated lower prefix bound");
  Require(!prefix_plan.prefix_upper_bound_generated &&
              !prefix_plan.prefix_upper_bound_unbounded,
          "ordered prefix plan recorded an upper prefix bound for unbounded request");
  Require(!prefix_plan.prefix_matcher.empty() &&
              prefix_plan.prefix_matcher == prefix_plan.lower_key.encoded,
          "ordered prefix plan did not publish matcher bytes");
  Require(prefix_plan.lower_key.encoded.size() < range_plan.lower_key.encoded.size() ||
              !StartsWithBytes(prefix_plan.lower_key.encoded, range_plan.lower_key.encoded),
          "ordered prefix plan used a terminated full-key bound");
}

}  // namespace

int main() {
  TargetedRepresentativePairs();
  DeterministicFuzzPairs();
  PrefixBoundsAreGeneratedAndOrderSafe();
  OrderedAccessPlansUseFreshEncoding();
  RefusalAndUnsafeLegacyGuards();
  std::cout << "index_key_encoding_order_property_gate=passed\n";
  return 0;
}
