// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-117 concurrency/stale-epoch closure gate.

#include "catalog/pinned_descriptor_cache.hpp"
#include "hot_point_lookup_cache.hpp"
#include "nosql/nosql_family_maintenance_api.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"
#include "optimizer_plan_cache.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace mga = scratchbird::transaction::mga;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Has(const std::vector<std::string>& values, std::string_view token) {
  return std::find(values.begin(), values.end(), std::string(token)) != values.end();
}

bool Contains(const std::vector<std::string>& values, std::string_view token) {
  for (const auto& value : values) {
    if (value.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireNoForbiddenRuntimeEvidence(const std::vector<std::string>& values,
                                       std::string_view scenario) {
  for (const auto& value : values) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "audit", "contracts",
          "references", "parser_or_reference_finality_authority=true",
          "parser_transaction_finality_authority=true",
          "provider_transaction_finality_authority=true",
          "provider_visibility_authority=true",
          "client_autocommit_authority=true",
          "write_ahead_log_transaction_finality_authority=true"}) {
      if (value.find(forbidden) != std::string::npos) {
        std::cerr << "forbidden runtime evidence in " << scenario << ": "
                  << value << '\n';
        Fail("ODF-117 runtime evidence leaked forbidden authority or document token");
      }
    }
  }
}

api::EngineUuid EngineUuid(std::string value) {
  api::EngineUuid result;
  result.canonical = std::move(value);
  return result;
}

platform::TypedUuid TestUuid(platform::UuidKind kind, unsigned char salt) {
  platform::TypedUuid value;
  value.kind = kind;
  value.value.bytes[0] = 0x01;
  value.value.bytes[1] = 0x9f;
  value.value.bytes[6] = 0x70;
  value.value.bytes[8] = 0x80;
  value.value.bytes[14] = 0x17;
  value.value.bytes[15] = salt;
  return value;
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, salt);
  Require(generated.ok(), "ODF-117 UUID generation failed");
  return generated.value;
}

mga::TransactionIdentity NewIdentity(platform::u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      NewUuid(platform::UuidKind::transaction, 1779511700000ull + local_id),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "ODF-117 transaction identity creation failed");
  return identity.identity;
}

mga::TransactionInventoryEntry InventoryEntry(platform::u64 local_id,
                                              mga::TransactionState state) {
  mga::TransactionInventoryEntry entry;
  entry.identity = NewIdentity(local_id);
  entry.state = state;
  entry.begin_unix_epoch_millis = 1779511700000ull + local_id;
  if (mga::IsTerminalTransactionState(state)) {
    entry.final_unix_epoch_millis = entry.begin_unix_epoch_millis + 1;
    entry.evidence_record_written = true;
  }
  return entry;
}

mga::AuthoritativeCleanupHorizonRequest HorizonRequest() {
  mga::AuthoritativeCleanupHorizonRequest request;
  for (platform::u64 local_id = 1; local_id < 70; ++local_id) {
    request.inventory.entries.push_back(
        InventoryEntry(local_id, mga::TransactionState::committed));
  }
  request.inventory.next_local_transaction_id = 70;
  request.inventory_authoritative = true;
  request.inventory_complete = true;
  request.active_snapshot_inventory_authoritative = true;
  return request;
}

api::EngineRequestContext Context(platform::u64 local_tx = 117) {
  api::EngineRequestContext context;
  context.database_path = "/tmp/sb_odf_117_concurrency_gate.sbdb";
  context.database_uuid.canonical = "019df117-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical = "019df117-0000-7000-8000-000000000117";
  context.local_transaction_id = local_tx;
  context.security_context_present = true;
  context.request_id = "odf117-concurrency-stale-epoch";
  context.trace_tags = {"optimizer_deficiency_odf_117_gate",
                        "benchmark_clean",
                        "mga_transaction_regression"};
  return context;
}

