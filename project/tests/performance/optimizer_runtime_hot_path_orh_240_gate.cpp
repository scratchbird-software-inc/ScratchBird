// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "current_row_map.hpp"
#include "mga_page_finality_evidence.hpp"
#include "mga_page_finality_map.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"
#include "visibility_status_cache.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mga = scratchbird::transaction::mga;
namespace opt = scratchbird::engine::optimizer;
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-240 gate failure: " << message << '\n';
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
  const auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  auto value = generated.value;
  value.bytes[15] = suffix;
  const auto typed = uuid::MakeTypedUuid(kind, value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

std::string UuidText(platform::UuidKind kind,
                     platform::u64 millis,
                     platform::byte suffix) {
  return uuid::UuidToString(GeneratedUuid(kind, millis, suffix).value);
}

bool HasEvidence(const std::vector<mga::CurrentRowMapEvidenceField>& evidence,
                 std::string_view name,
                 std::string_view value) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& field) {
    return field.name == name && field.value.find(value) != std::string::npos;
  });
}

bool HasEvidence(
    const std::vector<mga::VisibilityStatusCacheEvidenceField>& evidence,
    std::string_view name,
    std::string_view value) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& field) {
    return field.name == name && field.value.find(value) != std::string::npos;
  });
}

bool HasEvidence(const std::vector<mga::PageFinalityEvidenceField>& evidence,
                 std::string_view name,
                 std::string_view value) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& field) {
    return field.name == name && field.value.find(value) != std::string::npos;
  });
}

mga::LocalTransactionInventory CommittedInventory(std::uint64_t count) {
  auto inventory = mga::MakeEmptyLocalTransactionInventory();
  for (std::uint64_t ordinal = 1; ordinal <= count; ++ordinal) {
    auto begin = mga::BeginLocalTransaction(
        inventory,
        GeneratedUuid(platform::UuidKind::transaction,
                      1702400000000ull + ordinal,
                      static_cast<platform::byte>(0x20 + ordinal)),
        1702400000000ull + ordinal);
    Require(begin.ok(), "begin transaction failed");
    auto commit = mga::CommitLocalTransaction(
        begin.inventory,
        begin.entry.identity.local_id,
        1702400001000ull + ordinal);
    Require(commit.ok(), "commit transaction failed");
    inventory = std::move(commit.inventory);
    const auto lookup =
        mga::LookupLocalTransaction(inventory, mga::MakeLocalTransactionId(ordinal));
    Require(lookup.ok() && lookup.entry.evidence_record_written,
            "committed inventory evidence missing");
  }
  return inventory;
}

mga::VisibilityStatusCacheFacts VisibilityFacts() {
  mga::VisibilityStatusCacheFacts facts;
  facts.cache_generation = 11;
  facts.invalidation_generation = 12;
  facts.horizon_epoch = 13;
  facts.snapshot_epoch = 14;
  facts.relation_epoch = 15;
  facts.catalog_epoch = 16;
  facts.reader_visible_through_local_transaction_id =
      mga::MakeLocalTransactionId(10);
  facts.oldest_active_local_transaction_id = mga::MakeLocalTransactionId(11);
  facts.oldest_snapshot_local_transaction_id = mga::MakeLocalTransactionId(11);
  facts.transaction_inventory_authoritative = true;
  facts.transaction_horizon_authoritative = true;
  facts.normal_mga_visibility_authority_available = true;
  return facts;
}

mga::PageFinalityObservedFacts PageFacts(const std::string& relation_uuid) {
  mga::PageFinalityObservedFacts facts;
  facts.requested_scope = mga::PageFinalityScope::page;
  facts.relation_uuid = relation_uuid;
  facts.page_number = 42;
  facts.page_generation = 420;
  facts.extent_id = 7;
  facts.extent_epoch = 8;
  facts.relation_epoch = 15;
  facts.catalog_epoch = 16;
  facts.reader_visible_through_local_transaction_id =
      mga::MakeLocalTransactionId(10);
  facts.oldest_active_local_transaction_id = mga::MakeLocalTransactionId(11);
  facts.transaction_horizon_authoritative = true;
  facts.transaction_inventory_authoritative = true;
  facts.normal_mga_visibility_authority_available = true;
  return facts;
}

