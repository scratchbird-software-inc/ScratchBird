// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_ordered_access.hpp"
#include "index_posting.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid TestUuid(platform::UuidKind kind, unsigned char salt) {
  platform::TypedUuid uuid;
  uuid.kind = kind;
  uuid.value.bytes[0] = 0x01;
  uuid.value.bytes[1] = 0x9f;
  uuid.value.bytes[6] = 0x70;
  uuid.value.bytes[8] = 0x80;
  uuid.value.bytes[14] = 0x53;
  uuid.value.bytes[15] = salt;
  return uuid;
}

std::vector<platform::byte> KeyImage(unsigned char salt = 0x41) {
  std::vector<platform::byte> key;
  key.reserve(96);
  key.push_back('S');
  key.push_back('B');
  key.push_back('K');
  key.push_back('1');
  for (unsigned i = 0; i < 92; ++i) {
    key.push_back(static_cast<platform::byte>(salt + (i % 7)));
  }
  return key;
}

void Append32(std::vector<platform::byte>* out, platform::u32 value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<platform::byte>((value >> shift) & 0xffu));
  }
}

void Append64(std::vector<platform::byte>* out, platform::u64 value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<platform::byte>((value >> shift) & 0xffu));
  }
}

void AppendUuid(std::vector<platform::byte>* out,
                const platform::TypedUuid& uuid) {
  out->push_back(static_cast<platform::byte>(uuid.kind));
  out->insert(out->end(), uuid.value.bytes.begin(), uuid.value.bytes.end());
}

idx::IndexPostingEntry Entry(unsigned char salt,
                             platform::u64 visible_from = 10) {
  idx::IndexPostingEntry entry;
  entry.locator.table_uuid = TestUuid(platform::UuidKind::object, 0x10);
  entry.locator.row_uuid = TestUuid(platform::UuidKind::row, salt);
  entry.locator.version_uuid =
      TestUuid(platform::UuidKind::row, static_cast<unsigned char>(salt + 0x20));
  entry.locator.local_transaction_id = visible_from;
  entry.visible_from_transaction_id = visible_from;
  entry.flags = static_cast<platform::u32>(
      idx::IndexPostingFlag::requires_recheck);
  return entry;
}

idx::IndexPostingList EmptyPostingList() {
  idx::IndexPostingList list;
  list.index_uuid = TestUuid(platform::UuidKind::object, 0x01);
  list.encoded_key = KeyImage();
  return list;
}

std::vector<platform::byte> LegacySerializedPostingList(
    const idx::IndexPostingList& list) {
  std::vector<platform::byte> serialized = {'S', 'B', 'P', 'L'};
  AppendUuid(&serialized, list.index_uuid);
  Append32(&serialized, static_cast<platform::u32>(list.encoded_key.size()));
  serialized.insert(serialized.end(), list.encoded_key.begin(), list.encoded_key.end());
  Append32(&serialized, static_cast<platform::u32>(list.entries.size()));
  for (const auto& entry : list.entries) {
    AppendUuid(&serialized, entry.locator.table_uuid);
    AppendUuid(&serialized, entry.locator.row_uuid);
    AppendUuid(&serialized, entry.locator.version_uuid);
    Append64(&serialized, entry.locator.local_transaction_id);
    Append64(&serialized, entry.visible_from_transaction_id);
    Append64(&serialized, entry.visible_until_transaction_id);
    Append32(&serialized, entry.flags);
  }
  return serialized;
}

idx::OrderedDuplicateLifecycleRequest Request(idx::IndexPostingList list,
                                              idx::IndexPostingEntry incoming) {
  idx::OrderedDuplicateLifecycleRequest request;
  request.posting_list = std::move(list);
  request.incoming = std::move(incoming);
  request.family = idx::IndexFamily::btree;
  request.exact_index = true;
  request.uniqueness_mode = idx::OrderedUniquenessMode::non_unique;
  request.semantic_profile.profile_id = "odf_053_bytewise_exact";
  request.semantic_profile.bytewise_stable = true;
  request.semantic_profile.requires_recheck = true;
  request.equality_image_proof_present = true;
  request.stable_row_uuid_locators = true;
  request.preserve_mga_visibility_recheck = true;
  return request;
}