opt::OptimizerPlanCacheKeyInput BasePlanInput(platform::u64 epoch) {
  opt::OptimizerPlanCacheKeyInput input;
  input.operation_id = "odf117.select.ddl_dml_nosql";
  input.sblr_digest = "sblr:odf117:uuid-bound";
  input.descriptor_set_digest = "descriptor:odf117:rel:v" + std::to_string(epoch);
  input.statistics_snapshot_id = "stats:odf117:" + std::to_string(epoch);
  input.catalog_stats_digest = "catalog-stats:odf117:" + std::to_string(epoch);
  input.cost_profile_id = "cost:local:mga";
  input.executor_capability_set_id = "executor:local:mga";
  input.route_capability_digest = "route:nosql-local:generation=" + std::to_string(epoch);
  input.security_policy_digest = "security:tenant-reader:epoch=" + std::to_string(epoch);
  input.redaction_route_digest = "redaction:mask-secret:epoch=" + std::to_string(epoch);
  input.parameter_shape_digest = "slot0:int64:not_null:point";
  input.memory_grant_class = "memory:small";
  input.memory_grant_digest = "grant:small:64k";
  input.catalog_epoch = epoch;
  input.stats_epoch = epoch + 1;
  input.security_epoch = epoch + 2;
  input.policy_epoch = epoch + 3;
  input.resource_epoch = epoch + 4;
  input.name_resolution_epoch = epoch + 5;
  input.memory_policy_epoch = epoch + 6;
  input.compatibility_epoch = epoch + 7;
  input.format_compatibility_epoch = epoch + 8;
  input.route_epoch = epoch + 9;
  input.object_uuids = {"rel.odf117", "nosql.collection.odf117"};
  input.function_uuids = {"fn.odf117.redact"};
  input.index_uuids = {"idx.odf117.nosql.generation"};
  input.filespace_uuids = {"filespace.odf117.hot"};
  return input;
}

opt::CachedOptimizerPlan CachedPlan(const opt::OptimizerPlanCacheKeyInput& input) {
  opt::CachedOptimizerPlan plan;
  plan.key_input = input;
  plan.cache_key = opt::BuildOptimizerPlanCacheKey(input);
  plan.created_epoch = input.catalog_epoch;
  plan.result.ok = true;
  plan.result.plan_id = "odf117.cached-plan";
  plan.result.diagnostic_code = "ok";
  plan.metadata_only = true;
  plan.mga_visibility_recheck_required = true;
  plan.security_recheck_required = true;
  plan.parser_or_reference_finality_authority = false;
  return plan;
}

api::CatalogPinnedDescriptorCacheKey DescriptorKey(platform::u64 epoch) {
  api::CatalogPinnedDescriptorCacheKey key;
  key.descriptor_family = "catalog_descriptor";
  key.catalog_epoch = epoch;
  key.security_epoch = epoch + 2;
  key.resource_policy_epoch = epoch + 4;
  key.name_resolution_epoch = epoch + 5;
  key.stats_epoch = epoch + 1;
  key.stats_epoch_relevant = true;
  key.descriptor_set_digest = "descriptor:odf117:rel:v" + std::to_string(epoch);
  key.object_uuids = {"rel.odf117", "nosql.collection.odf117"};
  key.index_uuids = {"idx.odf117.nosql.generation"};
  key.security_policy_identity = "security:tenant-reader:epoch=" + std::to_string(epoch);
  key.redaction_policy_identity = "redaction:mask-secret:epoch=" + std::to_string(epoch);
  key.resource_policy_identity = "resource:oltp:epoch=" + std::to_string(epoch);
  return key;
}

api::CatalogPinnedDescriptorSnapshot DescriptorSnapshot(platform::u64 epoch) {
  api::CatalogPinnedDescriptorSnapshot snapshot;
  snapshot.key = DescriptorKey(epoch);
  snapshot.descriptor.descriptor_uuid = EngineUuid("rel.odf117");
  snapshot.descriptor.descriptor_kind = "table";
  snapshot.descriptor.canonical_type_name = "odf117_relation";
  snapshot.descriptor.encoded_descriptor =
      "columns=id,secret_payload;epoch=" + std::to_string(epoch);
  snapshot.descriptors = {snapshot.descriptor};
  snapshot.descriptor_owner = {EngineUuid("rel.odf117"), "table"};
  snapshot.primary_object = snapshot.descriptor_owner;
  snapshot.result_shape.result_kind = "descriptor";
  snapshot.result_shape.columns = snapshot.descriptors;
  snapshot.evidence = {"descriptor_metadata_cache_snapshot=read_only",
                       "mga_visibility_recheck=preserved",
                       "security_authorization_recheck=preserved",
                       "redaction_policy_bound=preserved"};
  return snapshot;
}

