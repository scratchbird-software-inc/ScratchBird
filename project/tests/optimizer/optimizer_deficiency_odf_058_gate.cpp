// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mga_visibility_status_cache_evidence.hpp"
#include "page_finality_evidence.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"
#include "visibility_status_cache.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace mga = scratchbird::transaction::mga;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "ODF-058 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

mga::VisibilityStatusCacheFacts Facts() {
  mga::VisibilityStatusCacheFacts facts;
  facts.cache_generation = 10;
  facts.invalidation_generation = 20;
  facts.horizon_epoch = 30;
  facts.snapshot_epoch = 40;
  facts.relation_epoch = 50;
  facts.catalog_epoch = 60;
  facts.reader_visible_through_local_transaction_id = mga::MakeLocalTransactionId(100);
  facts.oldest_active_local_transaction_id = mga::MakeLocalTransactionId(110);
  facts.oldest_snapshot_local_transaction_id = mga::MakeLocalTransactionId(120);
  facts.transaction_inventory_authoritative = true;
  facts.transaction_horizon_authoritative = true;
  facts.normal_mga_visibility_authority_available = true;
  return facts;
}

mga::LocalTransactionInventory CommittedInventory(platform::u64 committed_count) {
  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  for (platform::u64 i = 1; i <= committed_count; ++i) {
    const auto begin = mga::BeginLocalTransaction(
        inventory, NewUuid(platform::UuidKind::transaction, 58000 + i), NowMillis());
    Require(begin.ok(), "ODF-058 begin transaction failed");
    inventory = begin.inventory;
    const auto commit = mga::CommitLocalTransaction(
        inventory, mga::MakeLocalTransactionId(i), NowMillis() + i);
    Require(commit.ok(), "ODF-058 commit transaction failed");
    inventory = commit.inventory;
  }
  return inventory;
}

mga::TransactionStatusRangeCacheRequest RangeRequest(
    platform::u64 first,
    platform::u64 last,
    mga::VisibilityStatusCacheFacts facts = Facts()) {
  mga::TransactionStatusRangeCacheRequest request;
  request.first_local_transaction_id = mga::MakeLocalTransactionId(first);
  request.last_local_transaction_id = mga::MakeLocalTransactionId(last);
  request.facts = facts;
  return request;
}

struct PageFixture {
  std::string relation_uuid = NewUuidText(platform::UuidKind::object, 58100);
  platform::u64 page_number = 42;
  platform::u64 page_generation = 7;
  platform::u64 extent_id = 3;
  platform::u64 extent_epoch = 11;
};

mga::PageFinalityEvidenceDecision AcceptedPageFinality(const PageFixture& fixture) {
  mga::PageFinalityMapEntry entry;
  entry.scope = mga::PageFinalityScope::page;
  entry.status = mga::PageFinalityMapStatus::current;
  entry.provenance = mga::PageFinalityProvenance::engine_mga_transaction_inventory;
  entry.relation_uuid = fixture.relation_uuid;
  entry.page_number = fixture.page_number;
  entry.page_generation = fixture.page_generation;
  entry.extent_id = fixture.extent_id;
  entry.extent_epoch = fixture.extent_epoch;
  entry.relation_epoch = Facts().relation_epoch;
  entry.catalog_epoch = Facts().catalog_epoch;
  entry.final_through_local_transaction_id = mga::MakeLocalTransactionId(90);
  entry.map_generation = 5;
  entry.persisted_record_present = true;
  entry.checksum_valid = true;
  entry.all_visible = true;
  entry.all_final = false;

  mga::PageFinalityObservedFacts observed;
  observed.requested_scope = mga::PageFinalityScope::page;
  observed.relation_uuid = fixture.relation_uuid;
  observed.page_number = fixture.page_number;
  observed.page_generation = fixture.page_generation;
  observed.extent_id = fixture.extent_id;
  observed.extent_epoch = fixture.extent_epoch;
  observed.relation_epoch = Facts().relation_epoch;
  observed.catalog_epoch = Facts().catalog_epoch;
  observed.reader_visible_through_local_transaction_id =
      mga::MakeLocalTransactionId(100);
  observed.oldest_active_local_transaction_id = mga::MakeLocalTransactionId(110);
  observed.transaction_horizon_authoritative = true;
  observed.transaction_inventory_authoritative = true;
  observed.normal_mga_visibility_authority_available = true;
  return mga::EvaluatePageFinalityEvidence(
      entry, observed, mga::PageFinalityConsumer::index_only_scan);
}

