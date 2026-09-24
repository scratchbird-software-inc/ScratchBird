// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "page_selection.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace page = scratchbird::storage::page;
namespace p = scratchbird::core::platform;
static void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "binary page fence failure at " << at.line() << '\n'; std::abort(); }
}
static p::TypedUuid Id(p::UuidKind kind, unsigned n) {
  const auto id = scratchbird::tests::FixtureUuid(n, 1);
  const auto typed = scratchbird::core::uuid::MakeDurableEngineIdentityUuid(kind, id);
  Check(typed.ok()); return typed.value;
}
int main() {
  page::PageSelectionLedger ledger;
  page::InsertPageCandidate candidate;
  candidate.database_uuid = Id(p::UuidKind::database, 1);
  candidate.filespace_uuid = Id(p::UuidKind::filespace, 2);
  candidate.object_uuid = Id(p::UuidKind::object, 3);
  candidate.page_uuid = Id(p::UuidKind::page, 4);
  candidate.page_uuid.value.bytes[9] = '\n';
  candidate.page_uuid.value.bytes[10] = ':';
  candidate.page_uuid.value.bytes[11] = 0;
  candidate.page_family = "data"; candidate.page_number = 4;
  candidate.page_generation = 7; candidate.free_bytes = 1024;
  Check(page::RegisterInsertPageCandidate(&ledger, candidate).ok());
  page::InsertPageSelectionRequest request;
  request.database_uuid = candidate.database_uuid; request.object_uuid = candidate.object_uuid;
  request.transaction_uuid = Id(p::UuidKind::transaction, 5);
  request.local_transaction_id = 8; request.page_family = "data"; request.encoded_row_bytes = 32;
  const auto selected = page::SelectInsertTargetPage(&ledger, nullptr, request);
  Check(selected.ok());
  const auto& fence = selected.selection.selection_fence;
  Check(fence.size() == 40 && fence.starts_with("SBPGF002"));
  Check(fence.substr(8,16) == std::string(reinterpret_cast<const char*>(candidate.page_uuid.value.bytes.data()),16));
  const auto second = page::SelectInsertTargetPage(&ledger, nullptr, request);
  Check(second.ok() && second.selection.selection_fence != fence);
  page::InsertPageAppendRequest append;
  append.selection_fence = fence; append.encoded_row_bytes = 32;
  append.selection_fence.back() ^= 0x40;
  Check(!page::AppendRowToSelectedPage(&ledger, append).ok());
  Check(ledger.candidates.front().free_bytes == 1024);
  append.selection_fence = fence;
  Check(page::AppendRowToSelectedPage(&ledger, append).ok());
  Check(ledger.candidates.front().free_bytes == 992);
  Check(!page::AppendRowToSelectedPage(&ledger, append).ok());
  Check(ledger.candidates.front().free_bytes == 992);
  return EXIT_SUCCESS;
}