idx::HotPointLookupCacheKey HotPointKey(platform::u64 epoch) {
  idx::HotPointLookupCacheKey key;
  key.probe_class = idx::HotPointProbeClass::unique_index_lookup;
  key.database_uuid = TestUuid(platform::UuidKind::database, 0x01);
  key.object_uuid = TestUuid(platform::UuidKind::object, 0x02);
  key.index_uuid = TestUuid(platform::UuidKind::object, 0x03);
  key.encoded_probe_key = "odf117-hot-point";
  key.statistics_snapshot_id = "stats:odf117:" + std::to_string(epoch);
  key.descriptor_set_digest = "descriptor:odf117:rel:v" + std::to_string(epoch);
  key.index_definition_digest = "index:odf117:generation:" + std::to_string(epoch);
  key.security_policy_digest = "security:tenant-reader:epoch=" + std::to_string(epoch);
  key.redaction_policy_digest = "redaction:mask-secret:epoch=" + std::to_string(epoch);
  key.access_policy_digest = "access:exact-point:epoch=" + std::to_string(epoch);
  key.collation_profile_digest = "collation:binary";
  key.catalog_epoch = epoch;
  key.index_epoch = epoch + 1;
  key.statistics_epoch = epoch + 2;
  key.security_epoch = epoch + 3;
  key.policy_epoch = epoch + 4;
  key.object_epoch = epoch + 5;
  key.compatibility_epoch = epoch + 6;
  return key;
}

idx::HotPointLookupCacheEntry HotPointEntry(const idx::HotPointLookupCacheKey& key) {
  idx::HotPointLookupCacheEntry entry;
  entry.key = key;
  entry.created_epoch = key.catalog_epoch;
  entry.dependency_uuids = {key.object_uuid, key.index_uuid};
  idx::HotPointLookupCandidate candidate;
  candidate.locator.table_uuid = key.object_uuid;
  candidate.locator.row_uuid = TestUuid(platform::UuidKind::row, 0x17);
  candidate.locator.version_uuid = TestUuid(platform::UuidKind::row, 0x18);
  candidate.locator.local_transaction_id = 117;
  candidate.proof_kind = "candidate_locator";
  candidate.posting_list_digest = "posting:odf117";
  candidate.candidate_locator_only = true;
  candidate.equality_proof_metadata_only = true;
  candidate.requires_mga_visibility_recheck = true;
  candidate.requires_security_authorization_recheck = true;
  entry.candidates = {candidate};
  return entry;
}

api::EngineNoSqlPhysicalProviderContract ProviderContract(platform::u64 generation) {
  api::EngineNoSqlPhysicalProviderContract contract;
  contract.family = api::EngineNoSqlProviderFamily::kKeyValue;
  contract.scope = api::EngineNoSqlProviderScope::kLocal;
  contract.provider_id = "odf117.local.kv.provider";
  contract.fallback_provider_id = "odf117.exact.mga.security.recheck";
  contract.local_provider_available = true;
  contract.exact_fallback_available = true;
  contract.descriptor_visibility.proof_present = true;
  contract.descriptor_visibility.visible_to_snapshot = true;
  contract.descriptor_visibility.descriptor_shape_compatible = true;
  contract.descriptor_visibility.descriptor_generation = generation;
  contract.security_redaction.proof_present = true;
  contract.security_redaction.redaction_policy_bound = true;
  contract.security_redaction.security_snapshot_bound = true;
  contract.security_redaction.redaction_profile = "masked-secret";
  contract.index_generation.proof_present = true;
  contract.index_generation.visible_to_snapshot = true;
  contract.index_generation.covers_predicate = true;
  contract.index_generation.required_generation = generation;
  contract.index_generation.available_generation = generation;
  contract.index_generation.index_uuid = "idx.odf117.nosql.generation";
  contract.delta_overlay.required = true;
  contract.delta_overlay.proof_present = true;
  contract.delta_overlay.covers_snapshot = true;
  contract.delta_overlay.overlay_generation = generation;
  contract.policy.proof_present = true;
  contract.policy.allowed = true;
  contract.policy.policy_snapshot_uuid = "policy.odf117";
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return contract;
}

