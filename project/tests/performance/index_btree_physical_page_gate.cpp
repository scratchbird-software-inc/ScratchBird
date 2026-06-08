// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_btree_page.hpp"
#include "index_key_encoding.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace datatypes = scratchbird::core::datatypes;
namespace idx = scratchbird::core::index;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

namespace {

inline constexpr platform::u32 kOffsetBodyBytes = 16;
inline constexpr platform::u32 kOffsetBodyChecksum = 24;
inline constexpr platform::u32 kOffsetFreeSpaceBytes = 88;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "index_btree_physical_page_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 millis,
                                  platform::byte suffix) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(kind, generated.value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

std::vector<platform::byte> SortableI64(std::int64_t value) {
  const auto sortable = static_cast<std::uint64_t>(value) ^ 0x8000000000000000ull;
  std::vector<platform::byte> out(8);
  for (int i = 7; i >= 0; --i) {
    out[static_cast<std::size_t>(7 - i)] =
        static_cast<platform::byte>((sortable >> (i * 8)) & 0xffu);
  }
  return out;
}

std::vector<platform::byte> EncodedKey(std::int64_t value) {
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = GeneratedUuid(platform::UuidKind::object, 1700000000000ull, 0x20);
  component.sort_direction = idx::IndexKeySortDirection::ascending;
  component.null_placement = idx::IndexKeyNullPlacement::nulls_last;
  component.payload = SortableI64(value);
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "SBKO key encoding failed");
  return encoded.encoded;
}

std::vector<platform::byte> EncodedNullKey() {
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid = GeneratedUuid(platform::UuidKind::object, 1700000000100ull, 0x21);
  component.sort_direction = idx::IndexKeySortDirection::ascending;
  component.null_placement = idx::IndexKeyNullPlacement::nulls_last;
  component.is_null = true;
  const auto encoded = idx::EncodeIndexKey({component}, {});
  Require(encoded.ok(), "SBKO null key encoding failed");
  return encoded.encoded;
}

page::IndexBtreeCell Cell(std::int64_t key,
                          platform::byte row_suffix,
                          platform::byte version_suffix) {
  page::IndexBtreeCell cell;
  cell.key_ordinal = 0;
  cell.encoded_key = EncodedKey(key);
  cell.row_uuid = GeneratedUuid(platform::UuidKind::row, 1700000100000ull + row_suffix, row_suffix);
  cell.version_uuid =
      GeneratedUuid(platform::UuidKind::row, 1700000200000ull + version_suffix, version_suffix);
  return cell;
}

page::IndexBtreeCell NullCell(platform::byte row_suffix,
                              platform::byte version_suffix) {
  page::IndexBtreeCell cell;
  cell.key_ordinal = 0;
  cell.encoded_key = EncodedNullKey();
  cell.row_uuid = GeneratedUuid(platform::UuidKind::row, 1700000100000ull + row_suffix, row_suffix);
  cell.version_uuid =
      GeneratedUuid(platform::UuidKind::row, 1700000200000ull + version_suffix, version_suffix);
  return cell;
}

int CompareCells(const page::IndexBtreeCell& left, const page::IndexBtreeCell& right) {
  const auto key_compare = idx::CompareEncodedIndexKeys(left.encoded_key, right.encoded_key);
  Require(key_compare.ok(), "encoded key comparison failed");
  if (key_compare.comparison != 0) {
    return key_compare.comparison;
  }
  const int row_compare = uuid::CompareUuid128(left.row_uuid.value, right.row_uuid.value);
  if (row_compare != 0) {
    return row_compare;
  }
  return uuid::CompareUuid128(left.version_uuid.value, right.version_uuid.value);
}

void RequireSorted(const std::vector<page::IndexBtreeCell>& cells, std::string_view label) {
  for (std::size_t i = 1; i < cells.size(); ++i) {
    if (CompareCells(cells[i - 1], cells[i]) > 0) {
      std::cerr << "unsorted label=" << label << " index=" << i << '\n';
      std::exit(EXIT_FAILURE);
    }
  }
}

bool HasEvidence(const std::vector<std::string>& evidence, std::string_view needle) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const std::string& value) {
                       return value == needle || value.find(needle) != std::string::npos;
                     });
}

std::size_t LiveCellCount(const std::vector<page::IndexBtreeCell>& cells) {
  return static_cast<std::size_t>(
      std::count_if(cells.begin(),
                    cells.end(),
                    [](const page::IndexBtreeCell& cell) {
                      return !cell.deleted;
                    }));
}

std::size_t DeletedCellCount(const std::vector<page::IndexBtreeCell>& cells) {
  return static_cast<std::size_t>(
      std::count_if(cells.begin(),
                    cells.end(),
                    [](const page::IndexBtreeCell& cell) {
                      return cell.deleted;
                    }));
}