mga::PageVisibilityStatusCacheRequest PageRequest(
    const PageFixture& fixture,
    mga::VisibilityStatusCacheFacts facts = Facts()) {
  mga::PageVisibilityStatusCacheRequest request;
  request.relation_uuid = fixture.relation_uuid;
  request.page_number = fixture.page_number;
  request.page_generation = fixture.page_generation;
  request.extent_id = fixture.extent_id;
  request.extent_epoch = fixture.extent_epoch;
  request.final_through_local_transaction_id = mga::MakeLocalTransactionId(90);
  request.facts = facts;
  return request;
}

mga::RelationNoOlderReaderCacheRequest RelationRequest(
    const std::string& relation_uuid,
    platform::u64 threshold,
    mga::VisibilityStatusCacheFacts facts = Facts()) {
  mga::RelationNoOlderReaderCacheRequest request;
  request.relation_uuid = relation_uuid;
  request.no_reader_older_than_local_transaction_id =
      mga::MakeLocalTransactionId(threshold);
  request.facts = facts;
  return request;
}

bool HasDiagnostic(const opt::OptimizerMgaVisibilityStatusCacheEvidence& evidence,
                   std::string_view token) {
  for (const auto& diagnostic : evidence.diagnostics) {
    if (diagnostic.find(token) != std::string::npos) { return true; }
  }
  return false;
}

void RequireNoRuntimeDocTokens(
    const opt::OptimizerMgaVisibilityStatusCacheEvidence& evidence) {
  for (const auto& diagnostic : evidence.diagnostics) {
    for (const auto forbidden : {"docs/", "execution-plans", "findings",
                                 "contracts", "references"}) {
      Require(diagnostic.find(forbidden) == std::string::npos,
              "ODF-058 runtime evidence leaked documentation token");
    }
  }
}

void TxidRangeCacheAcceleratesOnlyInventoryCommittedRanges() {
  auto cache = mga::MakeMgaVisibilityStatusCache(10, 20);
  const auto inventory = CommittedInventory(4);

  const auto cached = mga::CacheTransactionStatusRangeFromInventory(
      &cache, inventory, RangeRequest(1, 4));
  Require(cached.accepted, "ODF-058 committed txid range was not cached");
  Require(cached.all_committed,
          "ODF-058 committed txid range did not report all-committed");
  Require(!cached.cache_is_transaction_finality_authority,
          "ODF-058 txid cache became finality authority");
  Require(cached.durable_mga_inventory_remains_authority,
          "ODF-058 durable inventory authority flag missing");

  const auto accelerated = mga::EvaluateCachedTransactionStatusRange(
      &cache, RangeRequest(2, 3));
  Require(accelerated.accepted,
          "ODF-058 committed txid range cache did not accelerate subset");
  Require(!accelerated.authoritative_path_required,
          "ODF-058 accepted txid cache still required authoritative path");

  const auto optimizer_evidence =
      opt::BuildOptimizerMgaVisibilityStatusCacheEvidence(accelerated);
  Require(optimizer_evidence.accepted,
          "ODF-058 optimizer evidence did not preserve txid acceptance");
  Require(optimizer_evidence.all_committed,
          "ODF-058 optimizer evidence lost all-committed flag");
  Require(!optimizer_evidence.cache_transaction_finality_authority,
          "ODF-058 optimizer evidence made cache authoritative");
  Require(HasDiagnostic(optimizer_evidence,
                        "authority_source=durable_mga_transaction_inventory"),
          "ODF-058 optimizer authority-source evidence missing");
  RequireNoRuntimeDocTokens(optimizer_evidence);
}

