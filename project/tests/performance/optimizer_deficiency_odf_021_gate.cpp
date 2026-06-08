// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/pinned_descriptor_cache.hpp"
#include "optimizer_statistics_full.hpp"
#include "prepared_execution_template.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

api::EngineUuid Uuid(const std::string& value) {
  api::EngineUuid uuid;
  uuid.canonical = value;
  return uuid;
}

api::EngineDescriptor Descriptor(const std::string& uuid, const std::string& encoded) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(uuid);
  descriptor.descriptor_kind = "table";
  descriptor.canonical_type_name = "customer";
  descriptor.encoded_descriptor = encoded;
  return descriptor;
}

api::CatalogPinnedDescriptorCacheKey BaseCatalogKey() {
  api::CatalogPinnedDescriptorCacheKey key;
  key.descriptor_family = "catalog_descriptor";
  key.catalog_epoch = 101;
  key.security_epoch = 202;
  key.resource_policy_epoch = 303;
  key.name_resolution_epoch = 404;
  key.descriptor_set_digest = "descset.customer.v1";
  key.object_uuids = {"rel.customer", "schema.public"};
  key.index_uuids = {"idx.customer.id"};
  key.security_policy_identity = "security.policy.reader";
  key.redaction_policy_identity = "redaction.policy.customer_mask";
  key.resource_policy_identity = "resource.policy.oltp";
  return key;
}

api::CatalogPinnedDescriptorSnapshot BaseCatalogSnapshot(api::CatalogPinnedDescriptorCacheKey key) {
  api::CatalogPinnedDescriptorSnapshot snapshot;
  snapshot.key = std::move(key);
  snapshot.descriptor = Descriptor("rel.customer", "version=1;columns=id,name");
  snapshot.descriptors = {snapshot.descriptor};
  snapshot.descriptor_owner = {Uuid("rel.customer"), "table"};
  snapshot.primary_object = snapshot.descriptor_owner;
  snapshot.result_shape.result_kind = "descriptor";
  snapshot.result_shape.columns = snapshot.descriptors;
  snapshot.evidence = {
      "descriptor_metadata_cache_snapshot=read_only",
      "mga_visibility_recheck=preserved",
      "security_authorization_recheck=preserved",
  };
  return snapshot;
}

bool CatalogPinnedCacheHitMissRefusalAndImmutableSnapshot() {
  api::CatalogPinnedDescriptorCache cache;
  auto key = BaseCatalogKey();

  const auto miss = cache.Lookup(key);
  if (!Require(!miss.ok, "empty pinned catalog cache unexpectedly hit") ||
      !Require(miss.diagnostic_code == "SB_CATALOG_PINNED_DESCRIPTOR_CACHE_MISS",
               "catalog cache miss diagnostic mismatch: " + miss.diagnostic_code)) {
    return false;
  }

  auto missing_epoch = key;
  missing_epoch.security_epoch = 0;
  const auto refused = cache.Lookup(missing_epoch);
  if (!Require(!refused.ok, "missing security epoch unexpectedly succeeded") ||
      !Require(refused.diagnostic_code == "SB_CATALOG_PINNED_DESCRIPTOR_EPOCH_REQUIRED",
               "missing epoch diagnostic mismatch: " + refused.diagnostic_code) ||
      !Require(refused.detail == "security_epoch is required",
               "missing epoch detail mismatch: " + refused.detail)) {
    return false;
  }

  auto snapshot = BaseCatalogSnapshot(key);
  const auto put = cache.Put(snapshot);
  snapshot.descriptor.encoded_descriptor = "version=2;mutable-source-change";
  const auto hit = cache.Lookup(key);
  return Require(put.ok, "catalog pinned descriptor put failed: " + put.diagnostic_code) &&
         Require(hit.ok && hit.cache_hit, "catalog pinned descriptor did not hit after put") &&
         Require(hit.snapshot->descriptor.encoded_descriptor == "version=1;columns=id,name",
                 "catalog pinned descriptor did not preserve immutable copied snapshot") &&
         Require(hit.snapshot.use_count() >= 1, "catalog pinned descriptor did not return shared const snapshot") &&
         Require(hit.cache_key.find("catalog_epoch=101") != std::string::npos,
                 "catalog cache key omitted catalog epoch") &&
         Require(hit.cache_key.find("security_epoch=202") != std::string::npos,
                 "catalog cache key omitted security epoch") &&
         Require(hit.cache_key.find("resource_policy_epoch=303") != std::string::npos,
                 "catalog cache key omitted resource/policy epoch") &&
         Require(hit.cache_key.find("name_resolution_epoch=404") != std::string::npos,
                 "catalog cache key omitted name-resolution epoch") &&
         Require(hit.cache_key.find("descset.customer.v1") != std::string::npos,
                 "catalog cache key omitted descriptor set digest") &&
         Require(hit.cache_key.find("rel.customer") != std::string::npos,
                 "catalog cache key omitted object UUID") &&
         Require(hit.cache_key.find("idx.customer.id") != std::string::npos,
                 "catalog cache key omitted index UUID") &&
         Require(hit.cache_key.find("security.policy.reader") != std::string::npos,
                 "catalog cache key omitted security policy identity") &&
         Require(hit.cache_key.find("redaction.policy.customer_mask") != std::string::npos,
                 "catalog cache key omitted redaction policy identity");
}

