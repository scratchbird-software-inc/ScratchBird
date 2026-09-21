// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "transaction_snapshot.hpp"
#include "transaction_inventory_validation.hpp"
#include "transaction_evidence.hpp"
#include "transaction_recovery.hpp"
#include "transaction/local_commit_publication.hpp"
#include "page_finality_evidence.hpp"
#include "visibility_status_cache.hpp"
#include "transaction_cleanup_horizon_service.hpp"
#include "transaction_cleanup.hpp"
#include "uuid.hpp"
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>
#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <tuple>

static_assert(!std::is_copy_constructible_v<scratchbird::transaction::mga::PublishedSnapshotPin>);
static_assert(std::is_nothrow_move_constructible_v<scratchbird::transaction::mga::PublishedSnapshotPin>);

namespace mga = scratchbird::transaction::mga;

// Explicit projection fixture, not an admitted policy/snapshot or restore grant.
static mga::TransactionEvidenceContext EvidenceContextFixture() {
  mga::TransactionEvidenceContext context;
  auto identity = [](unsigned tag) {
    scratchbird::core::platform::Uuid id;
    id.bytes[0] = 1; id.bytes[6] = 0x70; id.bytes[8] = 0x80; id.bytes[15] = tag;
    return id;
  };
  context.database_uuid = identity(201);
  context.snapshot_uuid = identity(202);
  context.policy_snapshot_uuid = identity(203);
  context.snapshot_generation = 11; context.catalog_generation = 12;
  context.security_generation = 13; context.policy_generation = 14;
  return context;
}

namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
static unsigned checks = 0;
static void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
struct Fixture {
  mga::LocalTransactionInventory inventory;
  mga::SnapshotVectorDescriptor snapshot;
  Fixture() {
    inventory = mga::MakeEmptyLocalTransactionInventory();
    for (unsigned i = 0; i < 2; ++i) {
      const auto id = uuid::GenerateEngineIdentityV7(platform::UuidKind::transaction, 1000+i);
      Check(id.ok(), "real transaction UUID issuance");
      auto begun = mga::BeginLocalTransaction(inventory, id.value, 1000+i);
      Check(begun.ok(), "real transaction begin");
      inventory = std::move(begun.inventory);
    }
    auto published = mga::PublishStatementStableSnapshotVector(inventory, mga::MakeLocalTransactionId(2), 1003);
    Check(published.ok(), "real statement snapshot publication");
    snapshot = std::move(published.descriptor);
    Check(snapshot.retention_horizon_transaction.value == 1, "snapshot retains oldest unresolved transaction1");
    auto committed = mga::CommitLocalTransaction(inventory, mga::MakeLocalTransactionId(1), 1004);
    Check(committed.ok(), "competing transaction commit");
    inventory = std::move(committed.inventory);
  }
  ~Fixture() {
    mga::RevokePublishedSnapshotVector(snapshot.snapshot_uuid);
    mga::ReleasePublishedSnapshotVector(snapshot.snapshot_uuid);
  }
  mga::AuthoritativeCleanupHorizonResult Cleanup() const {
    mga::AuthoritativeCleanupHorizonRequest request;
    request.inventory = inventory;
    request.inventory_authoritative = true;
    request.inventory_complete = true;
    return mga::ComputeAuthoritativeCleanupHorizon(request);
  }
};
static void AdditionalCases();
static void InventoryAdmissionCases();
static void ClusterRecoveryOwnershipCases() {
  const auto id = uuid::GenerateEngineIdentityV7(platform::UuidKind::transaction, 900);
  Check(id.ok(), "cluster recovery identity fixture");
  const auto begun = mga::BeginLocalTransaction(mga::MakeEmptyLocalTransactionInventory(), id.value, 1000);
  Check(begun.ok(), "cluster recovery candidate fixture");
  const auto fields = [](const mga::TransactionInventoryEntry& e) {
    return std::make_tuple(e.identity.local_id.value, e.identity.transaction_uuid.value,
        e.identity.transaction_uuid.kind, e.identity.scope, e.state, e.archived_from_state,
        e.begin_unix_epoch_millis, e.final_unix_epoch_millis,
        e.begin_visible_through_local_transaction_id, e.begin_visible_through_commit_sequence,
        e.commit_sequence, e.evidence_record_required, e.evidence_record_written, e.rollback_only);
  };
  const std::array unresolved{mga::TransactionState::created, mga::TransactionState::active,
      mga::TransactionState::read_only_active, mga::TransactionState::preparing,
      mga::TransactionState::prepared, mga::TransactionState::committing,
      mga::TransactionState::rolling_back, mga::TransactionState::limbo, mga::TransactionState::recovering};
  for (const auto state : unresolved) for (unsigned flags = 0; flags != 8; ++flags) {
    auto inventory = begun.inventory;
    auto& entry = inventory.entries.front();
    entry.identity.scope = mga::TransactionScope::cluster_global;
    entry.state = state;
    entry.rollback_only = flags & 1;
    entry.evidence_record_required = flags & 2;
    entry.evidence_record_written = flags & 4;
    Check(*mga::ValidateLocalTransactionInventoryStructure(inventory) == '\0', "cluster unresolved structural fixture");
    const auto classified = mga::ClassifyLocalTransactionForRecovery(entry);
    Check(classified.observed_state == state && classified.fail_closed &&
        classified.action == mga::TransactionRecoveryAction::cluster_provider_required &&
        classified.stable_reason == "cluster_recovery_requires_provider_decision", "local evidence decided cluster finality");
    const auto recovered = mga::ApplyLocalTransactionInventoryRecovery(inventory, 1200);
    Check(recovered.ok() && recovered.write_admission_must_remain_fenced && !recovered.inventory_changed &&
        recovered.recovered_inventory.entries.size() == 1 &&
        fields(recovered.recovered_inventory.entries.front()) == fields(entry) &&
        recovered.recovered_inventory.next_commit_sequence == inventory.next_commit_sequence &&
        recovered.recovered_inventory.next_local_transaction_id == inventory.next_local_transaction_id &&
        recovered.recovered_inventory.publication_base == inventory.publication_base,
        "local recovery altered unresolved cluster identity, counters or evidence");
    for (const auto& attempted : {mga::PrepareLocalTransaction(inventory, entry.identity.local_id),
        mga::CommitLocalTransaction(inventory, entry.identity.local_id, 1200),
        mga::RollbackLocalTransaction(inventory, entry.identity.local_id, 1200),
        mga::AbortLocalTransaction(inventory, entry.identity.local_id, 1200)})
      Check(!attempted.ok() && attempted.inventory.entries.empty() &&
          attempted.diagnostic.message_key == "transaction.inventory.local_scope_required",
          "local transaction transformation accepted cluster-global authority");
  }
  for (const auto state : {mga::TransactionState::committed, mga::TransactionState::rolled_back,
                          mga::TransactionState::failed_terminal})
    for (const bool archived : {false, true}) for (const bool rollback_only : {false, true}) {
      auto inventory = begun.inventory;
      auto& entry = inventory.entries.front();
      entry.identity.scope = mga::TransactionScope::cluster_global;
      entry.state = archived ? mga::TransactionState::archived : state;
      entry.archived_from_state = archived ? state : mga::TransactionState::none;
      entry.rollback_only = rollback_only;
      entry.evidence_record_written = true;
      entry.final_unix_epoch_millis = 1100;
      if (state == mga::TransactionState::committed) {entry.commit_sequence = 1; inventory.next_commit_sequence = 2;}
      const auto recovered = mga::ApplyLocalTransactionInventoryRecovery(inventory, 1200);
      Check(recovered.ok() && !recovered.inventory_changed &&
          recovered.write_admission_must_remain_fenced == (state == mga::TransactionState::failed_terminal) &&
          fields(recovered.recovered_inventory.entries.front()) == fields(entry), "cluster terminal or archived outcome changed");
    }
  auto limbo = begun.inventory;
  limbo.entries.front().identity.scope = mga::TransactionScope::cluster_global;
  limbo.entries.front().state = mga::TransactionState::limbo;
  for (const auto decision : {mga::LimboOperatorDecision::commit, mga::LimboOperatorDecision::rollback,
                             mga::LimboOperatorDecision::fail_terminal}) {
    mga::LimboOperatorResolutionPolicy policy;
    policy.operator_decision_authoritative = true;
    policy.operator_evidence_reference = "local-operator-is-not-cluster-provider";
    const auto refused = mga::ResolveLimboLocalTransactionWithOperatorDecision(limbo,
        limbo.entries.front().identity.local_id, decision, 1200, policy);
    Check(!refused.ok() && refused.inventory.entries.empty() &&
        refused.diagnostic.diagnostic_code == "SB-SNTXN-LIMBO-EXTERNAL-PROVIDER-REQUIRED",
        "local operator policy became a cluster provider decision");
  }
  for (const auto scope : {mga::TransactionScope::unknown, static_cast<mga::TransactionScope>(65535)}) {
    auto entry = begun.entry; entry.identity.scope = scope;
    const auto classified = mga::ClassifyLocalTransactionForRecovery(entry);
    Check(classified.fail_closed && classified.action == mga::TransactionRecoveryAction::fail_closed_ambiguous &&
        classified.stable_reason == "invalid_transaction_scope", "invalid scope was implicitly local");
  }
  const auto local = mga::ApplyLocalTransactionInventoryRecovery(begun.inventory, 1200);
  Check(local.ok() && local.inventory_changed && !local.write_admission_must_remain_fenced &&
      local.recovered_inventory.entries.front().state == mga::TransactionState::rolled_back,
      "local uncommitted recovery was disabled with cluster recovery");
  Check(std::string_view(mga::TransactionRecoveryActionName(mga::TransactionRecoveryAction::cluster_provider_required)) ==
      "cluster_provider_required", "cluster recovery classification has no stable name");
}
static void ArchivedRecoveryCases() {
  Fixture fixture;
  auto inventory = fixture.inventory;
  auto& entry = inventory.entries.front();
  entry.state = mga::TransactionState::archived;
  entry.archived_from_state = mga::TransactionState::failed_terminal;
  entry.commit_sequence = 0;
  Check(*mga::ValidateLocalTransactionInventoryStructure(inventory) == '\0', "failed archive fixture admission");
  const auto failed = mga::ClassifyLocalTransactionForRecovery(entry);
  Check(failed.fail_closed && failed.action == mga::TransactionRecoveryAction::fail_closed_ambiguous,
      "archived failed-terminal transaction escaped recovery review");
  for (const bool rollback_only : {false, true}) {
    entry.rollback_only = rollback_only;
    for (unsigned encoded = 0; encoded <= 65535; ++encoded) {
      const auto origin = static_cast<mga::TransactionState>(encoded);
      entry.archived_from_state = origin;
      const auto classified = mga::ClassifyLocalTransactionForRecovery(entry);
      const bool safe = origin == mga::TransactionState::committed || origin == mga::TransactionState::rolled_back;
      Check(classified.observed_state == mga::TransactionState::archived && classified.fail_closed == !safe &&
          classified.action == (safe ? mga::TransactionRecoveryAction::no_action :
              mga::TransactionRecoveryAction::fail_closed_ambiguous), "archive origin recovery matrix changed outcome");
    }
    entry.state = mga::TransactionState::failed_terminal;
    entry.archived_from_state = mga::TransactionState::none;
    Check(mga::ClassifyLocalTransactionForRecovery(entry).fail_closed,
        "rollback-only failed-terminal state escaped recovery review");
    entry.state = mga::TransactionState::archived;
  }
  entry.rollback_only = false;
  for (const auto origin : {mga::TransactionState::committed, mga::TransactionState::rolled_back,
                           mga::TransactionState::failed_terminal}) {
    entry.archived_from_state = origin;
    entry.commit_sequence = origin == mga::TransactionState::committed ? 1 : 0;
    const auto classified = mga::ClassifyTransactionInventoryForRestore(inventory, EvidenceContextFixture(), false);
    const bool safe = origin != mga::TransactionState::failed_terminal;
    Check(classified.ok() == safe && classified.restore_allowed == safe && classified.records.size() == inventory.entries.size(),
        "restore discarded archive finality");
    const auto& evidence = classified.records.front();
    Check(evidence.observed_state == "archived" && evidence.terminal &&
          evidence.terminal_state == mga::TransactionStateName(origin) &&
          evidence.restore_classification == (safe ? "restore_terminal_evidence" : "refuse_fail_closed"),
        "lineage lost exact archived terminal outcome");
    const auto recovery = mga::ApplyLocalTransactionInventoryRecovery(inventory, 1200);
    Check(recovery.ok() && recovery.write_admission_must_remain_fenced == !safe &&
          recovery.recovered_inventory.entries.front().state == mga::TransactionState::archived &&
          recovery.recovered_inventory.entries.front().archived_from_state == origin &&
          recovery.recovered_inventory.entries.front().commit_sequence == entry.commit_sequence,
        "recovery rewrote archived finality");
    mga::TransactionInventoryCompactionRequest compact;
    compact.inventory = inventory;
    compact.inventory_authoritative = true;
    compact.oldest_required_local_transaction_id = mga::MakeLocalTransactionId(inventory.next_local_transaction_id);
    const auto compacted = mga::CompactLocalTransactionInventory(compact);
    Check(compacted.ok() && compacted.compacted_entry_count == (safe ? 1u : 0u) &&
        mga::LookupLocalTransaction(compacted.inventory, entry.identity.local_id).ok() == !safe,
        "compaction discarded unresolved failed-terminal review authority");
  }
  for (unsigned encoded = 0; encoded <= 65535; ++encoded) {
    entry.state = static_cast<mga::TransactionState>(encoded);
    entry.archived_from_state = mga::TransactionState::rolled_back;
    Check(mga::ClassifyLocalTransactionForRecovery(entry).fail_closed == (entry.state != mga::TransactionState::archived),
        "nonarchived recovery accepted misplaced archive origin");
  }
  auto malformed = fixture.inventory;
  malformed.entries.push_back(malformed.entries.front());
  const auto invalid = mga::ClassifyTransactionInventoryForRestore(malformed, EvidenceContextFixture(), false);
  Check(!invalid.ok() && !invalid.restore_allowed && invalid.records.empty() &&
      invalid.error == mga::TransactionEvidenceError::invalid_inventory,
      "restore published a prefix from duplicate native inventory");
  for (const auto origin : {mga::TransactionState::none, mga::TransactionState::active,
                           mga::TransactionState::prepared, mga::TransactionState::archived,
                           static_cast<mga::TransactionState>(65535)}) {
    malformed = fixture.inventory;
    malformed.entries.front().state = mga::TransactionState::archived;
    malformed.entries.front().archived_from_state = origin;
    const auto lineage = mga::BuildTransactionLineageEvidence(malformed, EvidenceContextFixture());
    Check(lineage.ok() && !lineage.records.empty() && !lineage.records.front().terminal && lineage.records.front().terminal_state.empty() &&
        lineage.records.front().restore_classification == "refuse_fail_closed", "invalid archive lineage invented terminal evidence");
    const auto refused = mga::ClassifyTransactionInventoryForRestore(malformed, EvidenceContextFixture(), false);
    Check(!refused.ok() && !refused.restore_allowed && refused.records.empty(), "invalid archive restore published records");
  }
  const std::vector<std::function<void(mga::LocalTransactionInventory&)>> invalid_sequences{
    [](auto& v) { v.next_commit_sequence = 0; },
    [](auto& v) { v.entries[0].begin_visible_through_commit_sequence = v.next_commit_sequence; },
    [](auto& v) { v.entries[0].commit_sequence = 0; },
    [](auto& v) { v.entries[0].commit_sequence = v.next_commit_sequence; },
    [](auto& v) { v.entries[1].commit_sequence = 1; },
    [](auto& v) { v.entries[1].state = mga::TransactionState::committed; v.entries[1].commit_sequence = 1; },
    [](auto& v) { v.entries[0].state = mga::TransactionState::rolled_back; },
    [](auto& v) { v.entries[0].archived_from_state = mga::TransactionState::committed; }
  };
  for (const auto& mutate : invalid_sequences) {
    malformed = fixture.inventory; mutate(malformed);
    for (unsigned order = 0; order < 2; ++order) {
      std::reverse(malformed.entries.begin(), malformed.entries.end());
      const auto refused = mga::ClassifyTransactionInventoryForRestore(malformed, EvidenceContextFixture(), false);
      Check(!refused.ok() && !refused.restore_allowed && refused.records.empty(), "invalid restore sequence admitted a record prefix");
    }
  }
  const auto empty = mga::MakeEmptyLocalTransactionInventory();
  const auto empty_restore = mga::ClassifyTransactionInventoryForRestore(empty, EvidenceContextFixture(), false);
  Check(empty_restore.ok() && empty_restore.restore_allowed && empty_restore.records.empty(), "valid empty restore inventory refused");
  Check(!mga::ClassifyTransactionInventoryForRestore(empty, EvidenceContextFixture(), true).restore_allowed &&
        !mga::ClassifyTransactionInventoryForRestore(empty, {}, false).restore_allowed &&
        !mga::ClassifyTransactionInventoryForRestore(empty, mga::TransactionEvidenceContext{}, false).restore_allowed,
        "archive repair bypassed WAL or restore context refusals");
  std::cout << "archived_recovery origin_cases=131072 misplaced_cases=65536\n";
}
static void PublicationOutcomeCases() {
  namespace api = scratchbird::engine::internal_api;
  using Decision = api::LocalCommitPublicationRecoveryClass;
  mga::TransactionInventoryEntry entry;
  for (unsigned encoded = 0; encoded <= 65535; ++encoded) {
    const auto state = static_cast<mga::TransactionState>(encoded);
    Decision expected = Decision::in_doubt;
    if (state == mga::TransactionState::committed) expected = Decision::committed_by_inventory;
    else if (state == mga::TransactionState::rolled_back) expected = Decision::abandoned_by_rollback;
    else if (state == mga::TransactionState::created || state == mga::TransactionState::active ||
             state == mga::TransactionState::read_only_active || state == mga::TransactionState::preparing ||
             state == mga::TransactionState::committing || state == mga::TransactionState::rolling_back)
      expected = Decision::retryable_unpublished;
    entry.state = state; entry.archived_from_state = mga::TransactionState::none;
    Check(api::ClassifyLocalCommitPublicationInventoryOutcome(entry) == expected,
        "publication ordinary/unknown outcome classification drifted");
    entry.state = mga::TransactionState::archived; entry.archived_from_state = state;
    const auto archived_expected = state == mga::TransactionState::committed ? Decision::committed_by_inventory :
        state == mga::TransactionState::rolled_back ? Decision::abandoned_by_rollback : Decision::in_doubt;
    Check(api::ClassifyLocalCommitPublicationInventoryOutcome(entry) == archived_expected,
        "publication archive invented commit/rollback from nonfinal or failed origin");
    entry.state = state; entry.archived_from_state = mga::TransactionState::failed_terminal;
    Check(api::ClassifyLocalCommitPublicationInventoryOutcome(entry) == Decision::in_doubt,
        "publication misplaced/failed archive origin authorized mutation");
  }
  std::cout << "publication_outcome encoded_cases=196608\n";
}
static void PageFinalityAdmissionCases() {
  mga::PageFinalityMapEntry entry;
  entry.scope = mga::PageFinalityScope::page;
  entry.status = mga::PageFinalityMapStatus::current;
  entry.provenance = mga::PageFinalityProvenance::engine_mga_transaction_inventory;
  const auto relation = uuid::GenerateDurableEngineIdentityV7(platform::UuidKind::object, 1300);
  Check(relation.ok(), "page finality binary identity issuance");
  entry.relation_uuid = relation.value.value;
  entry.page_number = 3; entry.page_generation = 4; entry.extent_id = 5;
  entry.extent_epoch = 6; entry.relation_epoch = 7; entry.catalog_epoch = 8;
  entry.map_generation = 9; entry.final_through_local_transaction_id = mga::MakeLocalTransactionId(10);
  entry.persisted_record_present = true; entry.checksum_valid = true;
  entry.all_visible = true; entry.all_final = true;
  mga::PageFinalityObservedFacts facts;
  facts.requested_scope = entry.scope; facts.relation_uuid = entry.relation_uuid;
  facts.page_number = entry.page_number; facts.page_generation = entry.page_generation;
  facts.extent_id = entry.extent_id; facts.extent_epoch = entry.extent_epoch;
  facts.relation_epoch = entry.relation_epoch; facts.catalog_epoch = entry.catalog_epoch;
  facts.reader_visible_through_local_transaction_id = mga::MakeLocalTransactionId(11);
  facts.oldest_active_local_transaction_id = mga::MakeLocalTransactionId(12);
  facts.transaction_horizon_authoritative = true; facts.transaction_inventory_authoritative = true;
  facts.normal_mga_visibility_authority_available = true;
  Check(mga::EvaluatePageFinalityEvidence(entry, facts, mga::PageFinalityConsumer::index_only_scan).accepted,
      "valid page finality component fixture");
  Check(!mga::EvaluatePageFinalityEvidence(entry, facts, static_cast<mga::PageFinalityConsumer>(65535)).accepted,
      "unknown finality consumer admitted a shortcut");
  static_assert(sizeof(entry.relation_uuid) == 16);
  static_assert(sizeof(mga::PageVisibilityStatusCacheEntry{}.relation_uuid) == 16);
  static_assert(sizeof(mga::RelationNoOlderReaderCacheEntry{}.relation_uuid) == 16);
  for (unsigned encoded = 0; encoded <= 65535; ++encoded) {
    auto variant = entry; auto observed = facts;
    const auto consumer = static_cast<mga::PageFinalityConsumer>(encoded);
    Check(mga::EvaluatePageFinalityEvidence(entry, facts, consumer).accepted == (encoded < 4),
        "finality consumer enum membership drifted");
    variant.status = static_cast<mga::PageFinalityMapStatus>(encoded);
    Check(mga::EvaluatePageFinalityEvidence(variant, facts, mga::PageFinalityConsumer::index_only_scan).accepted == (encoded == 0),
        "noncurrent/unknown finality status admitted");
    variant = entry;
    variant.scope = observed.requested_scope = static_cast<mga::PageFinalityScope>(encoded);
    Check(mga::EvaluatePageFinalityEvidence(variant, observed, mga::PageFinalityConsumer::index_only_scan).accepted == (encoded < 2),
        "unknown matching finality scopes admitted");
  }
  mga::VisibilityStatusCacheFacts cache_facts;
  cache_facts.cache_generation = 1; cache_facts.invalidation_generation = 1;
  cache_facts.horizon_epoch = 1; cache_facts.snapshot_epoch = 1;
  cache_facts.relation_epoch = facts.relation_epoch; cache_facts.catalog_epoch = facts.catalog_epoch;
  cache_facts.reader_visible_through_local_transaction_id = facts.reader_visible_through_local_transaction_id;
  cache_facts.oldest_active_local_transaction_id = facts.oldest_active_local_transaction_id;
  cache_facts.oldest_snapshot_local_transaction_id = mga::MakeLocalTransactionId(13);
  cache_facts.transaction_inventory_authoritative = true; cache_facts.transaction_horizon_authoritative = true;
  cache_facts.normal_mga_visibility_authority_available = true;
  mga::PageVisibilityStatusCacheRequest page_request;
  page_request.relation_uuid = entry.relation_uuid; page_request.page_number = entry.page_number;
  page_request.page_generation = entry.page_generation; page_request.extent_id = entry.extent_id;
  page_request.extent_epoch = entry.extent_epoch; page_request.final_through_local_transaction_id = entry.final_through_local_transaction_id;
  page_request.facts = cache_facts;
  auto cache = mga::MakeMgaVisibilityStatusCache(1, 1);
  Check(mga::CachePageVisibilityStatus(&cache, page_request,
      mga::EvaluatePageFinalityEvidence(entry, facts, mga::PageFinalityConsumer::index_only_scan), true).accepted,
      "binary page cache admission failed");
  auto foreign_request = page_request;
  foreign_request.relation_uuid.bytes[15] ^= 1;
  auto foreign_cache = mga::MakeMgaVisibilityStatusCache(1, 1);
  Check(!mga::CachePageVisibilityStatus(&foreign_cache, foreign_request,
      mga::EvaluatePageFinalityEvidence(entry, facts, mga::PageFinalityConsumer::index_only_scan), true).accepted &&
      foreign_cache.page_visibility_entries.empty(), "foreign finality evidence authorized another relation cache");
  const auto bound = mga::EvaluatePageFinalityEvidence(entry, facts, mga::PageFinalityConsumer::index_only_scan);
  const std::vector<std::function<void(mga::PageVisibilityStatusCacheRequest&)>> misbindings{
      [](auto& r) { ++r.page_number; }, [](auto& r) { ++r.page_generation; },
      [](auto& r) { ++r.extent_id; }, [](auto& r) { ++r.extent_epoch; },
      [](auto& r) { ++r.facts.relation_epoch; }, [](auto& r) { ++r.facts.catalog_epoch; },
      [](auto& r) { --r.final_through_local_transaction_id.value; }};
  for (const auto& mutate : misbindings) {
    auto request = page_request; mutate(request);
    auto empty_cache = mga::MakeMgaVisibilityStatusCache(1, 1);
    const auto refused = mga::CachePageVisibilityStatus(&empty_cache, request, bound, true);
    Check(!refused.accepted && empty_cache.page_visibility_entries.empty() &&
          refused.refusal_reason == "page_finality_evidence_binding_mismatch", "cache ignored an exact finality binding field");
  }
  auto missing = bound; missing.bound_entry.reset();
  Check(!mga::CachePageVisibilityStatus(&foreign_cache, page_request, missing, true).accepted &&
      foreign_cache.page_visibility_entries.empty(), "unbound finality flags authorized page cache");
  auto extent = entry; auto extent_facts = facts;
  extent.scope = extent_facts.requested_scope = mga::PageFinalityScope::extent;
  Check(!mga::CachePageVisibilityStatus(&foreign_cache, page_request,
      mga::EvaluatePageFinalityEvidence(extent, extent_facts, mga::PageFinalityConsumer::summary_pruning), true).accepted,
      "extent proof substituted for exact page cache proof");
  mga::RelationNoOlderReaderCacheRequest relation_request;
  relation_request.relation_uuid = entry.relation_uuid;
  relation_request.no_reader_older_than_local_transaction_id = entry.final_through_local_transaction_id;
  relation_request.facts = cache_facts;
  Check(mga::CacheRelationNoOlderReaderStatus(&cache, relation_request).accepted, "binary relation cache admission failed");
  for (unsigned encoded = 0; encoded <= 65535; ++encoded) {
    cache.page_visibility_entries.front().status = static_cast<mga::VisibilityStatusCacheEntryStatus>(encoded);
    cache.relation_reader_entries.front().status = static_cast<mga::VisibilityStatusCacheEntryStatus>(encoded);
    Check(mga::EvaluateCachedPageVisibilityStatus(&cache, page_request, mga::VisibilityStatusCacheProbe::page_all_visible).accepted ==
        (encoded == 0), "noncurrent/unknown page cache status admitted");
    Check(mga::EvaluateCachedRelationNoOlderReaderStatus(&cache, relation_request).accepted == (encoded == 0),
        "noncurrent/unknown relation cache status admitted");
  }
  cache.page_visibility_entries.front().status = mga::VisibilityStatusCacheEntryStatus::current;
  cache.relation_reader_entries.front().status = mga::VisibilityStatusCacheEntryStatus::current;
  for (unsigned version = 0; version < 16; ++version) for (unsigned variant = 0; variant < 16; ++variant) {
    auto invalid = entry; auto observed = facts;
    invalid.relation_uuid.bytes[6] = static_cast<platform::byte>((invalid.relation_uuid.bytes[6] & 15) | (version << 4));
    invalid.relation_uuid.bytes[8] = static_cast<platform::byte>((invalid.relation_uuid.bytes[8] & 15) | (variant << 4));
    observed.relation_uuid = invalid.relation_uuid;
    const bool admitted = version == 7 && variant >= 8 && variant <= 11;
    Check(mga::EvaluatePageFinalityEvidence(invalid, observed, mga::PageFinalityConsumer::index_only_scan).accepted == admitted,
        "finality accepted non-v7/non-RFC system relation");
    page_request.relation_uuid = cache.page_visibility_entries.front().relation_uuid = invalid.relation_uuid;
    relation_request.relation_uuid = cache.relation_reader_entries.front().relation_uuid = invalid.relation_uuid;
    Check(mga::EvaluateCachedPageVisibilityStatus(&cache, page_request, mga::VisibilityStatusCacheProbe::page_all_visible).accepted == admitted &&
        mga::EvaluateCachedRelationNoOlderReaderStatus(&cache, relation_request).accepted == admitted,
        "visibility cache accepted matching invalid system UUIDs");
  }
  std::cout << "page_finality encoded_membership_cases=327680 uuid_profiles=256\n";
}
static void ArchivedCreatorProjectionCases() {
  Fixture fixture;
  mga::RowVersionMetadata row;
  const auto row_id = uuid::GenerateDurableEngineIdentityV7(platform::UuidKind::row, 1100);
  const auto version_id = uuid::GenerateDurableEngineIdentityV7(platform::UuidKind::row, 1101);
  Check(row_id.ok() && version_id.ok(), "archive projection identities");
  row.identity.row.row_uuid = row_id.value;
  row.identity.version_uuid = version_id.value.value;
  row.identity.creator_transaction = fixture.inventory.entries.front().identity;
  row.identity.version_sequence = 1;
  row.state = mga::RowVersionState::committed;
  row.creator_transaction_state = mga::TransactionState::archived;
  row.payload_present = true;
  const auto horizons = mga::ComputeLocalTransactionHorizons(fixture.inventory);
  Check(horizons.ok(), "archive projection cleanup horizons");
  Check(!mga::ValidateRowVersionMetadata(row).ok(), "unqualified archived creator accepted as row metadata authority");
  Check(mga::EvaluateVisibility(row, {}).decision != mga::VisibilityDecision::visible,
      "unqualified archive made row visible");
  Check(mga::EvaluateLocalCleanupWithHorizons(row, horizons.horizons).decision != mga::CleanupEligibilityDecision::eligible_authoritative,
      "unqualified archive authorized cleanup");
  row.state = mga::RowVersionState::delete_marker; row.payload_present = false;
  Check(mga::EvaluateVersionEffectVisibility(row, {}).decision != mga::VisibilityDecision::visible,
      "unqualified archive made delete effect visible");
  for (unsigned encoded = 0; encoded <= 65535; ++encoded) {
    auto entry = fixture.inventory.entries.front();
    const auto origin = static_cast<mga::TransactionState>(encoded);
    entry.state = mga::TransactionState::archived; entry.archived_from_state = origin;
    const bool terminal = origin == mga::TransactionState::committed || origin == mga::TransactionState::rolled_back ||
        origin == mga::TransactionState::failed_terminal;
    Check(mga::InventoryVisibilityState(entry) == (terminal ? origin : mga::TransactionState::none) &&
        mga::HasCommittedInventoryOutcome(entry) == (origin == mga::TransactionState::committed),
        "archive projection accepted unknown/nonterminal origin or changed terminal outcome");
    entry.state = mga::TransactionState::committed;
    Check(mga::InventoryVisibilityState(entry) == (origin == mga::TransactionState::none ?
        mga::TransactionState::committed : mga::TransactionState::none), "nonarchived creator accepted an archive origin");
  }
  for (const auto origin : {mga::TransactionState::committed, mga::TransactionState::rolled_back,
                           mga::TransactionState::failed_terminal}) {
    auto entry = fixture.inventory.entries.front();
    entry.state = mga::TransactionState::archived; entry.archived_from_state = origin;
    row.creator_transaction_state = mga::InventoryVisibilityState(entry);
    row.creator_commit_sequence = origin == mga::TransactionState::committed ? entry.commit_sequence : 0;
    for (const bool deleted : {false, true}) {
      row.state = deleted ? mga::RowVersionState::delete_marker : origin == mga::TransactionState::committed ?
          mga::RowVersionState::committed : mga::RowVersionState::rolled_back;
      row.payload_present = !deleted;
      Check(mga::ValidateRowVersionMetadata(row).ok(), "valid archive projection refused metadata");
      const auto effect = mga::EvaluateVersionEffectVisibility(row, {});
      Check(effect.ok() && (effect.decision == mga::VisibilityDecision::visible) == (origin == mga::TransactionState::committed),
          "resolved archived row or deletion effect changed outcome");
      if (origin == mga::TransactionState::failed_terminal)
        Check(mga::EvaluateLocalCleanupWithHorizons(row, horizons.horizons).decision != mga::CleanupEligibilityDecision::eligible_authoritative,
            "failed-terminal origin granted cleanup authority");
      else
        Check(mga::EvaluateLocalCleanupWithHorizons(row, horizons.horizons).decision == mga::CleanupEligibilityDecision::eligible_authoritative,
            "resolved commit/rollback version remained blocked after cleanup horizons");
    }
  }
}
int main() try {
  PageFinalityAdmissionCases();
  PublicationOutcomeCases();
  ArchivedRecoveryCases();
  ClusterRecoveryOwnershipCases();
  ArchivedCreatorProjectionCases();
  Fixture f;
  const auto plain = mga::ComputeLocalTransactionHorizons(f.inventory);
  Check(plain.ok() && plain.horizons.oldest_snapshot_transaction.value == 2 &&
        plain.horizons.oldest_active_transaction.value == 2,
        "pure inventory calculator has no runtime pins and defaults OST to OAT");
  const auto retained = f.Cleanup();
  Check(retained.ok() && retained.cleanup_horizon.value == 1,
        "actual cleanup service must retain the published statement snapshot");
  mga::RevokePublishedSnapshotVector(f.snapshot.snapshot_uuid);
  const auto released = f.Cleanup();
  Check(released.ok() && released.cleanup_horizon.value == 2,
        "revocation removes snapshot blocker but not active transaction2");
  // Fixed bounded ownership grid:0..8 pins x owner-release/revoke-one/revoke-txn.
  unsigned cases = 0;
  for (unsigned count = 0; count <= 8; ++count) {
    for (unsigned terminal = 0; terminal < 3; ++terminal) {
      Fixture owned;
      std::vector<mga::PublishedSnapshotPin> pins;
      for (unsigned i = 0; i < count; ++i) {
        auto pin = mga::RetainPublishedSnapshotVector(owned.snapshot.snapshot_uuid);
        Check(pin.valid(), "only live published authority issues pin");
        pins.push_back(std::move(pin));
        Check(!pin.valid() && !pin.Resolve().ok(), "move transfers ownership");
      }
      if (terminal == 1) mga::RevokePublishedSnapshotVector(owned.snapshot.snapshot_uuid);
      if (terminal == 2) mga::RevokePublishedSnapshotVectorsForTransaction(
          owned.snapshot.owning_transaction_uuid, owned.snapshot.owning_transaction);
      mga::ReleasePublishedSnapshotVector(owned.snapshot.snapshot_uuid);
      mga::ReleasePublishedSnapshotVector(owned.snapshot.snapshot_uuid);
      Check(!mga::RetainPublishedSnapshotVector(owned.snapshot.snapshot_uuid).valid(),
            "publication retirement cannot issue late pins");
      Check(!mga::ResolvePublishedSnapshotVector(owned.snapshot.snapshot_uuid).ok(),
            "retained pin does not reopen public snapshot lookup");
      for (auto& pin : pins) {
        const auto observed = pin.Resolve();
        Check(observed.ok() == (terminal == 0), "explicit invalidation revokes retained authority");
        if (terminal == 0) Check(mga::SnapshotVectorDescriptorEqual(observed.descriptor, owned.snapshot),
                                 "pinned snapshot contents remain exact");
        Check(owned.Cleanup().cleanup_horizon.value == (terminal == 0 ? 1U : 2U),
              "runtime cleanup respects remaining pins or explicit revocation");
        pin.Release(); pin.Release();
        Check(!pin.valid(), "pin release idempotent");
      }
      Check(owned.Cleanup().cleanup_horizon.value == 2, "last owner releases only its snapshot horizon");
      Check(mga::PublishedSnapshotRetentionHorizons(owned.inventory).horizons.empty(),
            "terminal snapshot has no retained runtime horizon");
      ++cases;
    }
  }
  Check(cases == 27, "complete fixed ownership grid");
  {
    Fixture first, other;
    mga::ReleasePublishedSnapshotVector(other.snapshot.snapshot_uuid);
    Check(other.Cleanup().cleanup_horizon.value == 2, "same local transaction ID in another inventory is isolated");
    auto invalid = first.snapshot.snapshot_uuid;
    invalid.kind = platform::UuidKind::transaction;
    Check(!mga::RetainPublishedSnapshotVector(invalid).valid(), "wrong UUID kind denied");
    invalid = first.snapshot.snapshot_uuid; invalid.value.bytes[6] ^= 0x30;
    Check(!mga::RetainPublishedSnapshotVector(invalid).valid(), "changed version cannot resolve original binary identity");
    auto pin = mga::RetainPublishedSnapshotVector(first.snapshot.snapshot_uuid);
    const auto id = uuid::GenerateEngineIdentityV7(platform::UuidKind::transaction, 1005);
    Check(id.ok(), "concurrent inventory advance UUID");
    auto advanced = mga::BeginLocalTransaction(first.inventory, id.value, 1005);
    Check(advanced.ok(), "concurrent inventory advance");
    const auto newer = mga::PublishStatementStableSnapshotVector(advanced.inventory, mga::MakeLocalTransactionId(2), 1006);
    Check(newer.ok(), "second statement snapshot");
    const auto stale = first.Cleanup();
    Check(!stale.ok() && stale.diagnostic.diagnostic_code == "SB-MGA-SNAPSHOT-VECTOR-STALE",
          "older inventory cannot silently ignore newer snapshot cohort");
    mga::RevokePublishedSnapshotVector(newer.descriptor.snapshot_uuid);
    mga::ReleasePublishedSnapshotVector(newer.descriptor.snapshot_uuid);
    Check(mga::SnapshotVectorDescriptorEqual(pin.Resolve().descriptor, first.snapshot),
          "inventory advance cannot refresh pinned exclusions");
    auto bad_inventory = first.inventory;
    bad_inventory.entries[1].identity.transaction_uuid.value.bytes[6] = 0x40;
    Check(!mga::PublishStatementStableSnapshotVector(bad_inventory, mga::MakeLocalTransactionId(2), 1007).ok(),
          "system snapshot owner cannot be user UUIDv4");
  }
  //64 deterministic competing-owner histories, not an exhaustive scheduler proof.
  for (unsigned iteration = 0; iteration < 64; ++iteration) {
    Fixture raced;
    std::atomic<bool> start{false};
    std::atomic<unsigned> failures{0};
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 4; ++i) {
      auto pin = mga::RetainPublishedSnapshotVector(raced.snapshot.snapshot_uuid);
      Check(pin.valid(), "concurrent owner pin issuance");
      threads.emplace_back([pin = std::move(pin), &start, &failures]() mutable {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        if (!pin.Resolve().ok()) ++failures;
        pin.Release();
      });
    }
    mga::ReleasePublishedSnapshotVector(raced.snapshot.snapshot_uuid);
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    Check(failures.load() == 0, "ordinary publication release preserves all concurrent readers");
    Check(raced.Cleanup().cleanup_horizon.value == 2, "last concurrent release removes horizon");
  }
  AdditionalCases();
  InventoryAdmissionCases();
  std::cout << "PASS snapshot_retention ownership_cases=27 concurrent_histories=128 checks=" << checks << '\n';
  return 0;
} catch (const std::exception& error) {
  std::cerr << "FAIL check=" << checks << " " << error.what() << '\n';
  return 1;
}