mga::PageFinalityMapEntry PageEntry(const std::string& relation_uuid) {
  mga::PageFinalityMapEntry entry;
  entry.scope = mga::PageFinalityScope::page;
  entry.status = mga::PageFinalityMapStatus::current;
  entry.provenance = mga::PageFinalityProvenance::engine_mga_transaction_inventory;
  entry.relation_uuid = relation_uuid;
  entry.page_number = 42;
  entry.page_generation = 420;
  entry.extent_id = 7;
  entry.extent_epoch = 8;
  entry.relation_epoch = 15;
  entry.catalog_epoch = 16;
  entry.final_through_local_transaction_id = mga::MakeLocalTransactionId(5);
  entry.map_generation = 17;
  entry.persisted_record_present = true;
  entry.checksum_valid = true;
  entry.all_visible = true;
  entry.all_final = false;
  return entry;
}

mga::PageFinalityMapEntry ExtentEntry(const std::string& relation_uuid) {
  auto entry = PageEntry(relation_uuid);
  entry.scope = mga::PageFinalityScope::extent;
  entry.page_number = 0;
  entry.page_generation = 420;
  entry.all_final = true;
  return entry;
}

mga::CurrentRowMapObservedFacts CurrentRowFacts(
    const std::string& relation_uuid,
    const std::string& row_uuid) {
  mga::CurrentRowMapObservedFacts facts;
  facts.relation_uuid = relation_uuid;
  facts.row_uuid = row_uuid;
  facts.relation_epoch = 15;
  facts.catalog_epoch = 16;
  facts.security_epoch = 17;
  facts.redaction_epoch = 18;
  facts.map_generation = 19;
  facts.invalidation_generation = 20;
  facts.reader_visible_through_local_transaction_id =
      mga::MakeLocalTransactionId(10);
  facts.oldest_active_local_transaction_id = mga::MakeLocalTransactionId(11);
  facts.durable_mga_inventory_proof = true;
  facts.transaction_horizon_authoritative = true;
  facts.normal_mga_visibility_authority_available = true;
  facts.security_recheck_planned = true;
  return facts;
}

void TestCurrentRowMapRebuildAndAdvisoryLookup() {
  const std::string relation_uuid =
      UuidText(platform::UuidKind::object, 1702400100000ull, 0x31);
  const std::string row_uuid =
      UuidText(platform::UuidKind::row, 1702400101000ull, 0x32);
  const std::string version_uuid =
      UuidText(platform::UuidKind::row, 1702400102000ull, 0x33);

  mga::CurrentRowMapRebuildRequest rebuild;
  rebuild.relation_uuid = relation_uuid;
  rebuild.relation_epoch = 15;
  rebuild.catalog_epoch = 16;
  rebuild.security_epoch = 17;
  rebuild.redaction_epoch = 18;
  rebuild.map_generation = 19;
  rebuild.invalidation_generation = 20;
  rebuild.authoritative_base_rows_proof = true;
  rebuild.durable_mga_inventory_proof = true;
  rebuild.base_rows.push_back(
      {row_uuid, version_uuid, 3, mga::MakeLocalTransactionId(5), false, true});

  auto rebuilt = mga::RebuildCurrentRowMapFromAuthoritativeBaseRows(rebuild);
  Require(rebuilt.ok, "current-row map rebuild from authoritative rows failed");
  Require(rebuilt.rebuilt_entry_count == 1,
          "current-row rebuild entry count mismatch");
  Require(HasEvidence(rebuilt.evidence,
                      "repair_truth_source",
                      "authoritative_base_rows"),
          "current-row rebuild truth-source evidence missing");
  Require(HasEvidence(rebuilt.evidence,
                      "durable_mga_inventory_remains_authority",
                      "true"),
          "current-row rebuild MGA authority evidence missing");

  auto facts = CurrentRowFacts(relation_uuid, row_uuid);
  auto decision = mga::LookupCurrentRowMap(&rebuilt.map, facts);
  Require(decision.accepted, "current-row map lookup was refused");
  Require(decision.current_visible, "current-row map did not return visible row");
  Require(decision.normal_mga_recheck_required,
          "current-row map skipped MGA recheck");
  Require(decision.security_recheck_required,
          "current-row map skipped security recheck");
  Require(!decision.map_is_visibility_authority &&
              !decision.map_is_transaction_finality_authority,
          "current-row map became visibility/finality authority");
  Require(HasEvidence(decision.evidence,
                      "current_version_uuid",
                      version_uuid),
          "current-row version evidence mismatch");

  auto stale_facts = facts;
  stale_facts.map_generation += 1;
  auto stale = mga::LookupCurrentRowMap(&rebuilt.map, stale_facts);
  Require(!stale.accepted &&
              stale.refusal_reason == "current_row_map_not_current",
          "stale current-row map generation was not refused");

  auto missing_mga = facts;
  missing_mga.durable_mga_inventory_proof = false;
  auto missing_mga_decision = mga::LookupCurrentRowMap(&rebuilt.map, missing_mga);
  Require(!missing_mga_decision.accepted &&
              missing_mga_decision.refusal_reason ==
                  "durable_mga_inventory_authority_required",
          "current-row map admitted missing durable MGA proof");

  auto parser_claim = facts;
  parser_claim.parser_client_or_reference_authority = true;
  auto parser_refusal = mga::LookupCurrentRowMap(&rebuilt.map, parser_claim);
  Require(!parser_refusal.accepted &&
              parser_refusal.refusal_reason ==
                  "parser_client_or_reference_authority_forbidden",
          "current-row map admitted parser/client/reference authority");
  Require(rebuilt.map.counters.accepted >= 1 &&
              rebuilt.map.counters.stale_refusals >= 1 &&
              rebuilt.map.counters.authority_refusals >= 2,
          "current-row map did not retain accepted/refused telemetry");

  rebuild.map_self_authoritative = true;
  auto unsafe_rebuild =
      mga::RebuildCurrentRowMapFromAuthoritativeBaseRows(rebuild);
  Require(!unsafe_rebuild.ok &&
              unsafe_rebuild.refusal_reason ==
                  "current_row_map_self_repair_authority_refused",
          "current-row map self-authoritative repair was admitted");
}