bool CatalogInvalidationReportsExactEntriesAndReasons() {
  api::CatalogPinnedDescriptorCache cache;
  const auto key = BaseCatalogKey();
  if (!Require(cache.Put(BaseCatalogSnapshot(key)).ok, "catalog put before invalidation failed")) return false;

  api::CatalogPinnedDescriptorInvalidationEvent event;
  event.event_kind = "catalog_alter";
  event.dependency_uuid = "rel.customer";
  event.event_epoch = 102;
  event.reason = "ddl_catalog_mutation";
  const auto invalidated = cache.Invalidate(event);
  if (!Require(invalidated.invalidated_entries.size() == 1,
               "catalog DDL invalidation did not report exactly one entry") ||
      !Require(invalidated.invalidated_entries[0].reason == "ddl_catalog_mutation",
               "catalog DDL invalidation reason mismatch") ||
      !Require(Has(invalidated.invalidated_entries[0].object_uuids, "rel.customer"),
               "catalog invalidation did not expose invalidated object UUID") ||
      !Require(!cache.Lookup(key).ok, "catalog descriptor remained cached after DDL invalidation")) {
    return false;
  }

  if (!Require(cache.Put(BaseCatalogSnapshot(key)).ok, "catalog put before security invalidation failed")) return false;
  event = {};
  event.event_kind = "security_epoch";
  event.event_epoch = 203;
  event.reason = "security_epoch_change";
  if (!Require(cache.Invalidate(event).invalidated_entries.size() == 1,
               "security epoch invalidation did not evict pinned descriptor")) {
    return false;
  }

  if (!Require(cache.Put(BaseCatalogSnapshot(key)).ok, "catalog put before redaction invalidation failed")) return false;
  event = {};
  event.event_kind = "redaction_policy_change";
  event.redaction_policy_identity = "redaction.policy.customer_mask";
  event.reason = "redaction_policy_epoch_change";
  return Require(cache.Invalidate(event).invalidated_entries.size() == 1,
                 "redaction policy invalidation did not evict pinned descriptor");
}

opt::OptimizerStatsIdentity StatsIdentity(const std::string& object_uuid,
                                          const std::string& statistic_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = 505;
  identity.catalog_epoch = 101;
  identity.transaction_visibility_epoch = 88;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kExact;
  return identity;
}

opt::OptimizerPinnedStatsDescriptorKey BaseStatsKey() {
  opt::OptimizerPinnedStatsDescriptorKey key;
  key.catalog_epoch = 101;
  key.security_epoch = 202;
  key.resource_policy_epoch = 303;
  key.name_resolution_epoch = 404;
  key.stats_epoch = 505;
  key.descriptor_set_digest = "descset.customer.v1";
  key.object_uuids = {"rel.customer"};
  key.index_uuids = {"idx.customer.id"};
  key.security_policy_identity = "security.policy.reader";
  key.redaction_policy_identity = "redaction.policy.customer_mask";
  return key;
}