static void InventoryAdmissionCases() {
  Fixture f;
  using Mutation = std::function<void(mga::LocalTransactionInventory&, unsigned)>;
  std::vector<std::pair<std::string, Mutation>> mutations;
  const auto add = [&](const char* name, Mutation mutation) {
    mutations.emplace_back(name, std::move(mutation));
  };
  add("next_zero", [](auto& v, unsigned) { v.next_local_transaction_id = 0; });
  add("next_at_last", [](auto& v, unsigned) { v.next_local_transaction_id = 2; });
  add("next_before_last", [](auto& v, unsigned) { v.next_local_transaction_id = 1; });
  add("local_zero", [](auto& v, unsigned i) { v.entries[i].identity.local_id.value = 0; });
  add("local_at_next", [](auto& v, unsigned i) { v.entries[i].identity.local_id.value = 3; });
  add("local_after_next", [](auto& v, unsigned i) { v.entries[i].identity.local_id.value = 4; });
  add("local_max", [](auto& v, unsigned i) { v.entries[i].identity.local_id.value = std::numeric_limits<platform::u64>::max(); });
  add("duplicate_local", [](auto& v, unsigned i) { v.entries[i].identity.local_id = v.entries[1-i].identity.local_id; });
  add("duplicate_uuid", [](auto& v, unsigned i) { v.entries[i].identity.transaction_uuid = v.entries[1-i].identity.transaction_uuid; });
  add("nil_uuid", [](auto& v, unsigned i) { v.entries[i].identity.transaction_uuid.value.bytes.fill(0); });
  add("wrong_kind", [](auto& v, unsigned i) { v.entries[i].identity.transaction_uuid.kind = platform::UuidKind::object; });
  add("unknown_scope", [](auto& v, unsigned i) { v.entries[i].identity.scope = mga::TransactionScope::unknown; });
  add("invalid_scope", [](auto& v, unsigned i) { v.entries[i].identity.scope = static_cast<mga::TransactionScope>(65535); });
  add("state_none", [](auto& v, unsigned i) { v.entries[i].state = mga::TransactionState::none; });
  add("state_after_last", [](auto& v, unsigned i) { v.entries[i].state = static_cast<mga::TransactionState>(14); });
  add("state_max", [](auto& v, unsigned i) { v.entries[i].state = static_cast<mga::TransactionState>(65535); });
  for (unsigned version = 0; version < 16; ++version) {
    if (version == 7) continue;
    mutations.emplace_back("version_" + std::to_string(version), [version](auto& v, unsigned i) {
      auto& octet = v.entries[i].identity.transaction_uuid.value.bytes[6];
      octet = static_cast<platform::byte>((octet & 15U) | (version << 4U));
    });
  }
  for (unsigned variant = 0; variant < 16; ++variant) {
    if (variant >= 8 && variant <= 11) continue;
    mutations.emplace_back("variant_" + std::to_string(variant), [variant](auto& v, unsigned i) {
      auto& octet = v.entries[i].identity.transaction_uuid.value.bytes[8];
      octet = static_cast<platform::byte>((octet & 15U) | (variant << 4U));
    });
  }
  Check(mutations.size() == 43, "immutable malformed inventory profile count");
  std::array<unsigned, 6> missed{};
  unsigned cases = 0;
  for (const auto& [name, mutate] : mutations) {
    for (unsigned position = 0; position < 2; ++position) {
      for (unsigned reverse = 0; reverse < 2; ++reverse) {
        auto inventory = f.inventory;
        mutate(inventory, position);
        if (reverse) std::reverse(inventory.entries.begin(), inventory.entries.end());
        const auto published = mga::PublishStatementStableSnapshotVector(inventory, f.snapshot.owning_transaction, 3000);
        if (published.ok() || published.descriptor.snapshot_uuid.valid()) ++missed[0];
        // Preserve the original failure without contaminating following cases.
        if (published.descriptor.snapshot_uuid.valid()) {
          mga::RevokePublishedSnapshotVector(published.descriptor.snapshot_uuid);
          mga::ReleasePublishedSnapshotVector(published.descriptor.snapshot_uuid);
        }
        const auto local = mga::CreateLocalTransactionSnapshot(inventory, f.snapshot.owning_transaction);
        if (local.ok()) ++missed[1];
        const auto current = mga::ResolveCurrentStatementStableSnapshotVector(inventory,
            f.snapshot.snapshot_uuid, f.snapshot.owning_transaction_uuid, f.snapshot.owning_transaction);
        if (current.ok() || current.descriptor.snapshot_uuid.valid()) ++missed[2];
        const auto retention = mga::PublishedSnapshotRetentionHorizons(inventory);
        if (retention.ok() || !retention.horizons.empty()) ++missed[3];
        mga::AuthoritativeCleanupHorizonRequest request;
        request.inventory = inventory;
        request.inventory_authoritative = true;
        request.inventory_complete = true;
        const auto cleanup = mga::ComputeAuthoritativeCleanupHorizon(request);
        if (cleanup.ok() || cleanup.cleanup_horizon_authoritative || cleanup.cleanup_horizon.valid()) ++missed[4];
        const auto restore = mga::ClassifyTransactionInventoryForRestore(inventory, EvidenceContextFixture(), false);
        if (restore.ok() || restore.restore_allowed || !restore.records.empty()) ++missed[5];
        Check(mga::SnapshotVectorDescriptorEqual(
            mga::ResolvePublishedSnapshotVector(f.snapshot.snapshot_uuid).descriptor, f.snapshot),
            "malformed inventory admission must not mutate live snapshot authority");
        ++cases;
      }
    }
  }
  std::cout << "inventory_admission cases=" << cases << " missed_by_route=";
  for (const auto count : missed) std::cout << count << ',';
  std::cout << '\n';
  Check(cases == 172 && std::all_of(missed.begin(), missed.end(), [](auto n) { return n == 0; }),
        "all malformed inventories rejected at all six authoritative consumers");
  // All existing non-none state values; both typed scopes and all RFC variant nibbles.
  unsigned positives = 0;
  for (unsigned state = 1; state <= 13; ++state) {
    for (unsigned scope = 0; scope < 2; ++scope) {
      for (unsigned variant = 8; variant <= 11; ++variant) {
        auto inventory = f.inventory;
        auto& entry = inventory.entries[0];
        entry.state = static_cast<mga::TransactionState>(state);
        entry.commit_sequence = (state == 6 || state == 12) ? 1 : 0;
        entry.archived_from_state = state == 12 ? mga::TransactionState::committed : mga::TransactionState::none;
        entry.identity.scope = static_cast<mga::TransactionScope>(scope);
        entry.identity.transaction_uuid.value.bytes[8] = static_cast<platform::byte>(
            (entry.identity.transaction_uuid.value.bytes[8] & 15U) | (variant << 4U));
        const auto published = mga::PublishStatementStableSnapshotVector(inventory, f.snapshot.owning_transaction, 3001);
        Check(published.ok(), "valid inventory state and binary identity must not be over-rejected");
        mga::ReleasePublishedSnapshotVector(published.descriptor.snapshot_uuid);
        Check(mga::CreateLocalTransactionSnapshot(inventory, f.snapshot.owning_transaction).ok(), "valid local snapshot");
        Check(mga::ResolveCurrentStatementStableSnapshotVector(inventory, f.snapshot.snapshot_uuid,
            f.snapshot.owning_transaction_uuid, f.snapshot.owning_transaction).ok(), "valid current snapshot");
        Check(mga::PublishedSnapshotRetentionHorizons(inventory).ok(), "valid retained snapshot inventory");
        mga::AuthoritativeCleanupHorizonRequest request;
        request.inventory = inventory; request.inventory_authoritative = true; request.inventory_complete = true;
        Check(mga::ComputeAuthoritativeCleanupHorizon(request).ok(), "valid cleanup inventory");
        ++positives;
      }
    }
  }
  Check(positives == 104, "complete finite positive inventory grid");
  std::cout << "inventory_admission positive_cases=" << positives << '\n';
  // Exhaust the complete encoded membership domains independently of the
  // implementation switch. These are structural checks, not lifecycle proof.
  for (unsigned encoded = 0; encoded <= 65535; ++encoded) {
    auto inventory = f.inventory;
    inventory.entries[0].state = static_cast<mga::TransactionState>(encoded);
    inventory.entries[0].commit_sequence = (encoded == 6 || encoded == 12) ? 1 : 0;
    inventory.entries[0].archived_from_state = encoded == 12 ? mga::TransactionState::committed : mga::TransactionState::none;
    Check((*mga::ValidateLocalTransactionInventoryStructure(inventory) == '\0') ==
        (encoded >= 1 && encoded <= 13), "complete u16 state membership domain");
    inventory = f.inventory;
    inventory.entries[0].identity.scope = static_cast<mga::TransactionScope>(encoded);
    Check((*mga::ValidateLocalTransactionInventoryStructure(inventory) == '\0') ==
        (encoded == 0 || encoded == 1), "complete u16 scope membership domain");
    inventory = f.inventory;
    auto& bytes = inventory.entries[0].identity.transaction_uuid.value.bytes;
    bytes[6] = static_cast<platform::byte>(encoded >> 8);
    bytes[8] = static_cast<platform::byte>(encoded & 255U);
    const bool admitted_uuid = (encoded & 0xf000U) == 0x7000U &&
                               (encoded & 0xc0U) == 0x80U;
    Check((*mga::ValidateLocalTransactionInventoryStructure(inventory) == '\0') == admitted_uuid,
          "complete version/variant octet domain preserves ordinary UUID data bits");
  }
  auto empty = mga::MakeEmptyLocalTransactionInventory();
  Check(*mga::ValidateLocalTransactionInventoryStructure(empty) == '\0', "empty initial inventory is valid");
  empty.next_local_transaction_id = std::numeric_limits<platform::u64>::max();
  Check(*mga::ValidateLocalTransactionInventoryStructure(empty) == '\0', "empty compacted inventory does not require dense IDs");
  auto sparse = f.inventory;
  sparse.next_local_transaction_id = std::numeric_limits<platform::u64>::max();
  sparse.entries[0].identity.local_id.value = sparse.next_local_transaction_id - 1;
  Check(*mga::ValidateLocalTransactionInventoryStructure(sparse) == '\0', "sparse highest allocated number remains below next");
  std::reverse(sparse.entries.begin(), sparse.entries.end());
  Check(*mga::ValidateLocalTransactionInventoryStructure(sparse) == '\0', "inventory ordering is not identity authority");
  std::cout << "inventory_admission encoded_membership_cases=196608 sparse_empty_cases=4\n";
}