void MissingStaleAndAmbiguousTxidCacheFailClosed() {
  auto cache = mga::MakeMgaVisibilityStatusCache(10, 20);
  const auto inventory = CommittedInventory(2);
  (void)mga::CacheTransactionStatusRangeFromInventory(
      &cache, inventory, RangeRequest(1, 2));

  auto stale_facts = Facts();
  stale_facts.invalidation_generation = 21;
  const auto stale = mga::EvaluateCachedTransactionStatusRange(
      &cache, RangeRequest(1, 2, stale_facts));
  Require(!stale.accepted,
          "ODF-058 stale invalidation generation was accepted");
  Require(stale.authoritative_path_required,
          "ODF-058 stale cache did not fail to authoritative path");
  Require(stale.refusal_reason ==
              "visibility_status_cache_generation_mismatch",
          "ODF-058 stale refusal reason mismatch");

  auto uncertain_facts = Facts();
  uncertain_facts.transaction_inventory_authoritative = false;
  const auto uncertain = mga::EvaluateCachedTransactionStatusRange(
      &cache, RangeRequest(1, 2, uncertain_facts));
  Require(!uncertain.accepted,
          "ODF-058 uncertain inventory authority was accepted");
  Require(uncertain.refusal_reason == "mga_authority_inputs_uncertain",
          "ODF-058 uncertain authority refusal reason mismatch");

  auto mixed_inventory = CommittedInventory(1);
  const auto begin = mga::BeginLocalTransaction(
      mixed_inventory, NewUuid(platform::UuidKind::transaction, 58250),
      NowMillis());
  Require(begin.ok(), "ODF-058 active setup transaction failed");
  mixed_inventory = begin.inventory;
  const auto mixed = mga::CacheTransactionStatusRangeFromInventory(
      &cache, mixed_inventory, RangeRequest(1, 2));
  Require(!mixed.accepted,
          "ODF-058 active transaction was cached as committed");
  Require(mixed.refusal_reason ==
              "txid_range_not_all_committed_by_inventory",
          "ODF-058 mixed transaction refusal reason mismatch");
}

void CacheContainerInvalidationGenerationFailsClosed() {
  auto cache = mga::MakeMgaVisibilityStatusCache(10, 20);
  const auto inventory = CommittedInventory(2);
  const auto cached = mga::CacheTransactionStatusRangeFromInventory(
      &cache, inventory, RangeRequest(1, 2));
  Require(cached.accepted,
          "ODF-058 setup range was not cached before invalidation");

  cache.invalidation_generation = 21;
  const auto invalidated = mga::EvaluateCachedTransactionStatusRange(
      &cache, RangeRequest(1, 2));
  Require(!invalidated.accepted,
          "ODF-058 cache-container invalidation generation was accepted");
  Require(invalidated.authoritative_path_required,
          "ODF-058 cache-container invalidation did not require authority path");
  Require(invalidated.refusal_reason ==
              "visibility_status_cache_generation_mismatch",
          "ODF-058 cache-container invalidation refusal reason mismatch");

  const auto optimizer_evidence =
      opt::BuildOptimizerMgaVisibilityStatusCacheEvidence(invalidated);
  Require(HasDiagnostic(optimizer_evidence,
                        "cache_container_invalidation_generation=21"),
          "ODF-058 optimizer evidence omitted container invalidation generation");
  Require(!optimizer_evidence.cache_transaction_finality_authority,
          "ODF-058 invalidated cache became finality authority");
  RequireNoRuntimeDocTokens(optimizer_evidence);
}