void TestVisibilityStatusAcceleratorsAndFallback() {
  const auto inventory = CommittedInventory(3);
  auto facts = VisibilityFacts();
  auto cache = mga::MakeMgaVisibilityStatusCache(facts.cache_generation,
                                                facts.invalidation_generation);
  mga::TransactionStatusRangeCacheRequest request;
  request.first_local_transaction_id = mga::MakeLocalTransactionId(1);
  request.last_local_transaction_id = mga::MakeLocalTransactionId(3);
  request.facts = facts;
  auto cached =
      mga::CacheTransactionStatusRangeFromInventory(&cache, inventory, request);
  Require(cached.accepted && cached.all_committed,
          "transaction range visibility accelerator did not cache");
  Require(HasEvidence(cached.evidence,
                      "authority_source",
                      "durable_mga_transaction_inventory"),
          "transaction range cache lost MGA authority source");
  auto accepted = mga::EvaluateCachedTransactionStatusRange(&cache, request);
  Require(accepted.accepted && accepted.all_committed,
          "transaction range visibility accelerator was not consumed");
  Require(!accepted.cache_is_transaction_finality_authority &&
              accepted.durable_mga_inventory_remains_authority,
          "visibility cache became transaction finality authority");

  auto stale = request;
  stale.facts.invalidation_generation += 1;
  auto refused = mga::EvaluateCachedTransactionStatusRange(&cache, stale);
  Require(!refused.accepted &&
              refused.refusal_reason ==
                  "visibility_status_cache_generation_mismatch",
          "stale visibility accelerator generation was not refused");
  Require(refused.authoritative_path_required,
          "stale visibility accelerator did not require fallback");

  auto missing_authority = request;
  missing_authority.facts.transaction_inventory_authoritative = false;
  refused = mga::EvaluateCachedTransactionStatusRange(&cache, missing_authority);
  Require(!refused.accepted &&
              refused.refusal_reason == "mga_authority_inputs_uncertain",
          "visibility cache admitted missing durable inventory authority");

  const auto lookup =
      mga::LookupLocalTransaction(inventory, mga::MakeLocalTransactionId(3));
  Require(lookup.ok() && lookup.entry.evidence_record_written,
          "authoritative fallback inventory scan failed after cache refusal");
}

