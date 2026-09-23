// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "index_apply_planner.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <cstdio>
#include <cstdlib>

namespace idx = scratchbird::core::index;

static void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(EXIT_FAILURE);
  }
}

int main() {
  idx::CommitGroupLocalityIndexApplyItem item;
  item.index_uuid = scratchbird::tests::FixtureUuid(6043, 1);
  item.family = "btree";
  item.profile = "rowstore_scalar_btree_v1";
  item.target_keys = {std::string("key\0payload", 11)};
  for (const bool unique : {false, true}) {
    item.unique = unique;
    const auto base = idx::CommitGroupLocalityTargetKey(item);
    Check(base.index_uuid == item.index_uuid && base.unique_order == unique,
          "locality key lost its binary identity or ordering policy");
    std::vector<idx::CommitGroupLocalityIndexApplyItem> items{item};
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto changed = item;
      changed.index_uuid.bytes[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
      Check(idx::CommitGroupLocalityTargetKey(changed) != base,
            "locality identity ignored a UUID bit");
      items.push_back(std::move(changed));
    }
    const auto plan = idx::PlanCommitGroupLocalityIndexApply(items);
    Check(plan.accepted && plan.groups.size() == items.size() &&
              plan.locality_group_count == items.size(),
          "different index identities merged into one locality group");
  }

  item.unique = true;
  item.source_row_ordinal = 9;
  auto earlier = item;
  earlier.source_row_ordinal = 2;
  const auto ordered = idx::PlanCommitGroupLocalityIndexApply({item, earlier});
  Check(ordered.accepted && ordered.groups.size() == 1 &&
            ordered.groups[0].item_ordinals == std::vector<std::size_t>{1, 0},
        "unique index source order was not preserved");

  item.unique = false;
  item.index_uuid.bytes[0] = 0;
  item.index_uuid.bytes[1] = '|';
  item.index_uuid.bytes[2] = ':';
  item.index_uuid.bytes[3] = '\n';
  const auto raw = idx::PlanCommitGroupLocalityIndexApply({item});
  Check(raw.accepted && raw.groups[0].target_leaf_page_locality_key.index_uuid == item.index_uuid,
        "binary delimiter or zero octets changed the index identity");

  auto other = item;
  item.family = "a|b";
  item.profile = "c";
  other.family = "a";
  other.profile = "b|c";
  const auto bucket = idx::CommitGroupLocalityTargetKey(item).bucket;
  bool matching_bucket = false;
  for (unsigned attempt = 0; attempt < 4096; ++attempt) {
    other.target_keys = {std::to_string(attempt)};
    if (idx::CommitGroupLocalityTargetKey(other).bucket == bucket) {
      matching_bucket = true;
      break;
    }
  }
  Check(matching_bucket, "could not construct the group-key collision fixture");
  const auto separated = idx::PlanCommitGroupLocalityIndexApply({item, other});
  Check(separated.accepted && separated.groups.size() == 2,
        "family/profile delimiters aliased different grouping fields");

  item.index_uuid = {};
  const auto nil = idx::PlanCommitGroupLocalityIndexApply({item});
  Check(!nil.accepted && nil.refusal_reason == "index_uuid_required",
        "nil index identity was admitted");
}