static void AdditionalCases() {
  Fixture f;
  auto pin = mga::RetainPublishedSnapshotVector(f.snapshot.snapshot_uuid);
  Check(pin.valid(), "cleanup consumer owns actual snapshot pin");
  const auto row_id = uuid::GenerateEngineIdentityV7(platform::UuidKind::row, 2000);
  Check(row_id.ok(), "row identity issuance");
  mga::RowVersionMetadata row;
  row.identity.version_uuid = scratchbird::core::uuid::GenerateDurableEngineIdentityV7(
      scratchbird::core::platform::UuidKind::row, 1770000000000ull).value.value;
  row.identity.row.row_uuid = row_id.value;
  row.identity.creator_transaction = f.inventory.entries[0].identity;
  row.identity.version_sequence = 1;
  row.state = mga::RowVersionState::delete_marker;
  row.creator_transaction_state = mga::TransactionState::committed;
  Check(mga::ValidateRowVersionMetadata(row).ok(), "real cleanup input is canonical");
  mga::LocalCleanupWorksetRequest request;
  request.inventory = f.inventory;
  request.inventory_authoritative = true;
  request.row_versions = {row};
  request.emit_reclaim_evidence_records = true;
  mga::ReleasePublishedSnapshotVector(f.snapshot.snapshot_uuid);
  const auto held = mga::ApplyLocalCleanupWithAuthoritativeInventory(request);
  Check(held.ok() && held.retained_row_version_count == 1 &&
        held.reclaimed_row_version_count == 0 && held.reclaim_evidence_records.empty(),
        "actual cleanup workset retains version under pinned snapshot");
  pin.Release();
  const auto unbudgeted = mga::ApplyLocalCleanupWithAuthoritativeInventory(request);
  Check(!unbudgeted.ok() && unbudgeted.bounded_memory_limit_hit &&
        unbudgeted.diagnostic.diagnostic_code == "SB-SNTXN-CLEANUP-RECLAIM-EVIDENCE-LIMIT-EXCEEDED",
        "cleanup evidence requires an explicit positive budget");
  request.max_reclaim_evidence_records = 1;
  const auto freed = mga::ApplyLocalCleanupWithAuthoritativeInventory(request);
  Check(freed.ok() && freed.retained_row_version_count == 0 &&
        freed.reclaimed_row_version_count == 1 && freed.reclaim_evidence_records.size() == 1,
        "actual cleanup workset reclaims after final pin release");
  for (unsigned iteration = 0; iteration < 64; ++iteration) {
    Fixture raced;
    std::atomic<unsigned> ready{0}, failures{0};
    std::atomic<bool> revoked{false};
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i) {
      auto retained = mga::RetainPublishedSnapshotVector(raced.snapshot.snapshot_uuid);
      Check(retained.valid(), "competing revocation pin issuance");
      readers.emplace_back([pin = std::move(retained), &ready, &failures, &revoked]() mutable {
        if (!pin.Resolve().ok()) ++failures;
        ready.fetch_add(1, std::memory_order_release);
        while (!revoked.load(std::memory_order_acquire)) std::this_thread::yield();
        if (pin.Resolve().ok()) ++failures;
        pin.Release();
      });
    }
    while (ready.load(std::memory_order_acquire) != 4) std::this_thread::yield();
    mga::RevokePublishedSnapshotVectorsForTransaction(raced.snapshot.owning_transaction_uuid,
                                                      raced.snapshot.owning_transaction);
    mga::ReleasePublishedSnapshotVector(raced.snapshot.snapshot_uuid);
    revoked.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();
    Check(failures.load() == 0, "revocation fences every retained reader");
    Check(raced.Cleanup().cleanup_horizon.value == 2, "revoked readers hold no cleanup horizon");
  }
}