opt::OptimizerPinnedStatsDescriptorSnapshot BaseStatsSnapshot() {
  opt::OptimizerStatisticsStore store;
  opt::TableCardinalityStats table;
  table.identity = StatsIdentity("rel.customer", "stat.customer.table");
  table.row_count = 100;
  table.visible_row_count = 99;
  table.page_count = 7;
  table.average_row_bytes = 64;
  store.UpsertTable(table);

  opt::IndexStats index;
  index.identity = StatsIdentity("rel.customer", "stat.customer.idx");
  index.index_uuid = "idx.customer.id";
  index.relation_uuid = "rel.customer";
  index.key_column_uuids = {"col.customer.id"};
  index.height = 2;
  index.leaf_pages = 4;
  index.distinct_keys = 100;
  store.UpsertIndex(index);

  opt::OptimizerPinnedStatsDescriptorSnapshot snapshot;
  snapshot.key = BaseStatsKey();
  snapshot.stats_snapshot = store.Snapshot("stats.customer.505");
  return snapshot;
}

bool OptimizerStatsPinnedCacheHitRefusalAndInvalidation() {
  opt::OptimizerPinnedStatsDescriptorCache cache;
  const auto key = BaseStatsKey();

  auto missing_stats_epoch = key;
  missing_stats_epoch.stats_epoch = 0;
  const auto refused = cache.Lookup(missing_stats_epoch);
  if (!Require(!refused.ok, "missing stats epoch unexpectedly succeeded") ||
      !Require(refused.diagnostic_code == "SB_OPT_PINNED_STATS_EPOCH_REQUIRED",
               "missing stats epoch diagnostic mismatch: " + refused.diagnostic_code)) {
    return false;
  }

  const auto put = cache.Put(BaseStatsSnapshot());
  const auto hit = cache.Lookup(key);
  if (!Require(put.ok, "stats pinned descriptor put failed: " + put.diagnostic_code) ||
      !Require(hit.ok && hit.cache_hit, "stats pinned descriptor did not hit after put") ||
      !Require(hit.snapshot->stats_snapshot.stats_epoch == 505,
               "stats pinned descriptor did not preserve stats epoch") ||
      !Require(hit.cache_key.find("stats_epoch=505") != std::string::npos,
               "stats cache key omitted stats epoch") ||
      !Require(hit.cache_key.find("idx.customer.id") != std::string::npos,
               "stats cache key omitted index UUID")) {
    return false;
  }

  opt::StatsInvalidationEvent stats_refresh;
  stats_refresh.event_kind = "statistics_refresh";
  stats_refresh.object_uuid = "rel.customer";
  stats_refresh.new_stats_epoch = 506;
  stats_refresh.reason = "stats_refresh";
  const auto invalidated = cache.Invalidate(stats_refresh);
  if (!Require(invalidated.invalidated_entries.size() == 1,
               "stats refresh did not invalidate pinned stats descriptor") ||
      !Require(invalidated.invalidated_entries[0].reason == "stats_refresh",
               "stats invalidation reason mismatch")) {
    return false;
  }

  if (!Require(cache.Put(BaseStatsSnapshot()).ok, "stats put before policy invalidation failed")) return false;
  opt::StatsInvalidationEvent policy_change;
  policy_change.event_kind = "redaction_policy_change";
  policy_change.redaction_policy_identity = "redaction.policy.customer_mask";
  policy_change.reason = "redaction_policy_epoch_change";
  return Require(cache.Invalidate(policy_change).invalidated_entries.size() == 1,
                 "redaction policy change did not invalidate pinned stats descriptor");
}