bool EvidenceHas(const std::vector<idx::IndexPostingEvidenceField>& evidence,
                 std::string_view name,
                 std::string_view value) {
  for (const auto& field : evidence) {
    if (field.name == name && field.value == value) {
      return true;
    }
  }
  return false;
}

void RequireNoRuntimeDocTokens(
    const idx::OrderedDuplicateLifecycleDecision& decision) {
  std::vector<std::string> values = {
      decision.diagnostic.diagnostic_code,
      decision.diagnostic.message_key,
      decision.diagnostic.source_component,
      decision.diagnostic.remediation_hint};
  for (const auto& argument : decision.diagnostic.arguments) {
    values.push_back(argument.key);
    values.push_back(argument.value);
  }
  for (const auto& field : decision.evidence) {
    values.push_back(field.name);
    values.push_back(field.value);
  }
  for (const auto& step : decision.steps) {
    values.push_back(step);
  }
  for (const auto& value : values) {
    for (const auto forbidden : {"docs/", "execution-plans", "findings",
                                 "contracts", "references"}) {
      Require(value.find(forbidden) == std::string::npos,
              "ODF-053 runtime evidence leaked documentation token");
    }
  }
}

idx::OrderedDuplicateLifecycleDecision BuildTwoDuplicatePostingList() {
  auto first = idx::DecideOrderedDuplicateLifecycle(
      Request(EmptyPostingList(), Entry(0x31, 11)));
  Require(first.ok(), "ODF-053 initial duplicate posting create failed");
  Require(first.action ==
              idx::OrderedDuplicateLifecycleAction::create_posting_list,
          "ODF-053 initial insert did not create posting list");

  auto second_request =
      Request(first.posting_result.posting_list, Entry(0x32, 12));
  auto second = idx::DecideOrderedDuplicateLifecycle(second_request);
  Require(second.ok(), "ODF-053 duplicate posting append failed");
  Require(second.action == idx::OrderedDuplicateLifecycleAction::append_duplicate,
          "ODF-053 duplicate insert selected wrong action");
  Require(second.posting_result.posting_list.compressed_duplicates,
          "ODF-053 duplicate posting list was not marked compressed");
  Require(second.posting_result.posting_list.entries.size() == 2,
          "ODF-053 duplicate posting list entry count mismatch");
  Require(second.compression_counters.compressed_key_count == 1 &&
              second.compression_counters.posting_entry_count == 2,
          "ODF-053 duplicate compression counters missing");
  Require(second.compression_counters.bytes_saved > 0,
          "ODF-053 duplicate compression did not report saved bytes");
  Require(EvidenceHas(second.evidence, "equality_proof_accepted", "true") &&
              EvidenceHas(second.evidence, "non_unique_exact_mode", "true") &&
              EvidenceHas(second.evidence, "recheck_required", "true") &&
              EvidenceHas(second.evidence, "selected_lifecycle_action",
                          "append_duplicate"),
          "ODF-053 duplicate compression evidence missing");
  return second;
}

void DuplicateInsertCompressionAndRoundTripAreStable() {
  const auto duplicate = BuildTwoDuplicatePostingList();
  const auto parsed =
      idx::ParseIndexPostingList(duplicate.posting_result.serialized);
  Require(parsed.ok(), "ODF-053 posting parse round trip failed");
  Require(parsed.posting_list.entries.size() == 2,
          "ODF-053 parsed posting entry count mismatch");
  const auto rebuilt = idx::BuildIndexPostingList(parsed.posting_list);
  Require(rebuilt.ok(), "ODF-053 parsed posting rebuild failed");
  Require(rebuilt.serialized == duplicate.posting_result.serialized,
          "ODF-053 posting serialization was not round-trip stable");
}