page::IndexBtreePageBody Fetch(const page::IndexBtreePhysicalTree& tree,
                               platform::u64 page_number) {
  auto fetched = page::FetchIndexBtreePhysicalPage(tree, page_number);
  if (!fetched.ok()) {
    std::cerr << "fetch failed page=" << page_number
              << " diagnostic=" << fetched.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  return fetched.body;
}

void CollectLeaves(const page::IndexBtreePhysicalTree& tree,
                   const page::IndexBtreePageBody& node,
                   platform::u64 expected_parent,
                   std::vector<page::IndexBtreePageBody>* leaves) {
  Require(node.parent_page_number == expected_parent, "page parent metadata mismatch");
  Require(node.index_uuid.value == tree.index_uuid.value, "page index uuid mismatch");
  Require(node.free_space_bytes > 0, "page free-space metadata missing");
  RequireSorted(node.cells, "branch-or-leaf");
  if (node.tree_level == 0) {
    Require(node.page_kind == page::IndexBtreePageKind::leaf ||
                node.page_kind == page::IndexBtreePageKind::root,
            "level-zero page kind mismatch");
    leaves->push_back(node);
    return;
  }
  Require(node.page_kind == page::IndexBtreePageKind::internal ||
              node.page_kind == page::IndexBtreePageKind::root,
          "internal page kind mismatch");
  for (const auto& fence : node.cells) {
    Require(fence.high_key, "internal child fence must be marked high key");
    Require(fence.child_page_number != 0, "internal fence child page missing");
    const auto child = Fetch(tree, fence.child_page_number);
    Require(child.tree_level + 1 == node.tree_level, "child level mismatch");
    CollectLeaves(tree, child, node.page_number, leaves);
  }
}

std::vector<page::IndexBtreePageBody> AllLeaves(const page::IndexBtreePhysicalTree& tree) {
  const auto root = Fetch(tree, tree.root_page_number);
  Require(root.page_kind == page::IndexBtreePageKind::root, "root kind mismatch");
  std::vector<page::IndexBtreePageBody> leaves;
  CollectLeaves(tree, root, 0, &leaves);
  return leaves;
}

void VerifyTreeOrderingAndMetadata(const page::IndexBtreePhysicalTree& tree,
                                   std::size_t expected_cells) {
  const auto root = Fetch(tree, tree.root_page_number);
  Require(root.page_number == tree.root_page_number, "root page number not stable");
  Require(root.index_uuid.value == tree.index_uuid.value, "root index uuid mismatch");
  Require(root.free_space_bytes > 0, "root free-space metadata missing");

  std::vector<page::IndexBtreeCell> all_cells;
  auto leaves = AllLeaves(tree);
  for (std::size_t i = 0; i < leaves.size(); ++i) {
    const auto& leaf = leaves[i];
    Require(leaf.tree_level == 0, "leaf level mismatch");
    RequireSorted(leaf.cells, "leaf");
    Require(DeletedCellCount(leaf.cells) == 0, "unexpected collected tombstone");
    if (leaves.size() > 1) {
      const platform::u64 expected_left = i == 0 ? 0 : leaves[i - 1].page_number;
      const platform::u64 expected_right = i + 1 == leaves.size() ? 0 : leaves[i + 1].page_number;
      Require(leaf.left_sibling_page_number == expected_left, "leaf left sibling mismatch");
      Require(leaf.right_sibling_page_number == expected_right, "leaf right sibling mismatch");
    }
    for (const auto& cell : leaf.cells) {
      if (!cell.deleted) {
        all_cells.push_back(cell);
      }
    }
  }
  Require(all_cells.size() == expected_cells, "tree cell count mismatch");
  RequireSorted(all_cells, "tree");
}

std::size_t ReachableDeletedCellCount(const page::IndexBtreePhysicalTree& tree) {
  auto leaves = AllLeaves(tree);
  std::size_t deleted = 0;
  for (const auto& leaf : leaves) {
    deleted += DeletedCellCount(leaf.cells);
  }
  return deleted;
}

std::vector<page::IndexBtreeCell> ReachableLiveCells(const page::IndexBtreePhysicalTree& tree) {
  std::vector<page::IndexBtreeCell> cells;
  for (const auto& leaf : AllLeaves(tree)) {
    for (const auto& cell : leaf.cells) {
      if (!cell.deleted) {
        cells.push_back(cell);
      }
    }
  }
  RequireSorted(cells, "reachable-live");
  return cells;
}

page::IndexBtreePhysicalScanBound Bound(const std::vector<platform::byte>& encoded_key,
                                        bool inclusive) {
  page::IndexBtreePhysicalScanBound bound;
  bound.encoded_key = encoded_key;
  bound.inclusive = inclusive;
  bound.unbounded = false;
  return bound;
}

std::vector<platform::byte> RawEncodedKey(platform::byte group, platform::byte suffix) {
  return {'S', 'B', 'K', 'O', 0x7f, group, suffix, 0x00, 0x00};
}

std::vector<platform::byte> RawEncodedPrefix(platform::byte group) {
  idx::IndexKeyEncodingComponent component;
  component.kind = idx::IndexKeyComponentKind::scalar;
  component.ordinal = 0;
  component.type_descriptor_uuid =
      GeneratedUuid(platform::UuidKind::object, 1700000000200ull, 0x22);
  component.payload = {group};
  const auto prefix = idx::BuildEncodedPrefixMatcher({component}, {});
  Require(prefix.ok(), "SBKO prefix matcher generation failed");
  return prefix.matcher_prefix;
}

page::IndexBtreeCell RawCell(platform::byte group,
                             platform::byte suffix,
                             platform::byte row_suffix,
                             platform::byte version_suffix) {
  page::IndexBtreeCell cell;
  cell.key_ordinal = 0;
  cell.encoded_key = RawEncodedKey(group, suffix);
  cell.row_uuid = GeneratedUuid(platform::UuidKind::row, 1700000100000ull + row_suffix, row_suffix);
  cell.version_uuid =
      GeneratedUuid(platform::UuidKind::row, 1700000200000ull + version_suffix, version_suffix);
  return cell;
}

bool SameLocator(const page::IndexBtreePhysicalRowLocator& locator,
                 const page::IndexBtreeCell& cell) {
  return locator.encoded_key == cell.encoded_key &&
         locator.row_uuid.value == cell.row_uuid.value &&
         locator.version_uuid.value == cell.version_uuid.value;
}

bool SameLiveEntry(const page::IndexBtreeCell& left, const page::IndexBtreeCell& right) {
  return left.encoded_key == right.encoded_key &&
         left.row_uuid.value == right.row_uuid.value &&
         left.version_uuid.value == right.version_uuid.value;
}

page::IndexBtreeCell FenceForChild(const page::IndexBtreePageBody& child) {
  page::IndexBtreeCell fence;
  for (auto it = child.cells.rbegin(); it != child.cells.rend(); ++it) {
    if (!it->deleted) {
      fence = *it;
      break;
    }
  }
  fence.high_key = true;
  fence.deleted = false;
  fence.child_page_number = child.page_number;
  return fence;
}

void ReplaceImagePage(page::IndexBtreePhysicalTreeImage* image,
                      const page::IndexBtreePageBody& body) {
  const auto built = page::BuildIndexBtreePageBody(body, image->page_size);
  if (!built.ok()) {
    std::cerr << "replacement page build failed diagnostic="
              << built.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  for (auto& page_image : image->pages) {
    if (page_image.page_number == body.page_number) {
      page_image.serialized = built.serialized;
      return;
    }
  }
  Fail("replacement page image not found");
}

bool LocatorLess(const page::IndexBtreePhysicalRowLocator& left,
                 const page::IndexBtreePhysicalRowLocator& right) {
  page::IndexBtreeCell left_cell;
  left_cell.encoded_key = left.encoded_key;
  left_cell.row_uuid = left.row_uuid;
  left_cell.version_uuid = left.version_uuid;
  page::IndexBtreeCell right_cell;
  right_cell.encoded_key = right.encoded_key;
  right_cell.row_uuid = right.row_uuid;
  right_cell.version_uuid = right.version_uuid;
  return CompareCells(left_cell, right_cell) < 0;
}

void RequireLocatorContracts(const page::IndexBtreePhysicalRowLocator& locator) {
  Require(locator.leaf_page_number != 0, "scan locator missing leaf page number");
  Require(locator.mga_recheck_required, "scan locator missing MGA recheck requirement");
  Require(locator.security_recheck_required, "scan locator missing security recheck requirement");
  Require(!locator.visibility_authority, "scan locator claimed visibility authority");
  Require(!locator.authorization_authority, "scan locator claimed authorization authority");
  Require(!locator.transaction_finality_authority,
          "scan locator claimed transaction finality authority");
  Require(!locator.recovery_authority, "scan locator claimed recovery authority");
  Require(locator.tombstone_excluded, "scan locator missing tombstone exclusion metadata");
}

void RequireUniqueContracts(const page::IndexBtreePhysicalUniqueInsertResult& result,
                            std::string_view label) {
  Require(result.ok(), std::string(label) + " unique physical insert failed");
  Require(HasEvidence(result.evidence, "unique_atomic_probe_insert=true"),
          std::string(label) + " missing atomic unique evidence");
  Require(HasEvidence(result.evidence, "exclusive_latch_acquired=true"),
          std::string(label) + " missing exclusive latch evidence");
  Require(HasEvidence(result.evidence, "latch_authority=structural_only"),
          std::string(label) + " missing structural latch evidence");
  Require(HasEvidence(result.evidence, "visibility_authority=false"),
          std::string(label) + " claimed visibility authority");
  Require(HasEvidence(result.evidence, "authorization_authority=false"),
          std::string(label) + " claimed authorization authority");
  Require(HasEvidence(result.evidence, "transaction_finality_authority=false"),
          std::string(label) + " claimed transaction finality authority");
  Require(HasEvidence(result.evidence, "recovery_authority=false"),
          std::string(label) + " claimed recovery authority");
  for (const auto& candidate : result.conflict_candidates) {
    Require(candidate.same_key_identity, std::string(label) + " candidate missing key identity");
    Require(candidate.leaf_page_number != 0, std::string(label) + " candidate missing leaf");
    Require(candidate.mga_recheck_required,
            std::string(label) + " candidate missing MGA recheck");
    Require(candidate.security_recheck_required,
            std::string(label) + " candidate missing security recheck");
    Require(!candidate.visibility_authority,
            std::string(label) + " candidate claimed visibility authority");
    Require(!candidate.authorization_authority,
            std::string(label) + " candidate claimed authorization authority");
    Require(!candidate.transaction_finality_authority,
            std::string(label) + " candidate claimed finality authority");
    Require(!candidate.recovery_authority,
            std::string(label) + " candidate claimed recovery authority");
  }
}

void RequireScanContracts(const page::IndexBtreePhysicalScanResult& scan,
                          std::string_view mode,
                          std::string_view ordering) {
  Require(scan.ok(), "physical scan failed");
  Require(HasEvidence(scan.evidence, std::string("scan_mode=") + std::string(mode)),
          "scan mode evidence missing");
  Require(HasEvidence(scan.evidence, std::string("scan_ordering=") + std::string(ordering)),
          "scan ordering evidence missing");
  Require(HasEvidence(scan.evidence, "mga_recheck_required=true"),
          "scan missing MGA recheck evidence");
  Require(HasEvidence(scan.evidence, "security_recheck_required=true"),
          "scan missing security recheck evidence");
  Require(HasEvidence(scan.evidence, "visibility_authority=false"),
          "scan claimed visibility authority");
  Require(HasEvidence(scan.evidence, "authorization_authority=false"),
          "scan claimed authorization authority");
  Require(HasEvidence(scan.evidence, "transaction_finality_authority=false"),
          "scan claimed transaction finality authority");
  Require(HasEvidence(scan.evidence, "recovery_authority=false"),
          "scan claimed recovery authority");
  Require(HasEvidence(scan.evidence, "latch_authority=structural_only"),
          "scan missing structural latch authority evidence");
  Require(HasEvidence(scan.evidence, "reachable_root_descent=true"),
          "scan missing root descent evidence");
  Require(HasEvidence(scan.evidence, "orphan_pages_ignored=true"),
          "scan missing orphan exclusion evidence");
  Require(HasEvidence(scan.evidence, "deleted_tombstones_excluded=true"),
          "scan missing tombstone exclusion evidence");
  Require(HasEvidence(scan.evidence, "visited_leaf_pages="),
          "scan missing visited leaf accounting evidence");
  Require(HasEvidence(scan.evidence, "pruned_leaf_pages="),
          "scan missing pruned leaf accounting evidence");
  for (const auto& locator : scan.locators) {
    RequireLocatorContracts(locator);
  }
}

void RequireValidationContracts(const page::IndexBtreePhysicalTreeValidationResult& validation,
                                std::size_t expected_live_entries,
                                std::string_view label) {
  if (!validation.ok()) {
    std::cerr << "validation failed label=" << label
              << " diagnostic=" << validation.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  Require(validation.live_entry_count == expected_live_entries,
          std::string(label) + " live entry count mismatch");
  Require(HasEvidence(validation.evidence, "structural_validation=true"),
          std::string(label) + " missing structural validation evidence");
  Require(HasEvidence(validation.evidence, "page_image_checksums_verified=true"),
          std::string(label) + " missing checksum validation evidence");
  Require(HasEvidence(validation.evidence, "parent_child_fences_verified=true"),
          std::string(label) + " missing fence validation evidence");
  Require(HasEvidence(validation.evidence, "sibling_order_verified=true"),
          std::string(label) + " missing sibling validation evidence");
  Require(HasEvidence(validation.evidence, "leaf_range_partition_verified=true"),
          std::string(label) + " missing leaf range partition evidence");
  Require(HasEvidence(validation.evidence, "global_leaf_stream_order_verified=true"),
          std::string(label) + " missing global leaf stream evidence");
  Require(HasEvidence(validation.evidence, "live_entries_no_duplicates=true"),
          std::string(label) + " missing duplicate-proof evidence");
  Require(HasEvidence(validation.evidence, "latch_authority=structural_only"),
          std::string(label) + " missing structural-only latch evidence");
  Require(HasEvidence(validation.evidence, "visibility_authority=false"),
          std::string(label) + " claimed visibility authority");
  Require(HasEvidence(validation.evidence, "authorization_authority=false"),
          std::string(label) + " claimed authorization authority");
  Require(HasEvidence(validation.evidence, "transaction_finality_authority=false"),
          std::string(label) + " claimed transaction finality authority");
  Require(HasEvidence(validation.evidence, "recovery_authority=false"),
          std::string(label) + " claimed recovery authority");
}

void RequireReportContracts(const page::IndexBtreePhysicalTreeReport& report,
                            std::size_t expected_live_entries,
                            std::string_view label) {
  if (!report.ok()) {
    std::cerr << "report failed label=" << label
              << " diagnostic=" << report.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  Require(report.valid, std::string(label) + " report unexpectedly invalid");
  Require(report.root_page_number != 0, std::string(label) + " report missing root page");
  Require(report.page_count >= report.reachable_page_count,
          std::string(label) + " report page accounting invalid");
  Require(report.reachable_page_count != 0, std::string(label) + " report missing reachable pages");
  Require(report.reachable_leaf_count != 0, std::string(label) + " report missing leaves");
  Require(report.tuple_live_entry_estimate == expected_live_entries,
          std::string(label) + " report live estimate mismatch");
  Require(report.tree_height != 0, std::string(label) + " report missing tree height");
  Require(report.page_size != 0, std::string(label) + " report missing page size");
  Require(report.next_page_number != 0, std::string(label) + " report missing next page number");
  Require(report.corruption_class == page::IndexBtreePhysicalCorruptionClass::none,
          std::string(label) + " report classified valid tree as corrupt");
  Require(!report.visibility_authority, std::string(label) + " claimed visibility authority");
  Require(!report.authorization_authority, std::string(label) + " claimed authorization authority");
  Require(!report.transaction_finality_authority,
          std::string(label) + " claimed transaction finality authority");
  Require(!report.recovery_authority, std::string(label) + " claimed recovery authority");
  Require(HasEvidence(report.support_bundle_rows, "root_page_number="),
          std::string(label) + " support row missing root page");
  Require(HasEvidence(report.support_bundle_rows, "page_count="),
          std::string(label) + " support row missing page count");
  Require(HasEvidence(report.support_bundle_rows, "reachable_page_count="),
          std::string(label) + " support row missing reachable page count");
  Require(HasEvidence(report.support_bundle_rows, "reachable_leaf_count="),
          std::string(label) + " support row missing reachable leaf count");
  Require(HasEvidence(report.support_bundle_rows, "tuple_live_entry_estimate="),
          std::string(label) + " support row missing live estimate");
  Require(HasEvidence(report.support_bundle_rows, "tombstone_deleted_entry_count="),
          std::string(label) + " support row missing tombstone count");
  Require(HasEvidence(report.support_bundle_rows, "tree_height="),
          std::string(label) + " support row missing tree height");
  Require(HasEvidence(report.support_bundle_rows, "page_size="),
          std::string(label) + " support row missing page size");
  Require(HasEvidence(report.support_bundle_rows, "next_page_number="),
          std::string(label) + " support row missing next page number");
  Require(HasEvidence(report.support_bundle_rows, "corruption_class=none"),
          std::string(label) + " support row missing corruption class");
  Require(HasEvidence(report.support_bundle_rows, "visibility=false"),
          std::string(label) + " support row missing visibility non-authority alias");
  Require(HasEvidence(report.support_bundle_rows, "authorization=false"),
          std::string(label) + " support row missing authorization non-authority alias");
  Require(HasEvidence(report.support_bundle_rows, "transaction_finality=false"),
          std::string(label) + " support row missing finality non-authority alias");
  Require(HasEvidence(report.support_bundle_rows, "recovery=false"),
          std::string(label) + " support row missing recovery non-authority alias");
  Require(HasEvidence(report.support_bundle_rows, "visibility_authority=false"),
          std::string(label) + " support row claimed visibility authority");
  Require(HasEvidence(report.support_bundle_rows, "authorization_authority=false"),
          std::string(label) + " support row claimed authorization authority");
  Require(HasEvidence(report.support_bundle_rows, "transaction_finality_authority=false"),
          std::string(label) + " support row claimed transaction finality authority");
  Require(HasEvidence(report.support_bundle_rows, "recovery_authority=false"),
          std::string(label) + " support row claimed recovery authority");
}

void RequireFullLeafTraversal(const page::IndexBtreePhysicalScanResult& scan,
                              std::string_view label) {
  Require(scan.reachable_leaf_pages > 1, std::string(label) + " needs a multi-leaf tree");
  Require(scan.visited_leaf_pages == scan.reachable_leaf_pages,
          std::string(label) + " did not visit all reachable leaves");
  Require(scan.pruned_leaf_pages == 0, std::string(label) + " unexpectedly pruned leaves");
  Require(scan.pruned_subtrees == 0, std::string(label) + " unexpectedly pruned subtrees");
}

void RequireFencePruning(const page::IndexBtreePhysicalScanResult& scan,
                         std::string_view label) {
  Require(scan.reachable_leaf_pages > 1, std::string(label) + " needs a multi-leaf tree");
  Require(scan.visited_leaf_pages < scan.reachable_leaf_pages || scan.pruned_leaf_pages > 0,
          std::string(label) + " did not prune any leaf pages");
  Require(scan.pruned_subtrees > 0, std::string(label) + " did not prune any fence subtrees");
  Require(HasEvidence(scan.evidence, "fence_pruning_enabled=true"),
          std::string(label) + " missing fence pruning evidence");
}

void RequireScanMatches(const page::IndexBtreePhysicalScanResult& scan,
                        const std::vector<page::IndexBtreeCell>& expected,
                        std::string_view label) {
  Require(scan.locators.size() == expected.size(), std::string(label) + " scan count mismatch");
  for (std::size_t i = 0; i < expected.size(); ++i) {
    if (!SameLocator(scan.locators[i], expected[i])) {
      std::cerr << "scan mismatch label=" << label << " index=" << i << '\n';
      std::exit(EXIT_FAILURE);
    }
  }
}

page::IndexBtreePhysicalTree SnapshotReopenRoundTrip(
    const page::IndexBtreePhysicalTree& tree,
    const std::vector<page::IndexBtreeCell>& expected_live,
    std::string_view label) {
  const auto validation = page::ValidateIndexBtreePhysicalTree(tree);
  RequireValidationContracts(validation, expected_live.size(), label);

  const auto exported = page::ExportIndexBtreePhysicalTreeImage(tree);
  if (!exported.ok()) {
    std::cerr << "export failed label=" << label
              << " diagnostic=" << exported.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  Require(HasEvidence(exported.evidence, "serialized_tree_image_exported=true"),
          std::string(label) + " missing export evidence");
  Require(HasEvidence(exported.evidence, "recovery_authority=false"),
          std::string(label) + " export claimed recovery authority");

  const auto reopened = page::ImportIndexBtreePhysicalTreeImage(exported.image);
  if (!reopened.ok()) {
    std::cerr << "import failed label=" << label
              << " diagnostic=" << reopened.diagnostic.diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  Require(HasEvidence(reopened.evidence, "serialized_tree_image_imported=true"),
          std::string(label) + " missing import evidence");
  Require(HasEvidence(reopened.evidence, "crash_reopen_style_validation=true"),
          std::string(label) + " missing crash/reopen validation evidence");

  const auto reopened_validation = page::ValidateIndexBtreePhysicalTree(reopened.tree);
  RequireValidationContracts(reopened_validation, expected_live.size(), label);
  const auto reopened_scan = page::OrderedScanIndexBtreePhysicalTree(reopened.tree);
  RequireScanContracts(reopened_scan, "ordered", "forward");
  RequireScanMatches(reopened_scan, expected_live, label);
  return reopened.tree;
}

std::vector<page::IndexBtreeCell> FilterPoint(
    const std::vector<page::IndexBtreeCell>& cells,
    const std::vector<platform::byte>& encoded_key) {
  std::vector<page::IndexBtreeCell> out;
  for (const auto& cell : cells) {
    const auto compare = idx::CompareEncodedIndexKeys(cell.encoded_key, encoded_key);
    Require(compare.ok(), "point filter key comparison failed");
    if (compare.comparison == 0) {
      out.push_back(cell);
    }
  }
  return out;
}

std::vector<page::IndexBtreeCell> FilterRange(
    const std::vector<page::IndexBtreeCell>& cells,
    const page::IndexBtreePhysicalScanBound& lower,
    const page::IndexBtreePhysicalScanBound& upper) {
  std::vector<page::IndexBtreeCell> out;
  for (const auto& cell : cells) {
    bool include = true;
    if (!lower.unbounded) {
      const auto compare = idx::CompareEncodedIndexKeys(cell.encoded_key, lower.encoded_key);
      Require(compare.ok(), "lower range filter key comparison failed");
      include = include && (compare.comparison > 0 ||
                            (compare.comparison == 0 && lower.inclusive));
    }
    if (!upper.unbounded) {
      const auto compare = idx::CompareEncodedIndexKeys(cell.encoded_key, upper.encoded_key);
      Require(compare.ok(), "upper range filter key comparison failed");
      include = include && (compare.comparison < 0 ||
                            (compare.comparison == 0 && upper.inclusive));
    }
    if (include) {
      out.push_back(cell);
    }
  }
  return out;
}

std::vector<page::IndexBtreeCell> FilterPrefix(
    const std::vector<page::IndexBtreeCell>& cells,
    const std::vector<platform::byte>& prefix) {
  std::vector<page::IndexBtreeCell> out;
  for (const auto& cell : cells) {
    if (cell.encoded_key.size() >= prefix.size() &&
        std::equal(prefix.begin(), prefix.end(), cell.encoded_key.begin())) {
      out.push_back(cell);
    }
  }
  return out;
}

std::size_t MaxRootLeafCapacity(platform::TypedUuid index_uuid, platform::u32 page_size) {
  page::IndexBtreePageBody body;
  body.index_uuid = index_uuid;
  body.page_number = 1;
  body.page_kind = page::IndexBtreePageKind::root;
  body.tree_level = 0;
  std::size_t capacity = 0;
  for (std::size_t i = 0; i < 64; ++i) {
    body.cells.push_back(Cell(static_cast<std::int64_t>(i),
                              static_cast<platform::byte>(0x20 + i),
                              static_cast<platform::byte>(0x60 + i)));
    const auto built = page::BuildIndexBtreePageBody(body, page_size);
    if (!built.ok()) {
      break;
    }
    capacity = body.cells.size();
  }
  return capacity;
}

void SerializeParseRoundTrip() {
  const auto index_uuid = GeneratedUuid(platform::UuidKind::object, 1700000300000ull, 0x31);
  page::IndexBtreePageBody body;
  body.index_uuid = index_uuid;
  body.page_number = 42;
  body.parent_page_number = 9;
  body.left_sibling_page_number = 40;
  body.right_sibling_page_number = 44;
  body.page_kind = page::IndexBtreePageKind::leaf;
  body.tree_level = 0;
  body.cells = {Cell(10, 0x41, 0x51), Cell(11, 0x42, 0x52)};

  const auto built = page::BuildIndexBtreePageBody(body, 1024);
  Require(built.ok(), "physical page build failed");
  const auto parsed = page::ParseIndexBtreePageBody(built.serialized, body.page_number);
  Require(parsed.ok(), "physical page parse failed");
  Require(parsed.body.page_number == body.page_number, "page number did not round trip");
  Require(parsed.body.parent_page_number == body.parent_page_number, "parent page did not round trip");
  Require(parsed.body.left_sibling_page_number == body.left_sibling_page_number,
          "left sibling did not round trip");
  Require(parsed.body.right_sibling_page_number == body.right_sibling_page_number,
          "right sibling did not round trip");
  Require(parsed.body.free_space_bytes > 0, "free-space metadata missing");
  Require(parsed.body.cells.size() == body.cells.size(), "cell count did not round trip");
  Require(parsed.body.cells[0].encoded_key == body.cells[0].encoded_key,
          "encoded key did not round trip");
  Require(parsed.body.cells[0].version_uuid.value == body.cells[0].version_uuid.value,
          "version uuid did not round trip");
}

void DeclaredBodyTrailingBytesRefusal() {
  const auto index_uuid = GeneratedUuid(platform::UuidKind::object, 1700000350000ull, 0x36);
  page::IndexBtreePageBody body;
  body.index_uuid = index_uuid;
  body.page_number = 43;
  body.page_kind = page::IndexBtreePageKind::leaf;
  body.cells = {Cell(12, 0x43, 0x53), Cell(13, 0x44, 0x54)};
  auto built = page::BuildIndexBtreePageBody(body, 1024);
  Require(built.ok(), "physical page build for trailing-body test failed");

  const platform::u32 body_bytes =
      platform::LoadLittle32(built.serialized.data() + kOffsetBodyBytes);
  const platform::u32 free_space =
      platform::LoadLittle32(built.serialized.data() + kOffsetFreeSpaceBytes);
  Require(free_space > 0, "trailing-body test needs spare free space");
  platform::StoreLittle32(built.serialized.data() + kOffsetBodyBytes, body_bytes + 1);
  platform::StoreLittle32(built.serialized.data() + kOffsetFreeSpaceBytes, free_space - 1);
  platform::StoreLittle64(built.serialized.data() + kOffsetBodyChecksum,
                          page::ComputeIndexBtreePageChecksum(built.serialized));

  const auto parsed = page::ParseIndexBtreePageBody(built.serialized, body.page_number);
  Require(!parsed.ok(), "trailing declared body bytes unexpectedly accepted");
  Require(parsed.diagnostic.diagnostic_code == "SB-INDEX-BTREE-PAGE-BODY-TRAILING-BYTES",
          "trailing declared body diagnostic mismatch");
}

void UnsafeLegacyKeyRefusal() {
  const auto index_uuid = GeneratedUuid(platform::UuidKind::object, 1700000400000ull, 0x61);
  page::IndexBtreePageBody body;
  body.index_uuid = index_uuid;
  body.page_number = 7;
  body.page_kind = page::IndexBtreePageKind::leaf;
  body.cells = {Cell(1, 0x62, 0x72)};
  body.cells[0].encoded_key = {'S', 'B', 'K', '1', 0x00};
  const auto built = page::BuildIndexBtreePageBody(body, 1024);
  Require(!built.ok(), "unsafe legacy key unexpectedly accepted");
  Require(built.diagnostic.diagnostic_code == "SB-INDEX-BTREE-PAGE-UNSAFE-LEGACY-KEY",
          "unsafe legacy key diagnostic mismatch");
}

void InsertSplitRootSplit() {
  auto init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700000500000ull, 0x81), 400);
  Require(init.ok(), "physical tree initialization failed");
  page::IndexBtreePhysicalTree tree = std::move(init.tree);

  bool saw_split = false;
  bool saw_root_split = false;
  std::vector<int> keys;
  for (int i = 0; i < 64; ++i) {
    keys.push_back((i * 37) % 64);
  }
  for (std::size_t i = 0; i < keys.size(); ++i) {
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = Cell(keys[i], static_cast<platform::byte>(0x90 + i),
                        static_cast<platform::byte>(0xa0 + i));
    const auto inserted = page::InsertIndexBtreeCell(&tree, request);
    if (!inserted.ok()) {
      std::cerr << "insert failed diagnostic=" << inserted.diagnostic.diagnostic_code << '\n';
      std::exit(EXIT_FAILURE);
    }
    saw_split = saw_split || inserted.split_performed;
    saw_root_split = saw_root_split || inserted.root_split_performed;
    VerifyTreeOrderingAndMetadata(tree, i + 1);
  }
  Require(saw_split, "leaf split was not triggered");
  Require(saw_root_split, "root split was not triggered");

  const auto root = Fetch(tree, tree.root_page_number);
  Require(root.page_kind == page::IndexBtreePageKind::root, "root kind changed after split");
  Require(root.tree_level >= 2, "root did not cascade beyond one internal level");
  Require(root.cells.size() >= 2, "root split did not publish two child fences");

  auto duplicate_key_tree = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700000600000ull, 0xc1), 1024);
  Require(duplicate_key_tree.ok(), "duplicate key tree initialization failed");
  const std::vector<page::IndexBtreeCell> duplicate_cells = {
      Cell(22, 0xd1, 0xe2), Cell(22, 0xd0, 0xe3), Cell(22, 0xd1, 0xe1)};
  for (const auto& cell : duplicate_cells) {
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = cell;
    const auto inserted = page::InsertIndexBtreeCell(&duplicate_key_tree.tree, request);
    Require(inserted.ok(), "duplicate key tie-break insert failed");
  }
  VerifyTreeOrderingAndMetadata(duplicate_key_tree.tree, duplicate_cells.size());
}

void PhysicalScanContracts() {
  auto init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700000650000ull, 0xc8), 400);
  Require(init.ok(), "scan tree initialization failed");
  page::IndexBtreePhysicalTree tree = std::move(init.tree);

  std::vector<page::IndexBtreeCell> inserted;
  for (int i = 0; i < 36; ++i) {
    inserted.push_back(Cell(i, static_cast<platform::byte>(0x20 + i),
                            static_cast<platform::byte>(0x60 + i)));
  }
  inserted.push_back(Cell(17, 0x71, 0x91));
  inserted.push_back(Cell(17, 0x72, 0x92));
  for (std::size_t i = 0; i < inserted.size(); ++i) {
    const std::size_t index = (i * 13) % inserted.size();
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = inserted[index];
    const auto result = page::InsertIndexBtreeCell(&tree, request);
    Require(result.ok(), "scan setup insert failed");
  }
  VerifyTreeOrderingAndMetadata(tree, inserted.size());

  const auto live = ReachableLiveCells(tree);
  const auto ordered = page::OrderedScanIndexBtreePhysicalTree(tree);
  RequireScanContracts(ordered, "ordered", "forward");
  RequireFullLeafTraversal(ordered, "ordered");
  Require(HasEvidence(ordered.evidence, "fence_pruning_enabled=false"),
          "ordered scan should report pruning disabled");
  RequireScanMatches(ordered, live, "ordered");

  const auto reverse = page::OrderedScanIndexBtreePhysicalTree(
      tree, page::IndexBtreePhysicalScanOrdering::reverse);
  RequireScanContracts(reverse, "ordered", "reverse");
  RequireFullLeafTraversal(reverse, "reverse");
  Require(reverse.locators.size() == ordered.locators.size(), "reverse scan count mismatch");
  for (std::size_t i = 0; i < ordered.locators.size(); ++i) {
    Require(SameLocator(reverse.locators[i], live[live.size() - 1 - i]),
            "reverse scan was not exact inverse of forward scan");
  }

  const auto point = page::PointLookupIndexBtreePhysicalTree(tree, EncodedKey(17));
  RequireScanContracts(point, "point", "forward");
  RequireFencePruning(point, "point");
  Require(HasEvidence(point.evidence, "point_fence_pruning=true"),
          "point scan missing mode-specific pruning evidence");
  RequireScanMatches(point, FilterPoint(live, EncodedKey(17)), "point");
  Require(point.locators.size() == 3, "point scan did not return duplicate key locator stream");

  const auto inclusive_range = page::RangeScanIndexBtreePhysicalTree(
      tree, Bound(EncodedKey(8), true), Bound(EncodedKey(12), true));
  RequireScanContracts(inclusive_range, "range", "forward");
  RequireFencePruning(inclusive_range, "inclusive range");
  Require(HasEvidence(inclusive_range.evidence, "range_fence_pruning=true"),
          "range scan missing mode-specific pruning evidence");
  RequireScanMatches(inclusive_range,
                     FilterRange(live, Bound(EncodedKey(8), true), Bound(EncodedKey(12), true)),
                     "inclusive-range");
  Require(inclusive_range.locators.size() == 5, "inclusive range boundary count mismatch");

  const auto exclusive_range = page::RangeScanIndexBtreePhysicalTree(
      tree, Bound(EncodedKey(8), false), Bound(EncodedKey(12), false));
  RequireScanContracts(exclusive_range, "range", "forward");
  RequireFencePruning(exclusive_range, "exclusive range");
  RequireScanMatches(exclusive_range,
                     FilterRange(live, Bound(EncodedKey(8), false), Bound(EncodedKey(12), false)),
                     "exclusive-range");
  Require(exclusive_range.locators.size() == 3, "exclusive range boundary count mismatch");

  const auto limited = page::OrderedScanIndexBtreePhysicalTree(
      tree, page::IndexBtreePhysicalScanOrdering::forward, 7);
  RequireScanContracts(limited, "ordered", "forward");
  RequireFullLeafTraversal(limited, "ordered limit");
  Require(HasEvidence(limited.evidence, "limit_applied=true"), "limit evidence missing");
  Require(limited.locators.size() == 7, "ordered limit count mismatch");
  RequireScanMatches(limited,
                     std::vector<page::IndexBtreeCell>(live.begin(), live.begin() + 7),
                     "ordered-limit");

  auto prefix_init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700000660000ull, 0xc9), 400);
  Require(prefix_init.ok(), "prefix tree initialization failed");
  page::IndexBtreePhysicalTree prefix_tree = std::move(prefix_init.tree);
  std::vector<page::IndexBtreeCell> prefix_cells = {
      RawCell(0x40, 0x10, 0xa0, 0xb0), RawCell(0x41, 0x10, 0xa1, 0xb1),
      RawCell(0x42, 0x10, 0xa2, 0xb2), RawCell(0x42, 0x11, 0xa3, 0xb3),
      RawCell(0x42, 0x20, 0xa4, 0xb4), RawCell(0x43, 0x10, 0xa5, 0xb5),
      RawCell(0x44, 0x10, 0xa6, 0xb6), RawCell(0x45, 0x10, 0xa7, 0xb7),
      RawCell(0x46, 0x10, 0xa8, 0xb8), RawCell(0x47, 0x10, 0xa9, 0xb9)};
  for (const auto& cell : prefix_cells) {
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = cell;
    const auto result = page::InsertIndexBtreeCell(&prefix_tree, request);
    Require(result.ok(), "prefix setup insert failed");
  }
  const auto prefix_live = ReachableLiveCells(prefix_tree);
  const auto encoded_prefix = RawEncodedPrefix(0x42);
  const auto prefix_scan = page::PrefixScanIndexBtreePhysicalTree(prefix_tree, encoded_prefix);
  RequireScanContracts(prefix_scan, "prefix", "forward");
  RequireFencePruning(prefix_scan, "prefix");
  Require(HasEvidence(prefix_scan.evidence, "prefix_fence_pruning=true"),
          "prefix scan missing mode-specific pruning evidence");
  RequireScanMatches(prefix_scan, FilterPrefix(prefix_live, encoded_prefix), "prefix");
  Require(prefix_scan.locators.size() == 3, "prefix scan count mismatch");

  const auto terminated_prefix =
      page::PrefixScanIndexBtreePhysicalTree(prefix_tree, RawEncodedKey(0x42, 0x10));
  Require(!terminated_prefix.ok() &&
              terminated_prefix.diagnostic.diagnostic_code ==
                  "SB-INDEX-BTREE-PHYSICAL-SCAN-PREFIX-TERMINATED",
          "prefix scan accepted manually terminated full key bytes");
}