api::EnginePlanNoSqlFamilyMaintenanceRequest MaintenanceRequest(bool authoritative = true) {
  api::EnginePlanNoSqlFamilyMaintenanceRequest request;
  request.context = Context();
  request.horizon_request = HorizonRequest();
  request.scheduler_policy.max_total_work_units = 8;
  request.scheduler_policy.max_scheduled_items = 8;
  request.scheduler_policy.default_max_family_work_units = 2;
  request.scheduler_policy.default_max_family_items = 2;
  request.scheduler_policy.max_work_units_per_item = 1;
  request.scheduler_policy.lease_duration_microseconds = 1000;
  request.now_microseconds = 117000;
  request.engine_mga_authoritative = authoritative;
  request.execute_plan = false;

  api::EngineNoSqlMaintenanceGenerationCandidate candidate;
  candidate.family = api::EngineNoSqlProviderFamily::kKeyValue;
  candidate.generation_id = "odf117-kv-generation";
  candidate.generation_kind = "key_value";
  candidate.sealed_local_transaction_id = 20;
  candidate.superseded_local_transaction_id = 21;
  candidate.expires_after_local_transaction_id = 19;
  candidate.estimated_bytes = 4096;
  candidate.generation_evidence_authoritative = true;
  candidate.ttl_evidence_authoritative = true;
  request.candidates.push_back(std::move(candidate));
  return request;
}

void RequirePlanHitPreservesAuthority(const opt::OptimizerPlanCacheLookupResult& hit) {
  Require(hit.hit, "ODF-117 cached plan did not hit");
  Require(hit.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_HIT",
          "ODF-117 plan hit diagnostic changed");
  Require(Has(hit.evidence, "cached_plan_metadata_only=true"),
          "ODF-117 plan hit cached mutable runtime state");
  Require(Has(hit.evidence, "mga_visibility_recheck=preserved"),
          "ODF-117 plan hit lost MGA recheck evidence");
  Require(Has(hit.evidence, "security_authorization_recheck=preserved"),
          "ODF-117 plan hit lost security recheck evidence");
  Require(Has(hit.evidence, "mga_finality_authority=engine_transaction_inventory"),
          "ODF-117 plan hit lost engine MGA finality evidence");
  RequireNoForbiddenRuntimeEvidence(hit.evidence, "plan_hit");
}

void DirectStaleEpochAndSecurityFailures() {
  opt::OptimizerPlanCache plan_cache;
  const auto input = BasePlanInput(1170);
  plan_cache.Put(CachedPlan(input));
  RequirePlanHitPreservesAuthority(plan_cache.Lookup(input));

  auto stale = input;
  stale.catalog_epoch += 1;
  auto lookup = plan_cache.Lookup(stale);
  Require(!lookup.hit &&
              lookup.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH",
          "ODF-117 stale catalog epoch did not fail closed");
  Require(Has(lookup.evidence, "optimizer_plan_cache_stale_epoch"),
          "ODF-117 stale catalog epoch evidence missing");

  auto redaction_changed = input;
  redaction_changed.redaction_route_digest = "redaction:none";
  lookup = plan_cache.Lookup(redaction_changed);
  Require(!lookup.hit &&
              lookup.diagnostic_code ==
                  "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH",
          "ODF-117 redaction policy change did not fail closed");

  auto unsafe = CachedPlan(input);
  unsafe.security_recheck_required = false;
  opt::OptimizerPlanCache unsafe_cache;
  unsafe_cache.Put(unsafe);
  lookup = unsafe_cache.Lookup(input);
  Require(!lookup.hit &&
              lookup.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_AUTHORITY_UNSAFE",
          "ODF-117 unsafe cached authority was reused");

  api::CatalogPinnedDescriptorCache descriptor_cache;
  Require(descriptor_cache.Put(DescriptorSnapshot(1170)).ok,
          "ODF-117 descriptor cache setup failed");
  const auto descriptor_hit = descriptor_cache.Lookup(DescriptorKey(1170));
  Require(descriptor_hit.ok && descriptor_hit.cache_hit,
          "ODF-117 descriptor cache did not hit");

  auto unsafe_descriptor = DescriptorSnapshot(1171);
  unsafe_descriptor.security_recheck_required = false;
  const auto refused_descriptor = descriptor_cache.Put(unsafe_descriptor);
  Require(!refused_descriptor.ok &&
              refused_descriptor.diagnostic_code ==
                  "SB_CATALOG_PINNED_DESCRIPTOR_UNSAFE_SNAPSHOT",
          "ODF-117 unsafe descriptor snapshot was admitted");

  api::CatalogPinnedDescriptorInvalidationEvent redaction_event;
  redaction_event.event_kind = "redaction_policy_change";
  redaction_event.redaction_policy_identity = DescriptorKey(1170).redaction_policy_identity;
  redaction_event.reason = "redaction_policy_epoch_change";
  Require(descriptor_cache.Invalidate(redaction_event).invalidated_entries.size() == 1,
          "ODF-117 redaction invalidation did not evict descriptor");
  Require(!descriptor_cache.Lookup(DescriptorKey(1170)).cache_hit,
          "ODF-117 descriptor cache reused a redaction-stale snapshot");

  idx::AdaptiveHotPointLookupCache hot_cache({4, 64, 64, 64});
  const auto hot_key = HotPointKey(1170);
  Require(hot_cache.Put(HotPointEntry(hot_key)).admitted,
          "ODF-117 hot-point setup admission failed");
  auto hot_stale = hot_key;
  hot_stale.security_epoch += 1;
  const auto hot_result = hot_cache.Lookup(hot_stale);
  Require(!hot_result.cache_hit &&
              hot_result.diagnostic_code ==
                  "SB_INDEX_HOT_POINT_LOOKUP_CACHE_STALE_EPOCH",
          "ODF-117 hot-point stale security epoch did not fail closed");
  Require(Has(hot_result.evidence, "epoch_compatibility=false"),
          "ODF-117 hot-point stale epoch evidence missing");
}