void DeleteMarkDeadAndPurgeUpdatePostingList() {
  auto duplicate = BuildTwoDuplicatePostingList();
  auto request =
      Request(duplicate.posting_result.posting_list,
              duplicate.posting_result.posting_list.entries.front());
  request.incoming.visible_until_transaction_id = 20;
  request.insert = false;
  request.delete_existing = true;
  auto deleted = idx::DecideOrderedDuplicateLifecycle(request);
  Require(deleted.ok(), "ODF-053 posting delete marker failed");
  Require(deleted.action == idx::OrderedDuplicateLifecycleAction::mark_dead,
          "ODF-053 posting delete selected wrong action");
  Require((deleted.posting_result.posting_list.entries.front().flags &
           static_cast<platform::u32>(idx::IndexPostingFlag::deleted)) != 0,
          "ODF-053 posting delete flag missing");

  auto purge = Request(deleted.posting_result.posting_list, Entry(0x99, 99));
  purge.insert = false;
  purge.purge_dead = true;
  purge.oldest_active_transaction_id = 30;
  const auto purged = idx::DecideOrderedDuplicateLifecycle(purge);
  Require(purged.ok(), "ODF-053 posting purge failed");
  Require(purged.action == idx::OrderedDuplicateLifecycleAction::purge_dead &&
              purged.posting_result.posting_list.entries.size() == 1,
          "ODF-053 posting purge retained wrong entries");
}

void UniqueVisibleDuplicateRefusalIsUnchanged() {
  auto list = EmptyPostingList();
  list.entries.push_back(Entry(0x41, 41));
  auto request = Request(list, Entry(0x42, 42));
  request.family = idx::IndexFamily::unique_btree;
  request.uniqueness_mode = idx::OrderedUniquenessMode::unique_immediate;
  const auto decision = idx::DecideOrderedDuplicateLifecycle(request);
  Require(!decision.ok(), "ODF-053 unique duplicate was accepted");
  Require(decision.action == idx::OrderedDuplicateLifecycleAction::refuse_duplicate,
          "ODF-053 unique duplicate selected compression action");
  Require(decision.diagnostic.diagnostic_code ==
              "SB-INDEX-ORDERED-DUPLICATE-UNIQUE-CONFLICT",
          "ODF-053 unique duplicate diagnostic changed");
}

void EqualityProofRefusalFailsClosed() {
  auto request = Request(EmptyPostingList(), Entry(0x51, 51));
  request.equality_image_proof_present = false;
  const auto decision = idx::DecideOrderedDuplicateLifecycle(request);
  Require(!decision.ok(), "ODF-053 missing equality proof was accepted");
  Require(decision.action == idx::OrderedDuplicateLifecycleAction::refuse_duplicate,
          "ODF-053 missing proof did not refuse compression");
  Require(EvidenceHas(decision.evidence, "equality_proof_refused", "true"),
          "ODF-053 missing proof refusal evidence absent");
}

void InvalidLocatorRefusalFailsClosed() {
  auto incoming = Entry(0x61, 61);
  incoming.locator.row_uuid.kind = platform::UuidKind::object;
  auto request = Request(EmptyPostingList(), incoming);
  const auto decision = idx::DecideOrderedDuplicateLifecycle(request);
  Require(!decision.ok(), "ODF-053 invalid row locator was accepted");
  Require(decision.diagnostic.diagnostic_code ==
              "SB-INDEX-POSTING-LOCATOR-INVALID",
          "ODF-053 invalid row locator diagnostic mismatch");
}

idx::IndexPostingList DirectCompressedList(unsigned char first,
                                           unsigned char second) {
  auto list = EmptyPostingList();
  list.compressed_duplicates = true;
  list.recheck_required = true;
  list.equality_proof.proof_present = true;
  list.equality_proof.non_unique_exact = true;
  list.equality_proof.encoded_key_bytewise_stable = true;
  list.equality_proof.stable_row_uuid_locators = true;
  list.equality_proof.preserves_mga_visibility_recheck = true;
  list.entries.push_back(Entry(first, first));
  list.entries.push_back(Entry(second, second));
  return list;
}