void DeleteTombstoneCleanupAndFreeSpaceReuse() {
  const auto index_uuid = GeneratedUuid(platform::UuidKind::object, 1700000700000ull, 0xd4);
  constexpr platform::u32 page_size = 768;
  const std::size_t capacity = MaxRootLeafCapacity(index_uuid, page_size);
  Require(capacity >= 3, "delete cleanup test requires root leaf capacity >= 3");

  auto init = page::InitializeIndexBtreePhysicalTree(index_uuid, page_size);
  Require(init.ok(), "delete cleanup tree initialization failed");
  page::IndexBtreePhysicalTree tree = std::move(init.tree);

  std::vector<page::IndexBtreeCell> inserted;
  for (std::size_t i = 0; i < capacity; ++i) {
    inserted.push_back(Cell(static_cast<std::int64_t>(100 + i),
                            static_cast<platform::byte>(0x30 + i),
                            static_cast<platform::byte>(0x70 + i)));
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = inserted.back();
    const auto result = page::InsertIndexBtreeCell(&tree, request);
    Require(result.ok(), "root leaf fill insert failed");
    Require(!result.split_performed, "root leaf filled too early");
  }
  const auto root_before_delete = Fetch(tree, tree.root_page_number);
  Require(root_before_delete.page_kind == page::IndexBtreePageKind::root, "root leaf kind mismatch");
  Require(root_before_delete.tree_level == 0, "root leaf unexpectedly split");

  page::IndexBtreePhysicalDeleteRequest delete_request;
  delete_request.cell = inserted[capacity / 2];
  const auto deleted = page::DeleteIndexBtreeCell(&tree, delete_request);
  Require(deleted.ok(), "delete exact locator failed");
  Require(deleted.deleted, "delete result did not mark deleted");
  Require(deleted.tombstone_marked, "delete did not mark tombstone");
  Require(HasEvidence(deleted.evidence, "visibility_authority=false"),
          "delete missing visibility non-authority evidence");
  Require(HasEvidence(deleted.evidence, "transaction_finality_authority=false"),
          "delete missing finality non-authority evidence");
  Require(HasEvidence(deleted.evidence, "recovery_authority=false"),
          "delete missing recovery non-authority evidence");
  Require(ReachableDeletedCellCount(tree) == 1, "deferred delete tombstone missing");

  const auto tombstone_scan = page::OrderedScanIndexBtreePhysicalTree(tree);
  RequireScanContracts(tombstone_scan, "ordered", "forward");
  std::vector<page::IndexBtreeCell> expected_after_delete = inserted;
  expected_after_delete.erase(expected_after_delete.begin() + static_cast<std::ptrdiff_t>(capacity / 2));
  RequireSorted(expected_after_delete, "expected-after-delete");
  RequireScanMatches(tombstone_scan, expected_after_delete, "tombstone-exclusion");

  const std::size_t page_count_before_reuse = tree.pages.size();
  page::IndexBtreePhysicalInsertRequest reuse_request;
  reuse_request.cell = Cell(1000, 0x7a, 0x7b);
  const auto reused = page::InsertIndexBtreeCell(&tree, reuse_request);
  Require(reused.ok(), "free-space reuse insert failed");
  Require(!reused.split_performed, "tombstone cleanup failed to prevent split");
  Require(tree.pages.size() == page_count_before_reuse,
          "free-space reuse unexpectedly allocated a page");
  Require(HasEvidence(reused.evidence, "bottom_up_cleanup_before_split=true"),
          "insert did not report bottom-up cleanup before split");
  Require(HasEvidence(reused.evidence, "free_space_reused_before_split=true"),
          "insert did not report free-space reuse");
  VerifyTreeOrderingAndMetadata(tree, capacity);
}

