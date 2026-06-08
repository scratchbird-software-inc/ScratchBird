// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SB-INDEX-BTREE-PAGE-PROBE-ANCHOR
#include "datatype_binary.hpp"
#include "index_btree_page.hpp"
#include "page_manager.hpp"
#include "page_skeleton.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using scratchbird::core::datatypes::CanonicalTypeId;
using scratchbird::core::datatypes::DatatypeBinaryValue;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;
using scratchbird::core::uuid::GenerateEngineIdentityV7;
using scratchbird::storage::disk::PageType;
using scratchbird::storage::page::BuildIndexBtreePageBody;
using scratchbird::storage::page::BuildManagedPageHeader;
using scratchbird::storage::page::ClassifyPageSkeleton;
using scratchbird::storage::page::IndexBtreeCell;
using scratchbird::storage::page::IndexBtreePageBody;
using scratchbird::storage::page::IndexBtreePageKind;
using scratchbird::storage::page::LookupPageSkeleton;
using scratchbird::storage::page::ManagedPageHeaderRequest;
using scratchbird::storage::page::PageManagerContext;
using scratchbird::storage::page::PageSkeletonKind;
using scratchbird::storage::page::ParseIndexBtreePageBody;

namespace {

TypedUuid Id(UuidKind kind, scratchbird::core::platform::u64 salt) {
  const auto generated = GenerateEngineIdentityV7(kind, salt);
  return generated.ok() ? generated.value : TypedUuid{};
}

DatatypeBinaryValue Int32Value(scratchbird::core::platform::u32 value) {
  DatatypeBinaryValue typed;
  typed.type_id = CanonicalTypeId::int32;
  typed.payload.resize(4);
  StoreLittle32(typed.payload.data(), value);
  return typed;
}

void Check(bool condition, const std::string& label) {
  if (!condition) {
    std::cerr << "FAIL " << label << '\n';
    std::exit(1);
  }
  std::cout << "PASS " << label << '\n';
}

}  // namespace

int main() {
  constexpr scratchbird::core::platform::u32 page_size = 16 * 1024;
  const auto database_uuid = Id(UuidKind::database, 101);
  const auto filespace_uuid = Id(UuidKind::filespace, 102);
  const auto page_uuid = Id(UuidKind::page, 103);
  const auto index_uuid = Id(UuidKind::object, 104);
  const auto row_uuid = Id(UuidKind::row, 105);

  const auto skeleton = LookupPageSkeleton(PageType::index_btree);
  Check(skeleton.ok() &&
            skeleton.descriptor.skeleton_kind == PageSkeletonKind::index_btree &&
            skeleton.descriptor.body_parser_available &&
            skeleton.descriptor.body_mutation_available,
        "NCE-0603 index skeleton is concrete");

  PageManagerContext context;
  context.page_size = page_size;
  context.database_uuid = database_uuid;
  context.filespace_uuid = filespace_uuid;
  ManagedPageHeaderRequest header_request;
  header_request.context = context;
  header_request.page_type = PageType::index_btree;
  header_request.page_uuid = page_uuid;
  header_request.page_number = 7;
  header_request.page_generation = 1;
  const auto header = BuildManagedPageHeader(header_request);
  Check(header.ok() &&
            header.classification.descriptor.skeleton_kind == PageSkeletonKind::index_btree &&
            header.classification.may_interpret_body &&
            header.classification.may_mutate_body,
        "NCE-0603 managed page header admits index body");

  IndexBtreePageBody body;
  body.index_uuid = index_uuid;
  body.page_number = 7;
  body.page_kind = IndexBtreePageKind::leaf;
  body.tree_level = 0;
  body.left_sibling_page_number = 6;
  body.right_sibling_page_number = 8;
  IndexBtreeCell cell;
  cell.key_ordinal = 1;
  cell.key_value = Int32Value(42);
  cell.row_uuid = row_uuid;
  body.cells.push_back(cell);

  const auto built = BuildIndexBtreePageBody(body, page_size);
  Check(built.ok() && !built.serialized.empty(),
        "NCE-0603 index page builds");
  const auto parsed = ParseIndexBtreePageBody(built.serialized, body.page_number);
  Check(parsed.ok() &&
            parsed.body.index_uuid.value == index_uuid.value &&
            parsed.body.page_kind == IndexBtreePageKind::leaf &&
            parsed.body.cells.size() == 1 &&
            parsed.body.cells.front().row_uuid.value == row_uuid.value &&
            parsed.body.cells.front().key_value.type_id == CanonicalTypeId::int32,
        "NCE-0603 index page round-trips");

  std::vector<byte> corrupted = built.serialized;
  corrupted.back() ^= 0x7fu;
  Check(!ParseIndexBtreePageBody(corrupted, body.page_number).ok(),
        "NCE-0603 index page checksum rejects corruption");

  IndexBtreePageBody bad_kind = body;
  bad_kind.page_kind = IndexBtreePageKind::unknown;
  Check(!BuildIndexBtreePageBody(bad_kind, page_size).ok(),
        "NCE-0603 index page rejects unknown kind");

  IndexBtreePageBody bad_row_uuid = body;
  bad_row_uuid.cells.front().row_uuid = Id(UuidKind::object, 106);
  Check(!BuildIndexBtreePageBody(bad_row_uuid, page_size).ok(),
        "NCE-0603 index page rejects non-row UUID cell target");

  return 0;
}