bool OptimizerStatisticsStoreInvalidatesGlobalPinnedStatsCache() {
  auto& cache = opt::GlobalOptimizerPinnedStatsDescriptorCache();
  cache.Clear();
  const auto key = BaseStatsKey();
  if (!Require(cache.Put(BaseStatsSnapshot()).ok, "global stats cache put before refresh failed") ||
      !Require(cache.Lookup(key).ok, "global stats cache did not hit before refresh")) {
    return false;
  }

  opt::OptimizerStatisticsStore store;
  opt::TableCardinalityStats refreshed;
  refreshed.identity = StatsIdentity("rel.customer", "stat.customer.table");
  refreshed.identity.stats_epoch = 506;
  refreshed.row_count = 101;
  store.UpsertTable(refreshed);
  if (!Require(!cache.Lookup(key).ok, "stats refresh did not invalidate global pinned stats cache")) {
    return false;
  }

  if (!Require(cache.Put(BaseStatsSnapshot()).ok, "global stats cache put before stale mark failed") ||
      !Require(cache.Lookup(key).ok, "global stats cache did not hit before stale mark")) {
    return false;
  }
  store.MarkStaleByObject("rel.customer", 102);
  return Require(!cache.Lookup(key).ok, "stats stale mark did not invalidate global pinned stats cache");
}

bool PreparedTemplateConsumesPinnedDescriptorsWithRecheckEvidence() {
  exec::PreparedTemplateCache cache;
  api::EngineDescriptor descriptor = Descriptor("rel.customer", "version=1;columns=id,name");

  exec::PreparedDescriptorSlot slot;
  slot.stable_name = "col.customer.id";
  slot.descriptor = descriptor;

  exec::PreparedResultShapeDescriptor result_shape;
  result_shape.result_kind = "projection";
  result_shape.columns = {slot};
  result_shape.digest = exec::PreparedResultShapeDigest(result_shape);

  exec::PreparedTemplateAdmission admission;
  admission.descriptor_slots = {slot};
  admission.result_shape = result_shape;
  admission.key.operation_id = "dml.select.odf021";
  admission.key.sblr_digest_or_trace_key = "trace.odf021";
  admission.key.descriptor_set_digest = "descset.customer.v1";
  admission.key.result_shape_digest = result_shape.digest;
  admission.key.epochs.catalog_epoch = 101;
  admission.key.epochs.security_epoch = 202;
  admission.key.epochs.policy_resource_epoch = 303;
  admission.key.epochs.name_resolution_epoch = 404;
  admission.key.dependency_uuids = {"rel.customer", "idx.customer.id"};

  exec::PreparedPinnedDescriptorReference pinned;
  pinned.cache_key = api::CatalogPinnedDescriptorCacheKeyText(BaseCatalogKey());
  pinned.descriptor_uuid = "rel.customer";
  pinned.object_uuid = "rel.customer";
  pinned.index_uuid = "idx.customer.id";
  pinned.descriptor_set_digest = "descset.customer.v1";
  pinned.catalog_epoch = 101;
  pinned.security_epoch = 202;
  pinned.resource_policy_epoch = 303;
  pinned.name_resolution_epoch = 404;
  pinned.stats_epoch = 505;
  pinned.security_policy_identity = "security.policy.reader";
  pinned.redaction_policy_identity = "redaction.policy.customer_mask";
  admission.pinned_descriptors = {pinned};

  const auto prepared = cache.Prepare(admission);
  if (!Require(prepared.ok, "prepared template refused pinned descriptor: " + prepared.diagnostic_code)) {
    return false;
  }

  exec::PreparedTemplateBindContext bind_context;
  bind_context.engine_context.catalog_generation_id = 101;
  bind_context.engine_context.security_epoch = 202;
  bind_context.engine_context.resource_epoch = 303;
  bind_context.engine_context.name_resolution_epoch = 404;
  bind_context.engine_context.security_context_present = true;
  bind_context.descriptor_set_digest = "descset.customer.v1";
  bind_context.result_shape_digest = result_shape.digest;
  bind_context.dependency_uuids = admission.key.dependency_uuids;

  const auto bound = cache.Bind(*prepared.prepared_template, bind_context);
  if (!Require(bound.ok, "prepared template bind failed: " + bound.diagnostic_code) ||
      !Require(Has(bound.evidence, "pinned_descriptor_snapshots_consumed=1"),
               "prepared bind did not expose pinned descriptor consumption evidence") ||
      !Require(prepared.prepared_template->key.pinned_descriptor_set_digest ==
                   exec::PreparedPinnedDescriptorDigest(prepared.prepared_template->pinned_descriptors),
               "prepared template key did not include pinned descriptor digest") ||
      !Require(Has(bound.evidence, "mga_visibility_recheck=preserved"),
               "prepared bind lost MGA recheck evidence") ||
      !Require(Has(bound.evidence, "security_authorization_recheck=preserved"),
               "prepared bind lost security recheck evidence")) {
    return false;
  }

  auto pinned_stale_admission = admission;
  pinned_stale_admission.key.operation_id = "dml.select.odf021.pinned_stale";
  pinned_stale_admission.pinned_descriptors[0].security_epoch = 999;
  const auto pinned_stale = cache.Prepare(pinned_stale_admission);
  const auto refused = cache.Bind(*pinned_stale.prepared_template, bind_context);
  if (!Require(!refused.ok, "prepared bind accepted stale pinned security epoch") ||
      !Require(refused.diagnostic_code == "SB_PREPARED_TEMPLATE_PINNED_DESCRIPTOR_STALE_SECURITY_EPOCH",
               "prepared pinned stale security diagnostic mismatch: " + refused.diagnostic_code)) {
    return false;
  }

  auto descriptor_mismatch = bind_context;
  descriptor_mismatch.descriptor_set_digest = "descset.customer.v2";
  const auto descriptor_refused = cache.Bind(*prepared.prepared_template, descriptor_mismatch);
  return Require(!descriptor_refused.ok, "prepared bind accepted mismatched pinned descriptor set") &&
         Require(descriptor_refused.diagnostic_code == "SB_PREPARED_TEMPLATE_DESCRIPTOR_MISMATCH" ||
                     descriptor_refused.diagnostic_code ==
                         "SB_PREPARED_TEMPLATE_PINNED_DESCRIPTOR_DESCRIPTOR_MISMATCH",
                 "prepared descriptor mismatch diagnostic mismatch: " +
                     descriptor_refused.diagnostic_code);
}