void DeleteLeafRebalanceAndMergeRootCollapse() {
  auto rebalance_init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700000800000ull, 0xe4), 768);
  Require(rebalance_init.ok(), "rebalance tree initialization failed");
  page::IndexBtreePhysicalTree rebalance_tree = std::move(rebalance_init.tree);

  constexpr std::size_t rebalance_insert_count = 36;
  for (std::size_t i = 0; i < rebalance_insert_count; ++i) {
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = Cell(static_cast<std::int64_t>(i), static_cast<platform::byte>(0x80 + i),
                        static_cast<platform::byte>(0xb0 + i));
    const auto inserted = page::InsertIndexBtreeCell(&rebalance_tree, request);
    Require(inserted.ok(), "rebalance setup insert failed");
  }
  VerifyTreeOrderingAndMetadata(rebalance_tree, rebalance_insert_count);

  auto leaves = AllLeaves(rebalance_tree);
  std::vector<page::IndexBtreeCell> rebalance_deletes;
  for (std::size_t i = 0; i + 1 < leaves.size(); ++i) {
    if (leaves[i].parent_page_number == leaves[i + 1].parent_page_number &&
        leaves[i].cells.size() >= 2 && leaves[i + 1].cells.size() > 2) {
      rebalance_deletes.assign(leaves[i].cells.begin(), leaves[i].cells.end() - 1);
      break;
    }
  }
  if (rebalance_deletes.empty()) {
    for (std::size_t i = 1; i < leaves.size(); ++i) {
      if (leaves[i - 1].parent_page_number == leaves[i].parent_page_number &&
          leaves[i - 1].cells.size() > 2 && leaves[i].cells.size() >= 2) {
        rebalance_deletes.assign(leaves[i].cells.begin(), leaves[i].cells.end() - 1);
        break;
      }
    }
  }
  Require(!rebalance_deletes.empty(), "rebalance setup did not produce a borrowable leaf pair");

  page::IndexBtreePhysicalDeleteResult rebalanced;
  for (const auto& cell : rebalance_deletes) {
    page::IndexBtreePhysicalDeleteRequest rebalance_request;
    rebalance_request.cell = cell;
    rebalanced = page::DeleteIndexBtreeCell(&rebalance_tree, rebalance_request);
    Require(rebalanced.ok(), "rebalance delete failed");
  }
  Require(rebalanced.rebalance_performed, "leaf rebalance was not performed");
  Require(HasEvidence(rebalanced.evidence, "leaf_rebalance_from_"),
          "rebalance evidence missing");
  VerifyTreeOrderingAndMetadata(rebalance_tree, rebalance_insert_count - rebalance_deletes.size());

  const auto merge_index_uuid = GeneratedUuid(platform::UuidKind::object, 1700000900000ull, 0xf4);
  constexpr platform::u32 merge_page_size = 400;
  const std::size_t merge_capacity = MaxRootLeafCapacity(merge_index_uuid, merge_page_size);
  Require(merge_capacity >= 2, "merge test requires root leaf capacity >= 2");
  auto merge_init = page::InitializeIndexBtreePhysicalTree(merge_index_uuid, merge_page_size);
  Require(merge_init.ok(), "merge tree initialization failed");
  page::IndexBtreePhysicalTree merge_tree = std::move(merge_init.tree);

  std::vector<page::IndexBtreeCell> merge_cells;
  for (std::size_t i = 0; i < merge_capacity + 1; ++i) {
    merge_cells.push_back(Cell(200 + i,
                               static_cast<platform::byte>(0xc0 + i),
                               static_cast<platform::byte>(0xd0 + i)));
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = merge_cells.back();
    const auto inserted = page::InsertIndexBtreeCell(&merge_tree, request);
    Require(inserted.ok(), "merge setup insert failed");
  }
  VerifyTreeOrderingAndMetadata(merge_tree, merge_cells.size());
  Require(Fetch(merge_tree, merge_tree.root_page_number).tree_level == 1,
          "merge setup did not split root to leaves");

  const auto merge_leaves = AllLeaves(merge_tree);
  Require(merge_leaves.size() == 2, "merge setup expected two leaves");
  const std::vector<page::IndexBtreeCell> delete_cells = merge_leaves.front().cells;

  page::IndexBtreePhysicalDeleteResult last_delete;
  for (const auto& cell : delete_cells) {
    page::IndexBtreePhysicalDeleteRequest request;
    request.cell = cell;
    last_delete = page::DeleteIndexBtreeCell(&merge_tree, request);
    Require(last_delete.ok(), "merge delete failed");
  }
  Require(last_delete.merge_performed, "leaf merge was not performed");
  Require(last_delete.root_collapsed, "root did not collapse after merge");
  Require(HasEvidence(last_delete.evidence, "leaf_merge_"), "merge evidence missing");
  Require(HasEvidence(last_delete.evidence, "root_height_reduced=true"),
          "root height reduction evidence missing");
  const auto root = Fetch(merge_tree, merge_tree.root_page_number);
  Require(root.page_kind == page::IndexBtreePageKind::root, "collapsed root kind mismatch");
  Require(root.tree_level == 0, "collapsed root did not become a leaf root");
  VerifyTreeOrderingAndMetadata(merge_tree, merge_cells.size() - delete_cells.size());
  Require(merge_tree.pages.size() > 1, "root collapse test expected stale orphan page images");
  const auto collapsed_scan = page::OrderedScanIndexBtreePhysicalTree(merge_tree);
  RequireScanContracts(collapsed_scan, "ordered", "forward");
  RequireScanMatches(collapsed_scan, ReachableLiveCells(merge_tree), "collapsed-root");
  for (const auto& locator : collapsed_scan.locators) {
    Require(locator.leaf_page_number == merge_tree.root_page_number,
            "collapsed root scan visited stale orphan leaf page");
  }
}