void TestPageFinalityMapsAndCacheAdmission() {
  const std::string relation_uuid =
      UuidText(platform::UuidKind::object, 1702400200000ull, 0x41);
  page::MgaPageFinalityMap finality_map;
  finality_map.entries.push_back(PageEntry(relation_uuid));
  finality_map.entries.push_back(ExtentEntry(relation_uuid));
  auto facts = PageFacts(relation_uuid);

  auto page_result = page::LookupPageAllVisibleEvidence(
      &finality_map, facts, mga::PageFinalityConsumer::index_only_scan);
  Require(page_result.decision.accepted &&
              page_result.decision.all_visible,
          "page-finality all-visible map was not accepted");
  Require(!page_result.decision.map_is_transaction_finality_authority &&
              page_result.decision.durable_mga_inventory_remains_authority,
          "page-finality map became transaction finality authority");
  Require(HasEvidence(page_result.decision.evidence,
                      "authority_source",
                      "durable_mga_transaction_inventory"),
          "page-finality authority evidence missing");

  const auto optimizer_evidence =
      opt::BuildOptimizerMgaPageFinalityEvidence(page_result);
  Require(optimizer_evidence.present && optimizer_evidence.accepted,
          "optimizer page-finality evidence did not preserve acceptance");
  Require(!optimizer_evidence.finality_map_transaction_authority,
          "optimizer treated finality map as transaction authority");

  auto visibility_facts = VisibilityFacts();
  auto cache = mga::MakeMgaVisibilityStatusCache(
      visibility_facts.cache_generation,
      visibility_facts.invalidation_generation);
  mga::PageVisibilityStatusCacheRequest page_cache;
  page_cache.relation_uuid = relation_uuid;
  page_cache.page_number = facts.page_number;
  page_cache.page_generation = facts.page_generation;
  page_cache.extent_id = facts.extent_id;
  page_cache.extent_epoch = facts.extent_epoch;
  page_cache.final_through_local_transaction_id =
      page_result.decision.evidence_name.empty()
          ? mga::MakeLocalTransactionId(0)
          : mga::MakeLocalTransactionId(5);
  page_cache.facts = visibility_facts;
  auto cache_page = mga::CachePageVisibilityStatus(
      &cache, page_cache, page_result.decision, true);
  Require(cache_page.accepted && cache_page.all_visible &&
              cache_page.all_committed,
          "page visibility cache did not admit page-finality evidence");
  auto accepted = mga::EvaluateCachedPageVisibilityStatus(
      &cache, page_cache, mga::VisibilityStatusCacheProbe::page_all_visible);
  Require(accepted.accepted && accepted.all_visible,
          "page visibility cache was not consumed");
  Require(!accepted.cache_is_transaction_finality_authority,
          "page visibility cache became transaction finality authority");

  auto authoritative_page_claim = page_result.decision;
  authoritative_page_claim.map_is_transaction_finality_authority = true;
  auto rejected_page = mga::CachePageVisibilityStatus(
      &cache, page_cache, authoritative_page_claim, true);
  Require(!rejected_page.accepted &&
              rejected_page.refusal_reason ==
                  "page_finality_evidence_not_acceptable_for_cache",
          "page-finality-as-authority was admitted to visibility cache");

  auto stale_entry = PageEntry(relation_uuid);
  stale_entry.status = mga::PageFinalityMapStatus::stale;
  page::MgaPageFinalityMap stale_map;
  stale_map.entries.push_back(stale_entry);
  auto stale_result = page::LookupPageAllVisibleEvidence(
      &stale_map, facts, mga::PageFinalityConsumer::index_only_scan);
  Require(!stale_result.decision.accepted &&
              stale_result.decision.refusal_reason == "finality_map_not_current",
          "stale page-finality map was not refused");

  auto unsafe_facts = facts;
  unsafe_facts.transaction_inventory_authoritative = false;
  auto unsafe = page::LookupPageAllVisibleEvidence(
      &finality_map, unsafe_facts, mga::PageFinalityConsumer::index_only_scan);
  Require(!unsafe.decision.accepted &&
              unsafe.decision.refusal_reason ==
                  "transaction_horizon_or_inventory_uncertain",
          "page-finality map admitted missing durable MGA proof");

  auto extent_facts = facts;
  extent_facts.requested_scope = mga::PageFinalityScope::extent;
  auto extent_result = page::LookupExtentAllFinalEvidence(
      &finality_map, extent_facts, mga::PageFinalityConsumer::cleanup);
  Require(extent_result.decision.accepted && extent_result.decision.all_final,
          "extent all-final map was not accepted for cleanup");
  auto cleanup = mga::EvaluateExactIndexCleanupAuthority(
      extent_result.decision,
      mga::MakeLocalTransactionId(5),
      true,
      true);
  Require(cleanup.accepted &&
              cleanup.authority_source == "durable_mga_transaction_inventory",
          "cleanup authority did not remain with MGA inventory");
}

}  // namespace

int main() {
  TestCurrentRowMapRebuildAndAdvisoryLookup();
  TestVisibilityStatusAcceleratorsAndFallback();
  TestPageFinalityMapsAndCacheAdmission();
  return EXIT_SUCCESS;
}