bool PreparedPinnedDescriptorsSeparateTemplateCacheIdentity() {
  exec::PreparedTemplateCache cache;
  api::EngineDescriptor descriptor = Descriptor("rel.customer", "version=1;columns=id,name");

  exec::PreparedDescriptorSlot slot;
  slot.stable_name = "col.customer.id";
  slot.descriptor = descriptor;

  exec::PreparedResultShapeDescriptor result_shape;
  result_shape.result_kind = "projection";
  result_shape.columns = {slot};
  result_shape.digest = exec::PreparedResultShapeDigest(result_shape);

  auto make_admission = [&](std::string cache_key, std::string redaction_policy) {
    exec::PreparedTemplateAdmission admission;
    admission.descriptor_slots = {slot};
    admission.result_shape = result_shape;
    admission.key.operation_id = "dml.select.odf021.identity";
    admission.key.sblr_digest_or_trace_key = "trace.odf021.identity";
    admission.key.descriptor_set_digest = "descset.customer.v1";
    admission.key.result_shape_digest = result_shape.digest;
    admission.key.epochs.catalog_epoch = 101;
    admission.key.epochs.security_epoch = 202;
    admission.key.epochs.policy_resource_epoch = 303;
    admission.key.epochs.name_resolution_epoch = 404;
    admission.key.dependency_uuids = {"rel.customer"};
    exec::PreparedPinnedDescriptorReference pinned;
    pinned.cache_key = std::move(cache_key);
    pinned.descriptor_uuid = "rel.customer";
    pinned.object_uuid = "rel.customer";
    pinned.descriptor_set_digest = "descset.customer.v1";
    pinned.catalog_epoch = 101;
    pinned.security_epoch = 202;
    pinned.resource_policy_epoch = 303;
    pinned.name_resolution_epoch = 404;
    pinned.security_policy_identity = "security.policy.reader";
    pinned.redaction_policy_identity = std::move(redaction_policy);
    admission.pinned_descriptors = {pinned};
    return admission;
  };

  const auto first = cache.Prepare(make_admission("pinned.cache.a", "redaction.policy.a"));
  const auto second = cache.Prepare(make_admission("pinned.cache.b", "redaction.policy.b"));
  return Require(first.ok, "first pinned template prepare failed: " + first.diagnostic_code) &&
         Require(second.ok, "second pinned template prepare failed: " + second.diagnostic_code) &&
         Require(!second.reused_existing_template,
                 "prepared cache reused a template across different pinned descriptor references") &&
         Require(first.prepared_template->template_id != second.prepared_template->template_id,
                 "different pinned descriptor references produced the same template identity");
}