void DeleteCrossParentLeafStructuralCompactionRebuild() {
  const auto index_uuid =
      GeneratedUuid(platform::UuidKind::object, 1700000950000ull, 0xf5);
  std::vector<page::IndexBtreeCell> cells;
  for (std::size_t i = 0; i < 10; ++i) {
    cells.push_back(Cell(500 + static_cast<std::int64_t>(i),
                         static_cast<platform::byte>(0x50 + i),
                         static_cast<platform::byte>(0x90 + i)));
  }

  page::IndexBtreePhysicalBulkBuildRequest build_request;
  build_request.index_uuid = index_uuid;
  build_request.page_size = 768;
  build_request.leaf_entry_capacity = 2;
  build_request.internal_entry_capacity = 2;
  build_request.sorted_cells = cells;
  build_request.sorted_order_proof_valid = true;
  const auto built = page::BuildIndexBtreePhysicalBulkLoadedTree(build_request);
  Require(built.ok(), "cross-parent compaction bulk build failed");
  page::IndexBtreePhysicalTree tree = built.tree;

  const auto leaves = AllLeaves(tree);
  Require(leaves.size() == 5, "cross-parent compaction setup expected five leaves");
  const auto target_leaf = leaves.back();
  Require(target_leaf.cells.size() == 2,
          "cross-parent compaction target leaf size mismatch");
  Require(target_leaf.left_sibling_page_number != 0,
          "cross-parent compaction target needs left sibling");
  const auto left_sibling = Fetch(tree, target_leaf.left_sibling_page_number);
  Require(left_sibling.parent_page_number != target_leaf.parent_page_number,
          "cross-parent compaction setup did not isolate target leaf parent");

  page::IndexBtreePhysicalDeleteRequest delete_request;
  delete_request.cell = target_leaf.cells.front();
  const auto deleted = page::DeleteIndexBtreeCell(&tree, delete_request);
  if (!deleted.ok()) {
    std::cerr << "cross-parent compaction delete diagnostic="
              << deleted.diagnostic.diagnostic_code
              << " message_key=" << deleted.diagnostic.message_key << '\n';
  }
  Require(deleted.ok(), "cross-parent compaction delete failed");
  Require(deleted.deleted && deleted.tombstone_marked,
          "cross-parent compaction delete did not mark exact tombstone");
  Require(deleted.structural_rebuild_performed,
          "cross-parent compaction did not perform structural rebuild");
  Require(HasEvidence(deleted.evidence, "leaf_structural_compaction_rebuild=true"),
          "cross-parent compaction missing rebuild evidence");
  Require(HasEvidence(deleted.evidence,
                      "leaf_delete_cross_parent_structural_compaction=true"),
          "cross-parent compaction missing route evidence");
  Require(HasEvidence(deleted.evidence, "visibility_authority=false"),
          "cross-parent compaction claimed visibility authority");
  Require(HasEvidence(deleted.evidence, "transaction_finality_authority=false"),
          "cross-parent compaction claimed finality authority");
  Require(HasEvidence(deleted.evidence, "recovery_authority=false"),
          "cross-parent compaction claimed recovery authority");

  cells.erase(std::remove_if(cells.begin(),
                             cells.end(),
                             [&](const page::IndexBtreeCell& cell) {
                               return SameLiveEntry(cell, delete_request.cell);
                             }),
              cells.end());
  VerifyTreeOrderingAndMetadata(tree, cells.size());
  const auto validation = page::ValidateIndexBtreePhysicalTree(tree);
  RequireValidationContracts(validation, cells.size(), "cross-parent-compaction");
  const auto scan = page::OrderedScanIndexBtreePhysicalTree(tree);
  RequireScanContracts(scan, "ordered", "forward");
  RequireScanMatches(scan, cells, "cross-parent-compaction");
  SnapshotReopenRoundTrip(tree, cells, "cross-parent-compaction-reopen");
}

void ConcurrentLatchStress() {
  auto init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700001000000ull, 0x11), 512);
  Require(init.ok(), "concurrent latch tree initialization failed");
  page::IndexBtreePhysicalTree tree = std::move(init.tree);

  std::vector<page::IndexBtreeCell> inserted;
  for (int i = 0; i < 96; ++i) {
    inserted.push_back(Cell(i,
                            static_cast<platform::byte>(0x20 + i),
                            static_cast<platform::byte>(0x90 + i)));
  }

  std::mutex failure_mutex;
  std::vector<std::string> failures;
  auto record_failure = [&](std::string message) {
    std::lock_guard<std::mutex> guard(failure_mutex);
    failures.push_back(std::move(message));
  };

  std::atomic<bool> insert_done{false};
  std::vector<std::thread> scan_threads;
  for (int t = 0; t < 3; ++t) {
    scan_threads.emplace_back([&]() {
      while (!insert_done.load()) {
        const auto scan = page::OrderedScanIndexBtreePhysicalTree(tree);
        if (!scan.ok()) {
          record_failure("concurrent insert scan failed: " + scan.diagnostic.diagnostic_code);
          return;
        }
        for (std::size_t i = 1; i < scan.locators.size(); ++i) {
          if (LocatorLess(scan.locators[i], scan.locators[i - 1])) {
            record_failure("concurrent insert scan observed unordered locators");
            return;
          }
        }
      }
    });
  }

  std::vector<std::thread> insert_threads;
  for (int t = 0; t < 4; ++t) {
    insert_threads.emplace_back([&, t]() {
      for (int i = t; i < static_cast<int>(inserted.size()); i += 4) {
        page::IndexBtreePhysicalInsertRequest request;
        request.cell = inserted[static_cast<std::size_t>(i)];
        const auto result = page::InsertIndexBtreeCell(&tree, request);
        if (!result.ok()) {
          record_failure("concurrent insert failed: " + result.diagnostic.diagnostic_code);
          return;
        }
        if (!HasEvidence(result.evidence, "latch_authority=structural_only") ||
            !HasEvidence(result.evidence, "recovery_authority=false")) {
          record_failure("concurrent insert missing latch/non-authority evidence");
          return;
        }
      }
    });
  }
  for (auto& thread : insert_threads) {
    thread.join();
  }
  insert_done.store(true);
  for (auto& thread : scan_threads) {
    thread.join();
  }
  Require(failures.empty(), failures.empty() ? "" : failures.front());

  std::sort(inserted.begin(), inserted.end(), [](const auto& left, const auto& right) {
    return CompareCells(left, right) < 0;
  });
  VerifyTreeOrderingAndMetadata(tree, inserted.size());
  const auto inserted_scan = page::OrderedScanIndexBtreePhysicalTree(tree);
  RequireScanContracts(inserted_scan, "ordered", "forward");
  RequireScanMatches(inserted_scan, inserted, "concurrent-inserts");

  std::vector<page::IndexBtreeCell> deleted;
  for (std::size_t i = 0; i < inserted.size(); i += 3) {
    deleted.push_back(inserted[i]);
  }
  std::atomic<bool> delete_done{false};
  failures.clear();
  scan_threads.clear();
  for (int t = 0; t < 3; ++t) {
    scan_threads.emplace_back([&]() {
      while (!delete_done.load()) {
        const auto scan = page::OrderedScanIndexBtreePhysicalTree(tree);
        if (!scan.ok()) {
          record_failure("concurrent delete scan failed: " + scan.diagnostic.diagnostic_code);
          return;
        }
        for (std::size_t i = 1; i < scan.locators.size(); ++i) {
          if (LocatorLess(scan.locators[i], scan.locators[i - 1])) {
            record_failure("concurrent delete scan observed unordered locators");
            return;
          }
        }
      }
    });
  }

  std::vector<std::thread> delete_threads;
  for (int t = 0; t < 4; ++t) {
    delete_threads.emplace_back([&, t]() {
      for (int i = t; i < static_cast<int>(deleted.size()); i += 4) {
        page::IndexBtreePhysicalDeleteRequest request;
        request.cell = deleted[static_cast<std::size_t>(i)];
        const auto result = page::DeleteIndexBtreeCell(&tree, request);
        if (!result.ok()) {
          record_failure("concurrent delete failed: " + result.diagnostic.diagnostic_code);
          return;
        }
        if (!HasEvidence(result.evidence, "optimistic_descent_validated=true") ||
            !HasEvidence(result.evidence, "recovery_authority=false")) {
          record_failure("concurrent delete missing descent/non-authority evidence");
          return;
        }
      }
    });
  }
  for (auto& thread : delete_threads) {
    thread.join();
  }
  delete_done.store(true);
  for (auto& thread : scan_threads) {
    thread.join();
  }
  Require(failures.empty(), failures.empty() ? "" : failures.front());

  std::vector<page::IndexBtreeCell> expected_live;
  for (const auto& cell : inserted) {
    const bool was_deleted =
        std::any_of(deleted.begin(), deleted.end(), [&](const auto& removed) {
          return SameLiveEntry(cell, removed);
        });
    if (!was_deleted) {
      expected_live.push_back(cell);
    }
  }
  const auto validation = page::ValidateIndexBtreePhysicalTree(tree);
  RequireValidationContracts(validation, expected_live.size(), "concurrent-delete-final");
  const auto final_scan = page::OrderedScanIndexBtreePhysicalTree(tree);
  RequireScanContracts(final_scan, "ordered", "forward");
  RequireScanMatches(final_scan, expected_live, "concurrent-delete-final");
  SnapshotReopenRoundTrip(tree, expected_live, "concurrent-latch-stress-reopen");
}