void PageVisibilityAndAllCommittedCacheAcceleratesOnlyWithPageEvidence() {
  auto cache = mga::MakeMgaVisibilityStatusCache(10, 20);
  const PageFixture fixture;
  const auto page_finality = AcceptedPageFinality(fixture);
  Require(page_finality.accepted, "ODF-058 page finality setup refused");

  const auto cached = mga::CachePageVisibilityStatus(
      &cache, PageRequest(fixture), page_finality, true);
  Require(cached.accepted, "ODF-058 page visibility cache insert refused");

  const auto all_visible = mga::EvaluateCachedPageVisibilityStatus(
      &cache, PageRequest(fixture), mga::VisibilityStatusCacheProbe::page_all_visible);
  Require(all_visible.accepted,
          "ODF-058 page all-visible cache did not accelerate");
  Require(all_visible.all_visible,
          "ODF-058 page all-visible cache lost all-visible flag");

  const auto all_committed = mga::EvaluateCachedPageVisibilityStatus(
      &cache, PageRequest(fixture),
      mga::VisibilityStatusCacheProbe::page_all_committed);
  Require(all_committed.accepted,
          "ODF-058 page all-committed cache did not accelerate");
  Require(all_committed.all_committed,
          "ODF-058 page all-committed cache lost all-committed flag");

  auto visible_only_cache = mga::MakeMgaVisibilityStatusCache(10, 20);
  const auto visible_only_cached = mga::CachePageVisibilityStatus(
      &visible_only_cache, PageRequest(fixture), page_finality, false);
  Require(visible_only_cached.accepted,
          "ODF-058 visible-only page cache insert refused");
  const auto visible_only_committed = mga::EvaluateCachedPageVisibilityStatus(
      &visible_only_cache, PageRequest(fixture),
      mga::VisibilityStatusCacheProbe::page_all_committed);
  Require(!visible_only_committed.accepted,
          "ODF-058 visible-only page cache satisfied all-committed");
  Require(visible_only_committed.refusal_reason ==
              "page_all_committed_cache_evidence_missing",
          "ODF-058 visible-only all-committed refusal reason mismatch");

  auto epoch_facts = Facts();
  ++epoch_facts.relation_epoch;
  const auto epoch_mismatch = mga::EvaluateCachedPageVisibilityStatus(
      &cache, PageRequest(fixture, epoch_facts),
      mga::VisibilityStatusCacheProbe::page_all_visible);
  Require(!epoch_mismatch.accepted,
          "ODF-058 page relation epoch mismatch was accepted");
  Require(epoch_mismatch.refusal_reason == "page_visibility_epoch_mismatch",
          "ODF-058 page epoch refusal reason mismatch");

  auto external_cache = mga::MakeMgaVisibilityStatusCache(10, 20);
  mga::PageVisibilityStatusCacheEntry external_entry;
  external_entry.status = mga::VisibilityStatusCacheEntryStatus::current;
  external_entry.provenance = mga::PageFinalityProvenance::parser_claim;
  external_entry.relation_uuid = fixture.relation_uuid;
  external_entry.page_number = fixture.page_number;
  external_entry.page_generation = fixture.page_generation;
  external_entry.extent_id = fixture.extent_id;
  external_entry.extent_epoch = fixture.extent_epoch;
  external_entry.relation_epoch = Facts().relation_epoch;
  external_entry.catalog_epoch = Facts().catalog_epoch;
  external_entry.final_through_local_transaction_id = mga::MakeLocalTransactionId(90);
  external_entry.cache_generation = Facts().cache_generation;
  external_entry.invalidation_generation = Facts().invalidation_generation;
  external_entry.horizon_epoch = Facts().horizon_epoch;
  external_entry.snapshot_epoch = Facts().snapshot_epoch;
  external_entry.all_visible = true;
  external_cache.page_visibility_entries.push_back(external_entry);
  const auto external = mga::EvaluateCachedPageVisibilityStatus(
      &external_cache, PageRequest(fixture),
      mga::VisibilityStatusCacheProbe::page_all_visible);
  Require(!external.accepted,
          "ODF-058 parser-owned page cache evidence was accepted");
  Require(external.refusal_reason ==
              "visibility_status_cache_external_provenance_refused",
          "ODF-058 external provenance refusal reason mismatch");
}

void RelationNoOlderReaderCacheUsesSnapshotHorizonOnlyAsEvidence() {
  auto cache = mga::MakeMgaVisibilityStatusCache(10, 20);
  const PageFixture fixture;

  const auto cached = mga::CacheRelationNoOlderReaderStatus(
      &cache, RelationRequest(fixture.relation_uuid, 90));
  Require(cached.accepted,
          "ODF-058 relation no-older-reader cache insert refused");
  Require(cached.no_older_reader,
          "ODF-058 relation cache lost no-older-reader flag");

  const auto accelerated = mga::EvaluateCachedRelationNoOlderReaderStatus(
      &cache, RelationRequest(fixture.relation_uuid, 80));
  Require(accelerated.accepted,
          "ODF-058 relation no-older-reader cache did not accelerate");
  Require(!accelerated.cache_is_transaction_finality_authority,
          "ODF-058 relation reader cache became finality authority");

  auto old_reader_facts = Facts();
  old_reader_facts.oldest_snapshot_local_transaction_id =
      mga::MakeLocalTransactionId(80);
  const auto old_reader = mga::EvaluateCachedRelationNoOlderReaderStatus(
      &cache, RelationRequest(fixture.relation_uuid, 90, old_reader_facts));
  Require(!old_reader.accepted,
          "ODF-058 older reader horizon was accepted");
  Require(old_reader.refusal_reason == "relation_reader_horizon_not_closed",
          "ODF-058 older reader refusal reason mismatch");
}

}  // namespace

int main() {
  TxidRangeCacheAcceleratesOnlyInventoryCommittedRanges();
  MissingStaleAndAmbiguousTxidCacheFailClosed();
  CacheContainerInvalidationGenerationFailsClosed();
  PageVisibilityAndAllCommittedCacheAcceleratesOnlyWithPageEvidence();
  RelationNoOlderReaderCacheUsesSnapshotHorizonOnlyAsEvidence();
  return 0;
}