bool UnsafeSnapshotsFailClosed() {
  api::CatalogPinnedDescriptorCache catalog_cache;
  auto unsafe_catalog = BaseCatalogSnapshot(BaseCatalogKey());
  unsafe_catalog.finality_authority_cached = true;
  const auto catalog_result = catalog_cache.Put(unsafe_catalog);
  if (!Require(!catalog_result.ok, "catalog cache accepted cached finality authority") ||
      !Require(catalog_result.diagnostic_code == "SB_CATALOG_PINNED_DESCRIPTOR_UNSAFE_SNAPSHOT",
               "catalog unsafe snapshot diagnostic mismatch: " + catalog_result.diagnostic_code)) {
    return false;
  }

  opt::OptimizerPinnedStatsDescriptorCache stats_cache;
  auto unsafe_stats = BaseStatsSnapshot();
  unsafe_stats.security_recheck_required = false;
  const auto stats_result = stats_cache.Put(unsafe_stats);
  if (!Require(!stats_result.ok, "stats cache accepted missing security recheck") ||
      !Require(stats_result.diagnostic_code == "SB_OPT_PINNED_STATS_UNSAFE_SNAPSHOT",
               "stats unsafe snapshot diagnostic mismatch: " + stats_result.diagnostic_code)) {
    return false;
  }

  exec::PreparedTemplateCache prepared_cache;
  exec::PreparedTemplateAdmission admission;
  admission.key.operation_id = "dml.select.odf021.unsafe";
  admission.key.sblr_digest_or_trace_key = "trace.unsafe";
  admission.key.descriptor_set_digest = "descset.customer.v1";
  api::EngineDescriptor descriptor = Descriptor("rel.customer", "version=1");
  exec::PreparedDescriptorSlot slot;
  slot.stable_name = "rel.customer";
  slot.descriptor = descriptor;
  admission.descriptor_slots = {slot};
  admission.result_shape.result_kind = "projection";
  admission.result_shape.columns = {slot};
  admission.result_shape.digest = exec::PreparedResultShapeDigest(admission.result_shape);
  admission.key.result_shape_digest = admission.result_shape.digest;
  exec::PreparedPinnedDescriptorReference pinned;
  pinned.cache_key = "unsafe";
  pinned.object_uuid = "rel.customer";
  pinned.descriptor_set_digest = "descset.customer.v1";
  pinned.finality_authority_cached = true;
  admission.pinned_descriptors = {pinned};
  const auto prepared = prepared_cache.Prepare(admission);
  return Require(!prepared.ok, "prepared cache accepted unsafe pinned descriptor") &&
         Require(prepared.diagnostic_code == "SB_PREPARED_TEMPLATE_PINNED_DESCRIPTOR_UNSAFE",
                 "prepared unsafe pinned diagnostic mismatch: " + prepared.diagnostic_code);
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  if (!CatalogPinnedCacheHitMissRefusalAndImmutableSnapshot()) return 1;
  if (!CatalogInvalidationReportsExactEntriesAndReasons()) return 1;
  if (!OptimizerStatsPinnedCacheHitRefusalAndInvalidation()) return 1;
  if (!OptimizerStatisticsStoreInvalidatesGlobalPinnedStatsCache()) return 1;
  if (!PreparedTemplateConsumesPinnedDescriptorsWithRecheckEvidence()) return 1;
  if (!PreparedPinnedDescriptorsSeparateTemplateCacheIdentity()) return 1;
  if (!UnsafeSnapshotsFailClosed()) return 1;
  return 0;
}