void CrashReopenImageValidation() {
  const auto index_uuid = GeneratedUuid(platform::UuidKind::object, 1700001100000ull, 0x21);
  constexpr platform::u32 page_size = 400;
  const std::size_t capacity = MaxRootLeafCapacity(index_uuid, page_size);
  Require(capacity >= 2, "crash/reopen test requires root leaf capacity >= 2");
  auto init = page::InitializeIndexBtreePhysicalTree(index_uuid, page_size);
  Require(init.ok(), "crash/reopen tree initialization failed");
  page::IndexBtreePhysicalTree tree = std::move(init.tree);

  std::vector<page::IndexBtreeCell> live;
  for (std::size_t i = 0; i < capacity; ++i) {
    live.push_back(Cell(300 + i,
                        static_cast<platform::byte>(0x30 + i),
                        static_cast<platform::byte>(0x70 + i)));
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = live.back();
    const auto result = page::InsertIndexBtreeCell(&tree, request);
    Require(result.ok(), "pre-split insert failed");
  }
  std::sort(live.begin(), live.end(), [](const auto& left, const auto& right) {
    return CompareCells(left, right) < 0;
  });
  tree = SnapshotReopenRoundTrip(tree, live, "before-root-split-interruption-point");

  page::IndexBtreePhysicalInsertRequest split_request;
  split_request.cell = Cell(900, 0x80, 0x81);
  live.push_back(split_request.cell);
  const auto split = page::InsertIndexBtreeCell(&tree, split_request);
  Require(split.ok(), "root split insert failed");
  Require(split.split_performed, "crash/reopen test did not trigger split");
  std::sort(live.begin(), live.end(), [](const auto& left, const auto& right) {
    return CompareCells(left, right) < 0;
  });
  tree = SnapshotReopenRoundTrip(tree, live, "after-root-split-interruption-point");

  page::IndexBtreePhysicalDeleteRequest delete_request;
  delete_request.cell = live[live.size() / 2];
  const auto deleted_cell = delete_request.cell;
  const auto deleted = page::DeleteIndexBtreeCell(&tree, delete_request);
  Require(deleted.ok(), "crash/reopen delete failed");
  live.erase(std::remove_if(live.begin(),
                            live.end(),
                            [&](const auto& cell) {
                              return SameLiveEntry(cell, deleted_cell);
                            }),
             live.end());
  tree = SnapshotReopenRoundTrip(tree, live, "after-delete-interruption-point");

  auto merge_init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700001200000ull, 0x22), page_size);
  Require(merge_init.ok(), "crash/reopen merge tree initialization failed");
  page::IndexBtreePhysicalTree merge_tree = std::move(merge_init.tree);
  std::vector<page::IndexBtreeCell> merge_live;
  for (std::size_t i = 0; i < capacity + 1; ++i) {
    merge_live.push_back(Cell(1200 + i,
                              static_cast<platform::byte>(0x90 + i),
                              static_cast<platform::byte>(0xa0 + i)));
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = merge_live.back();
    const auto result = page::InsertIndexBtreeCell(&merge_tree, request);
    Require(result.ok(), "merge crash/reopen setup insert failed");
  }
  std::sort(merge_live.begin(), merge_live.end(), [](const auto& left, const auto& right) {
    return CompareCells(left, right) < 0;
  });
  merge_tree = SnapshotReopenRoundTrip(merge_tree, merge_live, "before-merge-interruption-point");

  const auto merge_leaves = AllLeaves(merge_tree);
  Require(merge_leaves.size() == 2, "merge crash/reopen setup expected two leaves");
  const std::vector<page::IndexBtreeCell> merge_deletes = merge_leaves.front().cells;
  page::IndexBtreePhysicalDeleteResult last_delete;
  for (const auto& cell : merge_deletes) {
    page::IndexBtreePhysicalDeleteRequest request;
    request.cell = cell;
    last_delete = page::DeleteIndexBtreeCell(&merge_tree, request);
    Require(last_delete.ok(), "merge crash/reopen delete failed");
    merge_live.erase(std::remove_if(merge_live.begin(),
                                    merge_live.end(),
                                    [&](const auto& live_cell) {
                                      return SameLiveEntry(live_cell, cell);
                                    }),
                     merge_live.end());
  }
  Require(last_delete.merge_performed, "crash/reopen merge was not performed");
  Require(last_delete.root_collapsed, "crash/reopen root collapse was not performed");
  merge_tree =
      SnapshotReopenRoundTrip(merge_tree, merge_live, "after-merge-root-collapse-interruption-point");

  const auto exported = page::ExportIndexBtreePhysicalTreeImage(merge_tree);
  Require(exported.ok(), "corruption image export failed");
  auto corrupted = exported.image;
  Require(!corrupted.pages.empty() && !corrupted.pages.front().serialized.empty(),
          "corruption test needs serialized page bytes");
  corrupted.pages.front().serialized.back() ^= 0x5au;
  const auto refused = page::ImportIndexBtreePhysicalTreeImage(corrupted);
  Require(!refused.ok(), "corrupted serialized image was accepted");
  Require(refused.diagnostic.diagnostic_code == "SB-INDEX-BTREE-PAGE-CHECKSUM-MISMATCH",
          "corrupted serialized image diagnostic mismatch");
}

void LeafRangePartitionCorruptionRefusal() {
  constexpr platform::u32 page_size = 768;
  const auto index_uuid = GeneratedUuid(platform::UuidKind::object, 1700001300000ull, 0x23);
  const std::size_t capacity = MaxRootLeafCapacity(index_uuid, page_size);
  Require(capacity >= 4, "leaf range corruption needs root leaf capacity >= 4");
  auto init = page::InitializeIndexBtreePhysicalTree(
      index_uuid, page_size);
  Require(init.ok(), "leaf range corruption tree initialization failed");
  page::IndexBtreePhysicalTree tree = std::move(init.tree);

  std::vector<page::IndexBtreeCell> live;
  for (std::size_t i = 0; i < capacity + 2; ++i) {
    live.push_back(Cell(2000 + i,
                        static_cast<platform::byte>(0x20 + i),
                        static_cast<platform::byte>(0x80 + i)));
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = live.back();
    const auto result = page::InsertIndexBtreeCell(&tree, request);
    Require(result.ok(), "leaf range corruption setup insert failed");
  }
  std::sort(live.begin(), live.end(), [](const auto& left, const auto& right) {
    return CompareCells(left, right) < 0;
  });
  SnapshotReopenRoundTrip(tree, live, "leaf-range-corruption-baseline");

  const auto leaves = AllLeaves(tree);
  Require(leaves.size() >= 2, "leaf range corruption needs a multi-leaf tree");
  std::optional<std::pair<page::IndexBtreePageBody, page::IndexBtreePageBody>> adjacent_pair;
  for (std::size_t i = 0; i + 1 < leaves.size(); ++i) {
    if (leaves[i].parent_page_number == leaves[i + 1].parent_page_number &&
        leaves[i].cells.size() >= 2 && leaves[i + 1].cells.size() >= 2) {
      adjacent_pair = std::make_pair(leaves[i], leaves[i + 1]);
      break;
    }
  }
  Require(adjacent_pair.has_value(),
          "leaf range corruption needs adjacent leaves with the same parent");

  page::IndexBtreePageBody left = adjacent_pair->first;
  const page::IndexBtreePageBody right = adjacent_pair->second;
  page::IndexBtreeCell overlapping = right.cells.front();
  overlapping.row_uuid = GeneratedUuid(platform::UuidKind::row, 1700001400000ull, 0xf0);
  overlapping.version_uuid = GeneratedUuid(platform::UuidKind::row, 1700001500000ull, 0xf1);
  Require(CompareCells(overlapping, right.cells.front()) > 0,
          "crafted overlap cell did not sort after right first cell");
  Require(CompareCells(left.cells[left.cells.size() - 2], overlapping) < 0,
          "crafted overlap cell would break local left page order");
  left.cells.back() = overlapping;

  const auto exported = page::ExportIndexBtreePhysicalTreeImage(tree);
  Require(exported.ok(), "leaf range corruption export failed");
  auto corrupted = exported.image;
  ReplaceImagePage(&corrupted, left);

  page::IndexBtreePageBody parent = Fetch(tree, left.parent_page_number);
  bool replaced_fence = false;
  for (auto& fence : parent.cells) {
    if (fence.child_page_number == left.page_number) {
      fence = FenceForChild(left);
      replaced_fence = true;
      break;
    }
  }
  Require(replaced_fence, "leaf range corruption parent fence missing");
  std::sort(parent.cells.begin(), parent.cells.end(), [](const auto& a, const auto& b) {
    return CompareCells(a, b) < 0;
  });
  ReplaceImagePage(&corrupted, parent);

  const auto refused = page::ImportIndexBtreePhysicalTreeImage(corrupted);
  Require(!refused.ok(), "overlapping reachable leaf ranges were accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "SB-INDEX-BTREE-PHYSICAL-VALIDATE-LEAF-RANGE-PARTITION",
          "overlapping reachable leaf range diagnostic mismatch");
}

page::IndexBtreePhysicalTree BuildRepairReportTree(std::vector<page::IndexBtreeCell>* live) {
  auto init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700001600000ull, 0x30), 400);
  Require(init.ok(), "repair/report tree initialization failed");
  page::IndexBtreePhysicalTree tree = std::move(init.tree);
  for (int i = 0; i < 24; ++i) {
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = Cell(3000 + i,
                        static_cast<platform::byte>(0x30 + i),
                        static_cast<platform::byte>(0x70 + i));
    const auto inserted = page::InsertIndexBtreeCell(&tree, request);
    Require(inserted.ok(), "repair/report setup insert failed");
    live->push_back(request.cell);
  }
  std::sort(live->begin(), live->end(), [](const auto& left, const auto& right) {
    return CompareCells(left, right) < 0;
  });
  return tree;
}

page::IndexBtreePhysicalTree TreeFromImage(const page::IndexBtreePhysicalTreeImage& image) {
  page::IndexBtreePhysicalTree tree;
  tree.page_size = image.page_size;
  tree.index_uuid = image.index_uuid;
  tree.root_page_number = image.root_page_number;
  tree.next_page_number = image.next_page_number;
  tree.pages = image.pages;
  return tree;
}

void RequireCorruptionClass(page::IndexBtreePhysicalTree tree,
                            page::IndexBtreePhysicalCorruptionClass expected,
                            std::string_view diagnostic_substring,
                            std::string_view label) {
  const auto report = page::BuildIndexBtreePhysicalTreeReport(tree);
  Require(!report.ok(), std::string(label) + " corrupt report unexpectedly passed");
  Require(!report.report.valid, std::string(label) + " corrupt report marked valid");
  if (report.report.corruption_class != expected) {
    std::cerr << "classification mismatch label=" << label
              << " expected=" << page::IndexBtreePhysicalCorruptionClassName(expected)
              << " actual="
              << page::IndexBtreePhysicalCorruptionClassName(report.report.corruption_class)
              << " diagnostic=" << report.report.exact_diagnostic_code << '\n';
    std::exit(EXIT_FAILURE);
  }
  Require(report.report.exact_diagnostic_code.find(diagnostic_substring) != std::string::npos,
          std::string(label) + " exact diagnostic mismatch");
  Require(HasEvidence(report.report.support_bundle_rows,
                      std::string("corruption_class=") +
                          page::IndexBtreePhysicalCorruptionClassName(expected)),
          std::string(label) + " support row missing corruption class");
  Require(HasEvidence(report.report.support_bundle_rows, "visibility_authority=false"),
          std::string(label) + " support row claimed visibility authority");
  Require(HasEvidence(report.report.support_bundle_rows, "visibility=false"),
          std::string(label) + " support row missing visibility non-authority alias");
  Require(HasEvidence(report.report.support_bundle_rows, "authorization=false"),
          std::string(label) + " support row missing authorization non-authority alias");
  Require(HasEvidence(report.report.support_bundle_rows, "transaction_finality=false"),
          std::string(label) + " support row missing finality non-authority alias");
  Require(HasEvidence(report.report.support_bundle_rows, "recovery=false"),
          std::string(label) + " support row missing recovery non-authority alias");
  Require(HasEvidence(report.report.support_bundle_rows, "authorization_authority=false"),
          std::string(label) + " support row claimed authorization authority");
  Require(HasEvidence(report.report.support_bundle_rows, "transaction_finality_authority=false"),
          std::string(label) + " support row claimed transaction finality authority");
  Require(HasEvidence(report.report.support_bundle_rows, "recovery_authority=false"),
          std::string(label) + " support row claimed recovery authority");
}