void NoSqlGenerationAndCompactionFailures() {
  auto valid = api::SelectLocalNoSqlPhysicalProvider(ProviderContract(117));
  Require(valid.selected && !valid.fail_closed,
          "ODF-117 valid NoSQL provider proof was not selected");
  Require(Contains(valid.evidence, "row_mga_recheck_required=true") &&
              Contains(valid.evidence, "row_security_recheck_required=true"),
          "ODF-117 NoSQL provider did not preserve MGA/security recheck evidence");
  RequireNoForbiddenRuntimeEvidence(valid.evidence, "nosql_provider_valid");

  auto stale_contract = ProviderContract(117);
  stale_contract.index_generation.required_generation = 118;
  stale_contract.index_generation.available_generation = 117;
  auto stale = api::SelectLocalNoSqlPhysicalProvider(stale_contract);
  Require(!stale.selected && stale.fail_closed &&
              api::EngineNoSqlSelectionHasDiagnostic(
                  stale, api::kNoSqlProviderIndexGenerationStale),
          "ODF-117 stale NoSQL generation was not refused");

  auto missing_security = ProviderContract(117);
  missing_security.security_redaction.proof_present = false;
  missing_security.security_redaction.redaction_policy_bound = false;
  auto denied = api::SelectLocalNoSqlPhysicalProvider(missing_security);
  Require(!denied.selected && denied.fail_closed &&
              api::EngineNoSqlSelectionHasDiagnostic(
                  denied, api::kNoSqlProviderSecurityProofMissing),
          "ODF-117 missing NoSQL security/redaction proof was not refused");

  const auto maintenance = api::EnginePlanNoSqlFamilyMaintenance(MaintenanceRequest());
  Require(maintenance.ok, "ODF-117 NoSQL compaction maintenance planning failed");
  Require(maintenance.agent_result.actions.size() == 2,
          "ODF-117 NoSQL maintenance did not schedule TTL plus compaction");
  bool saw_compaction = false;
  std::vector<std::string> maintenance_values;
  for (const auto& row : maintenance.result_shape.rows) {
    for (const auto& [field, value] : row.fields) {
      maintenance_values.push_back(field);
      maintenance_values.push_back(value.encoded_value);
      if (value.encoded_value == "kv_lsm_generation_compaction") {
        saw_compaction = true;
      }
    }
  }
  for (const auto& evidence : maintenance.evidence) {
    maintenance_values.push_back(evidence.evidence_kind);
    maintenance_values.push_back(evidence.evidence_id);
  }
  Require(saw_compaction, "ODF-117 NoSQL compaction action evidence missing");
  RequireNoForbiddenRuntimeEvidence(maintenance_values, "nosql_maintenance");

  const auto non_authoritative =
      api::EnginePlanNoSqlFamilyMaintenance(MaintenanceRequest(false));
  Require(!non_authoritative.ok,
          "ODF-117 NoSQL maintenance accepted non-authoritative MGA horizon");
}