void DeterministicSerializationOrderDoesNotDependOnInputOrder() {
  const auto forward = idx::BuildIndexPostingList(DirectCompressedList(0x71, 0x72));
  const auto reverse = idx::BuildIndexPostingList(DirectCompressedList(0x72, 0x71));
  Require(forward.ok() && reverse.ok(),
          "ODF-053 deterministic posting setup failed");
  Require(forward.serialized == reverse.serialized,
          "ODF-053 deterministic serialization drifted by input order");
  Require(forward.posting_list.entries.front().locator.row_uuid.value.bytes[15] ==
              0x71,
          "ODF-053 deterministic posting order was not stable");
}

void ParseValidationReportsExactDiagnostics() {
  const auto good = idx::BuildIndexPostingList(DirectCompressedList(0x81, 0x82));
  Require(good.ok(), "ODF-053 parse validation setup failed");

  const auto bad_envelope =
      idx::ParseIndexPostingList(std::vector<platform::byte>{'n', 'o'});
  Require(!bad_envelope.ok() &&
              bad_envelope.diagnostic.diagnostic_code ==
                  "SB-INDEX-POSTING-BAD-ENVELOPE",
          "ODF-053 bad envelope diagnostic mismatch");

  auto truncated = good.serialized;
  truncated.pop_back();
  const auto truncated_result = idx::ParseIndexPostingList(truncated);
  Require(!truncated_result.ok() &&
              truncated_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-POSTING-ENTRY-TRUNCATED",
          "ODF-053 truncated entry diagnostic mismatch");

  auto proof_mismatch = good.serialized;
  proof_mismatch[9] ^= 0x01;
  const auto proof_result = idx::ParseIndexPostingList(proof_mismatch);
  Require(!proof_result.ok() &&
              proof_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-POSTING-EQUALITY-PROOF-MISMATCH",
          "ODF-053 equality proof mismatch diagnostic mismatch");

  auto invalid_locator = good.serialized;
  const std::size_t first_row_uuid_kind_offset =
      4 + 4 + 4 + 17 + 4 + good.posting_list.encoded_key.size() + 4 + 17;
  invalid_locator[first_row_uuid_kind_offset] =
      static_cast<platform::byte>(platform::UuidKind::object);
  const auto invalid_locator_result =
      idx::ParseIndexPostingList(invalid_locator);
  Require(!invalid_locator_result.ok() &&
              invalid_locator_result.diagnostic.diagnostic_code ==
                  "SB-INDEX-POSTING-LOCATOR-INVALID",
          "ODF-053 invalid locator parse diagnostic mismatch");
}

void LegacyUnversionedPostingListParsesAsUncompressed() {
  auto list = EmptyPostingList();
  list.entries.push_back(Entry(0x91, 91));
  const auto parsed = idx::ParseIndexPostingList(
      LegacySerializedPostingList(list));
  Require(parsed.ok(), "ODF-053 legacy posting parse failed");
  Require(!parsed.posting_list.compressed_duplicates,
          "ODF-053 legacy posting was marked compressed");
  Require(parsed.posting_list.entries.size() == 1,
          "ODF-053 legacy posting entry count mismatch");
  Require(parsed.counters.compressed_key_count == 0,
          "ODF-053 legacy posting compression counter mismatch");
  const auto upgraded = idx::BuildIndexPostingList(parsed.posting_list);
  Require(upgraded.ok(), "ODF-053 legacy posting rebuild failed");
}

void RuntimeEvidenceHasNoDocumentationTokens() {
  const auto decision = BuildTwoDuplicatePostingList();
  RequireNoRuntimeDocTokens(decision);
}

}  // namespace

int main() {
  DuplicateInsertCompressionAndRoundTripAreStable();
  DeleteMarkDeadAndPurgeUpdatePostingList();
  UniqueVisibleDuplicateRefusalIsUnchanged();
  EqualityProofRefusalFailsClosed();
  InvalidLocatorRefusalFailsClosed();
  DeterministicSerializationOrderDoesNotDependOnInputOrder();
  ParseValidationReportsExactDiagnostics();
  LegacyUnversionedPostingListParsesAsUncompressed();
  RuntimeEvidenceHasNoDocumentationTokens();
  return 0;
}