void ValidationReportSupportBundleAndCorruptionClassification() {
  auto report_init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700001550000ull, 0x2f), 1024);
  Require(report_init.ok(), "valid report tree initialization failed");
  page::IndexBtreePhysicalTree report_tree = std::move(report_init.tree);
  std::vector<page::IndexBtreeCell> report_live = {
      Cell(2900, 0x2a, 0x3a), Cell(2901, 0x2b, 0x3b), Cell(2902, 0x2c, 0x3c)};
  for (const auto& cell : report_live) {
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = cell;
    Require(page::InsertIndexBtreeCell(&report_tree, request).ok(),
            "valid report setup insert failed");
  }
  page::IndexBtreePhysicalDeleteRequest delete_request;
  delete_request.cell = report_live[1];
  const auto deleted = page::DeleteIndexBtreeCell(&report_tree, delete_request);
  Require(deleted.ok(), "report tombstone delete failed");
  report_live.erase(report_live.begin() + 1);

  const auto report_result = page::BuildIndexBtreePhysicalTreeReport(report_tree);
  Require(report_result.ok(), "valid report failed");
  RequireReportContracts(report_result.report, report_live.size(), "valid-report");
  Require(report_result.report.tombstone_deleted_entry_count == 1,
          "valid report did not count reachable tombstone");
  Require(HasEvidence(report_result.report.support_bundle_rows,
                      "tombstone_deleted_entry_count=1"),
          "valid report support row did not include exact tombstone count");
  Require(HasEvidence(report_result.evidence, "visibility_authority=false"),
          "valid report result missing visibility non-authority evidence");

  std::vector<page::IndexBtreeCell> live;
  page::IndexBtreePhysicalTree tree = BuildRepairReportTree(&live);
  const auto exported = page::ExportIndexBtreePhysicalTreeImage(tree);
  Require(exported.ok(), "classification export failed");

  auto checksum_image = exported.image;
  checksum_image.pages.front().serialized.back() ^= 0x11u;
  RequireCorruptionClass(TreeFromImage(checksum_image),
                         page::IndexBtreePhysicalCorruptionClass::checksum,
                         "CHECKSUM",
                         "checksum");

  auto page_image = exported.image;
  page_image.pages.front().page_number = 99999;
  RequireCorruptionClass(TreeFromImage(page_image),
                         page::IndexBtreePhysicalCorruptionClass::page,
                         "PAGE",
                         "page-number");

  const auto root = Fetch(tree, tree.root_page_number);
  Require(root.tree_level > 0, "classification tree needs internal root");
  const auto first_child = Fetch(tree, root.cells.front().child_page_number);

  auto parent_image = exported.image;
  page::IndexBtreePageBody wrong_parent = first_child;
  wrong_parent.parent_page_number = 99998;
  ReplaceImagePage(&parent_image, wrong_parent);
  RequireCorruptionClass(TreeFromImage(parent_image),
                         page::IndexBtreePhysicalCorruptionClass::parent,
                         "PARENT",
                         "parent");

  auto fence_image = exported.image;
  page::IndexBtreePageBody bad_fence_parent = root;
  for (auto& fence : bad_fence_parent.cells) {
    if (fence.child_page_number == first_child.page_number) {
      fence.encoded_key = RawEncodedKey(0x00, 0x01);
      fence.row_uuid = GeneratedUuid(platform::UuidKind::row, 1700001700000ull, 0x41);
      fence.version_uuid = GeneratedUuid(platform::UuidKind::row, 1700001800000ull, 0x42);
      break;
    }
  }
  ReplaceImagePage(&fence_image, bad_fence_parent);
  RequireCorruptionClass(TreeFromImage(fence_image),
                         page::IndexBtreePhysicalCorruptionClass::fence,
                         "FENCE",
                         "fence");

  const auto leaves = AllLeaves(tree);
  Require(leaves.size() >= 2, "classification tree needs multiple leaves");
  auto sibling_image = exported.image;
  page::IndexBtreePageBody bad_sibling = leaves.front();
  bad_sibling.right_sibling_page_number = 0;
  ReplaceImagePage(&sibling_image, bad_sibling);
  RequireCorruptionClass(TreeFromImage(sibling_image),
                         page::IndexBtreePhysicalCorruptionClass::sibling,
                         "SIBLING",
                         "sibling");

  const auto order_index_uuid =
      GeneratedUuid(platform::UuidKind::object, 1700001850000ull, 0x40);
  const std::size_t order_capacity = MaxRootLeafCapacity(order_index_uuid, 768);
  Require(order_capacity >= 4, "order classification capacity too small");
  auto order_init = page::InitializeIndexBtreePhysicalTree(order_index_uuid, 768);
  Require(order_init.ok(), "order classification tree initialization failed");
  page::IndexBtreePhysicalTree order_tree = std::move(order_init.tree);
  for (std::size_t i = 0; i < order_capacity + 2; ++i) {
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = Cell(3500 + i,
                        static_cast<platform::byte>(0x50 + i),
                        static_cast<platform::byte>(0x90 + i));
    Require(page::InsertIndexBtreeCell(&order_tree, request).ok(),
            "order classification setup insert failed");
  }
  const auto order_exported = page::ExportIndexBtreePhysicalTreeImage(order_tree);
  Require(order_exported.ok(), "order classification export failed");
  const auto order_leaves = AllLeaves(order_tree);
  Require(order_leaves.size() >= 2, "order classification needs multiple leaves");
  auto order_image = order_exported.image;
  page::IndexBtreePageBody overlapping_left = order_leaves.front();
  page::IndexBtreeCell overlap = order_leaves[1].cells.front();
  overlap.row_uuid = GeneratedUuid(platform::UuidKind::row, 1700001900000ull, 0x43);
  overlap.version_uuid = GeneratedUuid(platform::UuidKind::row, 1700002000000ull, 0x44);
  overlapping_left.cells.back() = overlap;
  ReplaceImagePage(&order_image, overlapping_left);
  page::IndexBtreePageBody order_parent = Fetch(order_tree, overlapping_left.parent_page_number);
  for (auto& fence : order_parent.cells) {
    if (fence.child_page_number == overlapping_left.page_number) {
      fence = FenceForChild(overlapping_left);
      break;
    }
  }
  std::sort(order_parent.cells.begin(), order_parent.cells.end(), [](const auto& a, const auto& b) {
    return CompareCells(a, b) < 0;
  });
  ReplaceImagePage(&order_image, order_parent);
  RequireCorruptionClass(TreeFromImage(order_image),
                         page::IndexBtreePhysicalCorruptionClass::order,
                         "LEAF-RANGE",
                         "order");

  auto duplicate_init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700002100000ull, 0x45), 1024);
  Require(duplicate_init.ok(), "duplicate classification tree initialization failed");
  page::IndexBtreePhysicalTree duplicate_tree = std::move(duplicate_init.tree);
  std::vector<page::IndexBtreeCell> duplicate_cells = {
      Cell(1, 0x46, 0x56), Cell(2, 0x47, 0x57), Cell(3, 0x48, 0x58)};
  for (const auto& cell : duplicate_cells) {
    page::IndexBtreePhysicalInsertRequest request;
    request.cell = cell;
    Require(page::InsertIndexBtreeCell(&duplicate_tree, request).ok(),
            "duplicate classification setup insert failed");
  }
  const auto duplicate_exported = page::ExportIndexBtreePhysicalTreeImage(duplicate_tree);
  Require(duplicate_exported.ok(), "duplicate classification export failed");
  auto duplicate_image = duplicate_exported.image;
  page::IndexBtreePageBody duplicate_root = Fetch(duplicate_tree, duplicate_tree.root_page_number);
  duplicate_root.cells.insert(duplicate_root.cells.begin() + 1, duplicate_root.cells[1]);
  ReplaceImagePage(&duplicate_image, duplicate_root);
  RequireCorruptionClass(TreeFromImage(duplicate_image),
                         page::IndexBtreePhysicalCorruptionClass::duplicate,
                         "DUPLICATE",
                         "duplicate");
}

void RebuildAndRepairSupportContracts() {
  std::vector<page::IndexBtreeCell> live;
  page::IndexBtreePhysicalTree tree = BuildRepairReportTree(&live);

  page::IndexBtreePageBody orphan;
  orphan.index_uuid = tree.index_uuid;
  orphan.page_number = tree.next_page_number++;
  orphan.page_kind = page::IndexBtreePageKind::leaf;
  orphan.cells = {Cell(9000, 0xa1, 0xb1)};
  const auto built_orphan = page::BuildIndexBtreePageBody(orphan, tree.page_size);
  Require(built_orphan.ok(), "valid orphan page build failed");
  tree.pages.push_back({orphan.page_number, built_orphan.serialized});

  const auto before_report = page::BuildIndexBtreePhysicalTreeReport(tree);
  Require(before_report.ok(), "valid orphan report failed");
  Require(before_report.report.page_count > before_report.report.reachable_page_count,
          "valid orphan report did not expose stale page accounting");

  const auto rebuilt = page::RebuildIndexBtreePhysicalTree(tree);
  Require(rebuilt.ok(), "valid tree rebuild failed");
  Require(rebuilt.rebuilt, "rebuild result did not mark rebuilt");
  Require(HasEvidence(rebuilt.evidence, "structural_rebuild=true"),
          "rebuild missing structural evidence");
  Require(HasEvidence(rebuilt.evidence, "visibility_authority=false"),
          "rebuild claimed visibility authority");
  Require(HasEvidence(rebuilt.evidence, "visibility=false"),
          "rebuild missing visibility non-authority alias");
  Require(HasEvidence(rebuilt.evidence, "authorization=false"),
          "rebuild missing authorization non-authority alias");
  Require(HasEvidence(rebuilt.evidence, "transaction_finality=false"),
          "rebuild missing finality non-authority alias");
  Require(HasEvidence(rebuilt.evidence, "recovery=false"),
          "rebuild missing recovery non-authority alias");
  Require(rebuilt.report.ok(), "rebuilt report failed");
  Require(rebuilt.report.page_count == rebuilt.report.reachable_page_count,
          "rebuilt tree retained stale orphan pages");
  Require(rebuilt.report.tuple_live_entry_estimate == live.size(),
          "rebuilt report live estimate mismatch");
  const auto rebuilt_scan = page::OrderedScanIndexBtreePhysicalTree(rebuilt.tree);
  RequireScanContracts(rebuilt_scan, "ordered", "forward");
  RequireScanMatches(rebuilt_scan, live, "rebuilt-live-locators");
  Require(rebuilt.tree.root_page_number != 0, "rebuilt root evidence missing");
  Require(rebuilt.image.root_page_number == rebuilt.tree.root_page_number,
          "rebuilt image root evidence mismatch");

  page::IndexBtreePhysicalTree stale_tree = tree;
  stale_tree.pages.back().serialized.back() ^= 0x55u;
  const auto repair = page::RepairIndexBtreePhysicalTree(stale_tree);
  Require(repair.ok(), "safe orphan/stale repair failed");
  Require(repair.repaired, "safe orphan/stale repair did not mark repaired");
  Require(HasEvidence(repair.evidence, "orphan_stale_page_images_removed=true"),
          "safe repair missing orphan removal evidence");
  Require(HasEvidence(repair.evidence, "visibility_authority=false"),
          "safe repair claimed visibility authority");
  Require(HasEvidence(repair.evidence, "visibility=false"),
          "safe repair missing visibility non-authority alias");
  Require(HasEvidence(repair.evidence, "authorization=false"),
          "safe repair missing authorization non-authority alias");
  Require(HasEvidence(repair.evidence, "transaction_finality=false"),
          "safe repair missing finality non-authority alias");
  Require(HasEvidence(repair.evidence, "recovery=false"),
          "safe repair missing recovery non-authority alias");
  Require(repair.before_report.corruption_class ==
              page::IndexBtreePhysicalCorruptionClass::orphan_stale_page_image,
          "safe repair did not classify orphan stale image");
  Require(repair.after_report.ok(), "safe repair after report failed");
  Require(repair.after_report.page_count == repair.after_report.reachable_page_count,
          "safe repair retained stale page image");
  const auto repair_scan = page::OrderedScanIndexBtreePhysicalTree(repair.tree);
  RequireScanContracts(repair_scan, "ordered", "forward");
  RequireScanMatches(repair_scan, live, "safe-repair-live-locators");

  page::IndexBtreePhysicalTree unsafe_tree = rebuilt.tree;
  for (auto& page_image : unsafe_tree.pages) {
    if (page_image.page_number == unsafe_tree.root_page_number) {
      page_image.serialized.back() ^= 0x66u;
      break;
    }
  }
  const auto unsafe = page::RepairIndexBtreePhysicalTree(unsafe_tree);
  Require(!unsafe.ok(), "unsafe reachable checksum repair unexpectedly succeeded");
  Require(unsafe.refused, "unsafe reachable checksum repair did not mark refused");
  Require(unsafe.corruption_class == page::IndexBtreePhysicalCorruptionClass::checksum,
          "unsafe repair corruption class mismatch");
  Require(unsafe.diagnostic.diagnostic_code == "SB-INDEX-BTREE-PAGE-CHECKSUM-MISMATCH",
          "unsafe repair exact diagnostic mismatch");
  Require(HasEvidence(unsafe.evidence, "repair_refused=true"),
          "unsafe repair missing refusal evidence");
  Require(HasEvidence(unsafe.evidence, "visibility_authority=false"),
          "unsafe repair claimed visibility authority");
  Require(HasEvidence(unsafe.evidence, "visibility=false"),
          "unsafe repair missing visibility non-authority alias");
  Require(HasEvidence(unsafe.evidence, "authorization=false"),
          "unsafe repair missing authorization non-authority alias");
  Require(HasEvidence(unsafe.evidence, "transaction_finality=false"),
          "unsafe repair missing finality non-authority alias");
  Require(HasEvidence(unsafe.evidence, "recovery=false"),
          "unsafe repair missing recovery non-authority alias");
  Require(HasEvidence(unsafe.evidence, "transaction_finality_authority=false"),
          "unsafe repair claimed finality authority");
  Require(HasEvidence(unsafe.evidence, "recovery_authority=false"),
          "unsafe repair claimed recovery authority");
}