void NoSqlOptimizerInvalidationVocabulary() {
  const std::vector<std::string> events = {
      "nosql_generation_publish", "nosql_generation_retire",
      "nosql_compaction", "nosql_family_compaction"};
  for (const auto& event_kind : events) {
    opt::OptimizerPlanCache plan_cache;
    const auto input = BasePlanInput(2117);
    plan_cache.Put(CachedPlan(input));
    opt::OptimizerInvalidationEvent event;
    event.event_kind = event_kind;
    event.dependency_uuid = "idx.odf117.nosql.generation";
    event.event_epoch = 2118;
    const auto invalidated = plan_cache.InvalidateWithEvidence(event);
    Require(invalidated.invalidated_count == 1,
            "ODF-117 NoSQL event did not invalidate the cached plan");
    Require(invalidated.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH",
            "ODF-117 NoSQL event diagnostic did not map to stale epoch");
    Require(Contains(invalidated.evidence, event_kind),
            "ODF-117 NoSQL event evidence missing event kind");
    const auto lookup = plan_cache.Lookup(input);
    Require(!lookup.hit &&
                lookup.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH",
            "ODF-117 NoSQL-invalidated plan was reused");
  }
}

void ConcurrentDdlDmlNoSqlInvalidationStress() {
  for (platform::u64 iteration = 0; iteration < 24; ++iteration) {
    const platform::u64 epoch = 3000 + iteration * 20;
    opt::OptimizerPlanCache plan_cache;
    api::CatalogPinnedDescriptorCache descriptor_cache;
    idx::AdaptiveHotPointLookupCache hot_cache({8, 256, 256, 64});

    const auto plan_input = BasePlanInput(epoch);
    const auto descriptor_key = DescriptorKey(epoch);
    const auto hot_key = HotPointKey(epoch);
    plan_cache.Put(CachedPlan(plan_input));
    Require(descriptor_cache.Put(DescriptorSnapshot(epoch)).ok,
            "ODF-117 descriptor setup failed");
    Require(hot_cache.Put(HotPointEntry(hot_key)).admitted,
            "ODF-117 DML hot-point setup failed");

    std::mutex errors_mutex;
    std::vector<std::string> errors;
    const auto record_error = [&](std::string message) {
      std::lock_guard<std::mutex> lock(errors_mutex);
      errors.push_back(std::move(message));
    };

    std::vector<std::thread> readers;
    for (int worker = 0; worker < 6; ++worker) {
      readers.emplace_back([&, worker] {
        for (int pass = 0; pass < 16; ++pass) {
          const auto hit = plan_cache.Lookup(plan_input);
          if (!hit.hit || hit.diagnostic_code != "SB_OPTIMIZER_PLAN_CACHE_HIT" ||
              !Has(hit.evidence, "mga_visibility_recheck=preserved") ||
              !Has(hit.evidence, "security_authorization_recheck=preserved")) {
            record_error("plan reuse lost MGA/security evidence before invalidation");
          }
          const auto descriptor_hit = descriptor_cache.Lookup(descriptor_key);
          if (!descriptor_hit.ok || !descriptor_hit.cache_hit ||
              !descriptor_hit.snapshot->security_recheck_required ||
              !descriptor_hit.snapshot->visibility_recheck_required) {
            record_error("descriptor reuse lost security/MGA recheck evidence");
          }
          (void)worker;
        }
      });
    }
    for (auto& reader : readers) {
      reader.join();
    }
    Require(errors.empty(), errors.empty() ? "" : errors.front());

    std::vector<std::thread> mutators;
    mutators.emplace_back([&] {
      const auto invalidated = plan_cache.InvalidateWithEvidence(
          opt::OptimizerInvalidationEventForMutation(
              "catalog_object_alter", "rel.odf117", epoch + 1));
      if (invalidated.diagnostic_code !=
          "SB_OPTIMIZER_PLAN_CACHE_DEPENDENCY_INVALIDATED") {
        record_error("concurrent DDL invalidation diagnostic changed");
      }
    });
    mutators.emplace_back([&] {
      const auto invalidated = plan_cache.InvalidateWithEvidence(
          opt::OptimizerInvalidationEventForMutation(
              "statistics_refresh", "rel.odf117", epoch + 2));
      if (invalidated.diagnostic_code != "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH") {
        record_error("concurrent statistics refresh diagnostic changed");
      }
    });
    mutators.emplace_back([&] {
      const auto invalidated = plan_cache.InvalidateWithEvidence(
          opt::OptimizerInvalidationEventForMutation(
              "redaction_policy_mutation", "fn.odf117.redact", epoch + 3));
      if (invalidated.diagnostic_code !=
          "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH") {
        record_error("concurrent redaction invalidation diagnostic changed");
      }
    });
    mutators.emplace_back([&] {
      const auto invalidated = plan_cache.InvalidateWithEvidence(
          opt::OptimizerInvalidationEventForMutation(
              "nosql_generation_publication",
              "idx.odf117.nosql.generation",
              epoch + 4));
      if (invalidated.diagnostic_code != "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH") {
        record_error("concurrent NoSQL generation diagnostic changed");
      }
    });
    mutators.emplace_back([&] {
      api::CatalogPinnedDescriptorInvalidationEvent event;
      event.event_kind = "nosql_compaction";
      event.index_uuid = "idx.odf117.nosql.generation";
      event.reason = "nosql_compaction_epoch_change";
      descriptor_cache.Invalidate(event);
    });
    mutators.emplace_back([&] {
      idx::HotPointLookupInvalidationEvent event;
      event.event_kind = "nosql_compaction";
      event.index_uuid = hot_key.index_uuid;
      event.statistics_epoch = hot_key.statistics_epoch + 1;
      const auto invalidated = hot_cache.Invalidate(event);
      if (invalidated.diagnostic_code !=
          "SB_INDEX_HOT_POINT_LOOKUP_CACHE_DEPENDENCY_INVALIDATED") {
        record_error("concurrent DML hot-point invalidation diagnostic changed");
      }
    });
    mutators.emplace_back([&] {
      const auto maintenance =
          api::EnginePlanNoSqlFamilyMaintenance(MaintenanceRequest());
      if (!maintenance.ok || maintenance.agent_result.actions.empty()) {
        record_error("concurrent NoSQL compaction planning failed");
      }
    });
    for (auto& mutator : mutators) {
      mutator.join();
    }
    Require(errors.empty(), errors.empty() ? "" : errors.front());

    const auto stale_plan = plan_cache.Lookup(plan_input);
    Require(!stale_plan.hit,
            "ODF-117 concurrent invalidation allowed stale plan reuse");
    Require(stale_plan.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH" ||
                stale_plan.diagnostic_code ==
                    "SB_OPTIMIZER_PLAN_CACHE_DEPENDENCY_INVALIDATED" ||
                stale_plan.diagnostic_code ==
                    "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH",
            "ODF-117 concurrent stale plan diagnostic was not fail-closed");

    auto new_epoch_plan = plan_input;
    new_epoch_plan.catalog_epoch += 10;
    new_epoch_plan.stats_epoch += 10;
    new_epoch_plan.security_epoch += 10;
    new_epoch_plan.policy_epoch += 10;
    const auto new_epoch_lookup = plan_cache.Lookup(new_epoch_plan);
    Require(!new_epoch_lookup.hit,
            "ODF-117 changed epoch reused an old cached plan");

    Require(!descriptor_cache.Lookup(descriptor_key).cache_hit,
            "ODF-117 NoSQL compaction reused stale descriptor snapshot");
    const auto stale_hot = hot_cache.Lookup(hot_key);
    Require(!stale_hot.cache_hit &&
                stale_hot.diagnostic_code ==
                    "SB_INDEX_HOT_POINT_LOOKUP_CACHE_DEPENDENCY_INVALIDATED",
            "ODF-117 NoSQL compaction reused stale DML hot-point cache");
  }
}

}  // namespace

int main() {
  DirectStaleEpochAndSecurityFailures();
  NoSqlGenerationAndCompactionFailures();
  NoSqlOptimizerInvalidationVocabulary();
  ConcurrentDdlDmlNoSqlInvalidationStress();
  return 0;
}