void UniqueAtomicConflictPath() {
  auto init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700002200000ull, 0x50), 512);
  Require(init.ok(), "unique tree initialization failed");
  page::IndexBtreePhysicalTree tree = std::move(init.tree);

  page::IndexBtreePhysicalUniqueInsertRequest first;
  first.cell = Cell(4200, 0x51, 0x61);
  first.null_policy = page::IndexBtreePhysicalUniqueNullPolicy::nulls_not_distinct;
  const auto first_result = page::InsertUniqueIndexBtreeCell(&tree, first);
  RequireUniqueContracts(first_result, "unique-first");
  Require(first_result.inserted, "first unique insert did not insert");
  VerifyTreeOrderingAndMetadata(tree, 1);

  page::IndexBtreePhysicalUniqueInsertRequest duplicate;
  duplicate.cell = Cell(4200, 0x52, 0x62);
  duplicate.null_policy = page::IndexBtreePhysicalUniqueNullPolicy::nulls_not_distinct;
  duplicate.active_duplicate_policy =
      page::IndexBtreePhysicalUniqueActiveDuplicatePolicy::wait_for_mga;
  const auto duplicate_result = page::InsertUniqueIndexBtreeCell(&tree, duplicate);
  RequireUniqueContracts(duplicate_result, "unique-duplicate");
  Require(!duplicate_result.inserted, "duplicate unique insert unexpectedly inserted");
  Require(duplicate_result.conflict, "duplicate unique insert did not return conflict");
  Require(duplicate_result.conflict_state ==
              page::IndexBtreePhysicalUniqueConflictState::wait_for_mga,
          "duplicate unique conflict did not expose wait state");
  Require(duplicate_result.conflict_candidates.size() == 1,
          "duplicate unique conflict candidate count mismatch");
  Require(duplicate_result.conflict_candidates.front().encoded_key == first.cell.encoded_key,
          "duplicate conflict candidate key mismatch");
  Require(!duplicate_result.conflict_candidates.front().same_row,
          "different-row duplicate reported same-row conflict");
  Require(HasEvidence(duplicate_result.evidence, "unique_probe_same_encoded_key_identity=true"),
          "duplicate probe did not report key identity comparison");
  Require(HasEvidence(duplicate_result.evidence, "unique_probe_compares_row_uuid=false"),
          "duplicate probe compared row uuid as identity");
  Require(HasEvidence(duplicate_result.evidence,
                      "unique_conflict_candidates_require_mga_security_recheck=true"),
          "duplicate conflict missing MGA/security recheck evidence");
  VerifyTreeOrderingAndMetadata(tree, 1);

  auto same_row_init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700002210000ull, 0x53), 512);
  Require(same_row_init.ok(), "same-row unique tree initialization failed");
  page::IndexBtreePhysicalTree same_row_tree = std::move(same_row_init.tree);
  page::IndexBtreePhysicalUniqueInsertRequest original;
  original.cell = Cell(4300, 0x54, 0x64);
  original.null_policy = page::IndexBtreePhysicalUniqueNullPolicy::nulls_not_distinct;
  Require(page::InsertUniqueIndexBtreeCell(&same_row_tree, original).inserted,
          "same-row original insert failed");

  page::IndexBtreePhysicalUniqueInsertRequest same_row_no_proof = original;
  same_row_no_proof.cell.version_uuid =
      GeneratedUuid(platform::UuidKind::row, 1700002220000ull, 0x65);
  same_row_no_proof.active_duplicate_policy =
      page::IndexBtreePhysicalUniqueActiveDuplicatePolicy::refuse_candidate;
  const auto no_proof = page::InsertUniqueIndexBtreeCell(&same_row_tree, same_row_no_proof);
  RequireUniqueContracts(no_proof, "same-row-no-proof");
  Require(no_proof.conflict, "same-row update without proof was admitted");
  Require(!no_proof.same_row_update_allowed,
          "same-row update without proof marked allowed");
  Require(no_proof.conflict_state ==
              page::IndexBtreePhysicalUniqueConflictState::refuse_candidate,
          "same-row no-proof conflict did not refuse candidate");

  page::IndexBtreePhysicalUniqueInsertRequest same_row_with_proof = same_row_no_proof;
  same_row_with_proof.allow_same_row_update = true;
  same_row_with_proof.same_row_proof_uuid = original.cell.row_uuid;
  same_row_with_proof.cell.version_uuid =
      GeneratedUuid(platform::UuidKind::row, 1700002230000ull, 0x66);
  const auto with_proof =
      page::InsertUniqueIndexBtreeCell(&same_row_tree, same_row_with_proof);
  RequireUniqueContracts(with_proof, "same-row-with-proof");
  Require(with_proof.inserted, "same-row update with proof did not insert");
  Require(with_proof.same_row_update_allowed,
          "same-row update with proof not marked allowed");
  Require(HasEvidence(with_proof.evidence, "same_row_update_proof=true"),
          "same-row proof evidence missing");
  VerifyTreeOrderingAndMetadata(same_row_tree, 2);

  auto null_distinct_init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700002240000ull, 0x57), 512);
  Require(null_distinct_init.ok(), "nulls-distinct unique tree initialization failed");
  page::IndexBtreePhysicalTree null_distinct_tree = std::move(null_distinct_init.tree);
  page::IndexBtreePhysicalUniqueInsertRequest null_a;
  null_a.cell = NullCell(0x58, 0x68);
  null_a.incoming_key_has_null = true;
  null_a.null_policy = page::IndexBtreePhysicalUniqueNullPolicy::nulls_distinct;
  const auto null_a_result = page::InsertUniqueIndexBtreeCell(&null_distinct_tree, null_a);
  RequireUniqueContracts(null_a_result, "null-distinct-first");
  Require(null_a_result.inserted, "first nulls-distinct insert failed");
  page::IndexBtreePhysicalUniqueInsertRequest null_b = null_a;
  null_b.cell = NullCell(0x59, 0x69);
  const auto null_b_result = page::InsertUniqueIndexBtreeCell(&null_distinct_tree, null_b);
  RequireUniqueContracts(null_b_result, "null-distinct-second");
  Require(null_b_result.inserted, "second nulls-distinct insert failed");
  Require(null_b_result.null_exempt_from_conflict,
          "nulls-distinct second insert not marked conflict-exempt");
  VerifyTreeOrderingAndMetadata(null_distinct_tree, 2);

  auto null_not_distinct_init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700002250000ull, 0x5a), 512);
  Require(null_not_distinct_init.ok(), "nulls-not-distinct unique tree initialization failed");
  page::IndexBtreePhysicalTree null_not_distinct_tree = std::move(null_not_distinct_init.tree);
  page::IndexBtreePhysicalUniqueInsertRequest null_c;
  null_c.cell = NullCell(0x5b, 0x6b);
  null_c.incoming_key_has_null = true;
  null_c.null_policy = page::IndexBtreePhysicalUniqueNullPolicy::nulls_not_distinct;
  Require(page::InsertUniqueIndexBtreeCell(&null_not_distinct_tree, null_c).inserted,
          "nulls-not-distinct first insert failed");
  page::IndexBtreePhysicalUniqueInsertRequest null_d = null_c;
  null_d.cell = NullCell(0x5c, 0x6c);
  const auto null_d_result =
      page::InsertUniqueIndexBtreeCell(&null_not_distinct_tree, null_d);
  RequireUniqueContracts(null_d_result, "null-not-distinct-second");
  Require(null_d_result.conflict, "nulls-not-distinct duplicate did not conflict");
  VerifyTreeOrderingAndMetadata(null_not_distinct_tree, 1);

  auto partial_init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700002260000ull, 0x5d), 512);
  Require(partial_init.ok(), "partial unique tree initialization failed");
  page::IndexBtreePhysicalTree partial_tree = std::move(partial_init.tree);
  page::IndexBtreePhysicalUniqueInsertRequest partial_false;
  partial_false.cell = Cell(4400, 0x5e, 0x6e);
  partial_false.partial_predicate_participates = false;
  const auto bypassed = page::InsertUniqueIndexBtreeCell(&partial_tree, partial_false);
  RequireUniqueContracts(bypassed, "partial-false");
  Require(bypassed.bypassed_partial_predicate,
          "partial false request did not bypass physical insert");
  Require(!bypassed.inserted, "partial false request inserted a physical entry");
  VerifyTreeOrderingAndMetadata(partial_tree, 0);
  page::IndexBtreePhysicalUniqueInsertRequest partial_true = partial_false;
  partial_true.partial_predicate_participates = true;
  Require(page::InsertUniqueIndexBtreeCell(&partial_tree, partial_true).inserted,
          "partial true insert failed");
  page::IndexBtreePhysicalUniqueInsertRequest partial_duplicate = partial_true;
  partial_duplicate.cell = Cell(4400, 0x5f, 0x6f);
  const auto partial_conflict =
      page::InsertUniqueIndexBtreeCell(&partial_tree, partial_duplicate);
  RequireUniqueContracts(partial_conflict, "partial-true-conflict");
  Require(partial_conflict.conflict, "partial true duplicate did not conflict");
  VerifyTreeOrderingAndMetadata(partial_tree, 1);
}

void UniqueConcurrentSameKeyRace() {
  auto init = page::InitializeIndexBtreePhysicalTree(
      GeneratedUuid(platform::UuidKind::object, 1700002300000ull, 0x70), 512);
  Require(init.ok(), "unique race tree initialization failed");
  page::IndexBtreePhysicalTree tree = std::move(init.tree);

  std::atomic<int> inserted_count{0};
  std::atomic<int> conflict_count{0};
  std::mutex failure_mutex;
  std::vector<std::string> failures;
  auto record_failure = [&](std::string message) {
    std::lock_guard<std::mutex> guard(failure_mutex);
    failures.push_back(std::move(message));
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < 16; ++i) {
    threads.emplace_back([&, i]() {
      page::IndexBtreePhysicalUniqueInsertRequest request;
      request.cell = Cell(4500,
                          static_cast<platform::byte>(0x71 + i),
                          static_cast<platform::byte>(0x91 + i));
      request.null_policy = page::IndexBtreePhysicalUniqueNullPolicy::nulls_not_distinct;
      request.active_duplicate_policy =
          page::IndexBtreePhysicalUniqueActiveDuplicatePolicy::refuse_candidate;
      const auto result = page::InsertUniqueIndexBtreeCell(&tree, request);
      if (!result.ok()) {
        record_failure("unique race result failed: " + result.diagnostic.diagnostic_code);
        return;
      }
      if (!HasEvidence(result.evidence, "atomic_conflict_probe_insert_latch=structural_exclusive") ||
          !HasEvidence(result.evidence, "transaction_finality_authority=false")) {
        record_failure("unique race result missing atomic/non-authority evidence");
        return;
      }
      if (result.inserted) {
        ++inserted_count;
      } else if (result.conflict &&
                 result.conflict_state ==
                     page::IndexBtreePhysicalUniqueConflictState::refuse_candidate &&
                 !result.conflict_candidates.empty()) {
        ++conflict_count;
      } else {
        record_failure("unique race produced neither insert nor exact conflict");
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  Require(failures.empty(), failures.empty() ? "" : failures.front());
  Require(inserted_count.load() == 1, "unique race admitted more than one insert");
  Require(conflict_count.load() == 15, "unique race conflict count mismatch");
  VerifyTreeOrderingAndMetadata(tree, 1);
  const auto scan = page::PointLookupIndexBtreePhysicalTree(tree, EncodedKey(4500));
  RequireScanContracts(scan, "point", "forward");
  Require(scan.locators.size() == 1, "unique race final point lookup count mismatch");
}

}  // namespace

int main() {
  SerializeParseRoundTrip();
  DeclaredBodyTrailingBytesRefusal();
  UnsafeLegacyKeyRefusal();
  InsertSplitRootSplit();
  PhysicalScanContracts();
  DeleteTombstoneCleanupAndFreeSpaceReuse();
  DeleteLeafRebalanceAndMergeRootCollapse();
  DeleteCrossParentLeafStructuralCompactionRebuild();
  ConcurrentLatchStress();
  CrashReopenImageValidation();
  LeafRangePartitionCorruptionRefusal();
  ValidationReportSupportBundleAndCorruptionClassification();
  RebuildAndRepairSupportContracts();
  UniqueAtomicConflictPath();
  UniqueConcurrentSameKeyRace();
  return EXIT_SUCCESS;
}
