// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hot_point_lookup_cache.hpp"
#include "dml/dml_target_access_plan.hpp"
#include "optimizer_hot_point_lookup.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace dml = scratchbird::engine::internal_api;
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

platform::TypedUuid TestUuid(platform::UuidKind kind, unsigned char salt) {
  platform::TypedUuid uuid;
  uuid.kind = kind;
  uuid.value.bytes[0] = 0x01;
  uuid.value.bytes[1] = 0x9f;
  uuid.value.bytes[6] = 0x70;
  uuid.value.bytes[8] = 0x80;
  uuid.value.bytes[14] = 0x56;
  uuid.value.bytes[15] = salt;
  return uuid;
}

std::string TestUuidText(platform::UuidKind kind, unsigned char salt) {
  return uuid::UuidToString(TestUuid(kind, salt).value);
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view expected) {
  for (const auto& value : evidence) {
    if (value == expected) {
      return true;
    }
  }
  return false;
}

bool EvidenceContains(const std::vector<std::string>& evidence,
                      std::string_view expected) {
  for (const auto& value : evidence) {
    if (value.find(expected) != std::string::npos) {
      return true;
    }
  }
  return false;
}

idx::HotPointLookupCacheKey BaseKey(idx::HotPointProbeClass probe_class,
                                    unsigned char salt = 0x10) {
  idx::HotPointLookupCacheKey key;
  key.probe_class = probe_class;
  key.database_uuid = TestUuid(platform::UuidKind::database, 0x01);
  key.object_uuid = TestUuid(platform::UuidKind::object, 0x02);
  key.index_uuid = TestUuid(platform::UuidKind::object, 0x03);
  key.encoded_probe_key = "encoded-point-key-" + std::to_string(salt);
  key.statistics_snapshot_id = "stats.snapshot.56";
  key.descriptor_set_digest = "descriptor.set.56";
  key.index_definition_digest = "index.definition.56";
  key.security_policy_digest = "security.policy.56";
  key.redaction_policy_digest = "redaction.policy.56";
  key.access_policy_digest = "access.policy.56";
  key.collation_profile_digest = "collation.profile.56";
  key.catalog_epoch = 101;
  key.index_epoch = 202;
  key.statistics_epoch = 303;
  key.security_epoch = 404;
  key.policy_epoch = 505;
  key.object_epoch = 606;
  key.compatibility_epoch = 707;
  return key;
}

idx::HotPointLookupCandidate Candidate(unsigned char salt) {
  idx::HotPointLookupCandidate candidate;
  candidate.locator.table_uuid = TestUuid(platform::UuidKind::object, 0x02);
  candidate.locator.row_uuid = TestUuid(platform::UuidKind::row, salt);
  candidate.locator.version_uuid =
      TestUuid(platform::UuidKind::row, static_cast<unsigned char>(salt + 0x20));
  candidate.locator.local_transaction_id = 900 + salt;
  candidate.proof_kind = "candidate_locator";
  candidate.posting_list_digest = "posting.digest.56";
  candidate.candidate_locator_only = true;
  candidate.equality_proof_metadata_only = true;
  candidate.requires_mga_visibility_recheck = true;
  candidate.requires_security_authorization_recheck = true;
  return candidate;
}

idx::HotPointLookupCacheEntry Entry(const idx::HotPointLookupCacheKey& key,
                                    unsigned char salt = 0x31) {
  idx::HotPointLookupCacheEntry entry;
  entry.key = key;
  entry.candidates.push_back(Candidate(salt));
  entry.dependency_uuids = {
      key.object_uuid,
      key.index_uuid,
      TestUuid(platform::UuidKind::object, 0x66),
  };
  entry.created_epoch = key.catalog_epoch;
  return entry;
}

std::vector<std::string> RuntimeValues(
    const idx::HotPointLookupCacheResult& result) {
  auto values = result.evidence;
  values.push_back(result.diagnostic_code);
  values.push_back(result.cache_key);
  if (result.entry.has_value()) {
    values.push_back(result.entry->invalidation_diagnostic_code);
    values.push_back(result.entry->invalidation_event_kind);
    values.push_back(result.entry->invalidation_dependency_uuid);
    for (const auto& candidate : result.entry->candidates) {
      values.push_back(candidate.proof_kind);
      values.push_back(candidate.posting_list_digest);
    }
  }
  return values;
}

void RequireNoRuntimeDocTokens(const std::vector<std::string>& values) {
  for (const auto& value : values) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references"}) {
      Require(value.find(forbidden) == std::string::npos,
              "ODF-056 runtime evidence leaked documentation token");
    }
  }
}

void RowUuidHitMissAndKeyComponents() {
  idx::AdaptiveHotPointLookupCache cache({4, 8, 8, 32});
  const auto key = BaseKey(idx::HotPointProbeClass::row_uuid_lookup);

  const auto miss = cache.Lookup(key);
  Require(!miss.cache_hit &&
              miss.diagnostic_code == "SB_INDEX_HOT_POINT_LOOKUP_CACHE_MISS",
          "ODF-056 row UUID empty cache did not miss");

  const auto put = cache.Put(Entry(key));
  Require(put.ok && put.admitted, "ODF-056 row UUID cache admission failed");
  Require(put.cache_key.find("catalog_epoch=101") != std::string::npos &&
              put.cache_key.find("index_epoch=202") != std::string::npos &&
              put.cache_key.find("statistics_epoch=303") != std::string::npos &&
              put.cache_key.find("security_epoch=404") != std::string::npos &&
              put.cache_key.find("policy_epoch=505") != std::string::npos &&
              put.cache_key.find("security.policy.56") != std::string::npos,
          "ODF-056 cache key omitted epoch/security/policy components");

  const auto hit = cache.Lookup(key);
  Require(hit.ok && hit.cache_hit &&
              hit.diagnostic_code == "SB_INDEX_HOT_POINT_LOOKUP_CACHE_HIT",
          "ODF-056 row UUID cache did not hit");
  Require(hit.entry.has_value() && hit.entry->candidates.size() == 1,
          "ODF-056 hit did not return candidate locator metadata");
  Require(EvidenceHas(hit.evidence, "mga_visibility_recheck=required") &&
              EvidenceHas(hit.evidence,
                          "security_authorization_recheck=required") &&
              EvidenceHas(hit.evidence,
                          "cache_visibility_finality_authority=false"),
          "ODF-056 hit evidence did not preserve MGA/security recheck");
}

void ProbeClassesAndOptimizerDecisionSurface() {
  for (const auto probe_class : {
           idx::HotPointProbeClass::row_uuid_lookup,
           idx::HotPointProbeClass::unique_index_lookup,
           idx::HotPointProbeClass::fk_parent_existence_lookup,
           idx::HotPointProbeClass::conflict_preflight_lookup,
       }) {
    idx::AdaptiveHotPointLookupCache cache({4, 8, 8, 32});
    const auto key = BaseKey(
        probe_class,
        static_cast<unsigned char>(
            0x20 + static_cast<unsigned>(probe_class)));
    Require(cache.Put(Entry(key)).admitted,
            "ODF-056 probe-class admission failed");
    const auto hit = cache.Lookup(key);
    Require(hit.cache_hit, "ODF-056 probe class did not hit");
    Require(EvidenceContains(hit.evidence, idx::HotPointProbeClassName(probe_class)),
            "ODF-056 probe class evidence missing");

    const auto decision =
        opt::PlanOptimizerHotPointLookup(probe_class, true, true, true);
    Require(decision.lookup_allowed && decision.admission_allowed,
            "ODF-056 optimizer hot-point decision did not allow safe point probe");
    Require(!decision.cache_finality_authority &&
                EvidenceHas(decision.evidence,
                            "cache_visibility_finality_authority=false"),
            "ODF-056 optimizer decision exposed finality authority");
  }

  const auto refused = opt::PlanOptimizerHotPointLookup(
      idx::HotPointProbeClass::unique_index_lookup, true, false, true);
  Require(!refused.lookup_allowed &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_HOT_POINT_LOOKUP_CACHE_AUTHORITY_REFUSED",
          "ODF-056 optimizer decision accepted missing MGA recheck");
}

void StaleEpochRefusalFailsClosed() {
  idx::AdaptiveHotPointLookupCache cache({4, 8, 8, 32});
  const auto key = BaseKey(idx::HotPointProbeClass::unique_index_lookup);
  Require(cache.Put(Entry(key)).admitted,
          "ODF-056 stale epoch setup admission failed");

  auto stale = key;
  stale.catalog_epoch += 1;
  const auto result = cache.Lookup(stale);
  Require(!result.cache_hit &&
              result.diagnostic_code ==
                  "SB_INDEX_HOT_POINT_LOOKUP_CACHE_STALE_EPOCH",
          "ODF-056 stale epoch did not fail closed");
  Require(EvidenceHas(result.evidence, "epoch_compatibility=false"),
          "ODF-056 stale epoch evidence missing");
}

void DependencyInvalidationReportsExactEvidence() {
  idx::AdaptiveHotPointLookupCache cache({4, 8, 8, 32});
  const auto key = BaseKey(idx::HotPointProbeClass::fk_parent_existence_lookup);
  Require(cache.Put(Entry(key)).admitted,
          "ODF-056 dependency invalidation setup failed");

  idx::HotPointLookupInvalidationEvent event;
  event.event_kind = "dependency_invalidation";
  event.dependency_uuid = TestUuid(platform::UuidKind::object, 0x66);
  const auto invalidated = cache.Invalidate(event);
  Require(invalidated.invalidated_count == 1,
          "ODF-056 dependency invalidation count mismatch");
  Require(EvidenceHas(invalidated.evidence,
                      "invalidation_kind=dependency_invalidation"),
          "ODF-056 invalidation event evidence missing");

  const auto lookup = cache.Lookup(key);
  Require(!lookup.cache_hit &&
              lookup.diagnostic_code ==
                  "SB_INDEX_HOT_POINT_LOOKUP_CACHE_DEPENDENCY_INVALIDATED",
          "ODF-056 invalidated entry did not refuse reuse");
  Require(EvidenceContains(lookup.evidence,
                           "hot_point_lookup_cache_dependency_invalidation"),
          "ODF-056 dependency invalidation lookup evidence missing");
}

void PartitionCountersContentionDisableAndReset() {
  idx::AdaptiveHotPointLookupCache cache({4, 2, 8, 32});
  const auto key = BaseKey(idx::HotPointProbeClass::conflict_preflight_lookup);
  Require(cache.Put(Entry(key)).admitted,
          "ODF-056 contention setup admission failed");
  const auto partition = cache.PartitionForKey(key);

  Require(cache.Lookup(key).cache_hit, "ODF-056 counter setup hit failed");
  const auto before = cache.PartitionCounters(partition);
  Require(before.puts == 1 && before.hits == 1 && before.entry_count == 1,
          "ODF-056 partition counters did not record put/hit");

  cache.RecordContentionRefusal(key);
  const auto disabled = cache.RecordContentionRefusal(key);
  Require(disabled.diagnostic_code ==
              "SB_INDEX_HOT_POINT_LOOKUP_CACHE_PARTITION_AUTO_DISABLED",
          "ODF-056 contention threshold did not auto-disable partition");
  const auto disabled_lookup = cache.Lookup(key);
  Require(!disabled_lookup.cache_hit &&
              disabled_lookup.diagnostic_code ==
                  "SB_INDEX_HOT_POINT_LOOKUP_CACHE_PARTITION_AUTO_DISABLED",
          "ODF-056 disabled partition did not fail open to uncached lookup");
  const auto disabled_counters = cache.PartitionCounters(partition);
  Require(disabled_counters.auto_disabled &&
              disabled_counters.contention_refusals >= 2,
          "ODF-056 contention counters missing");

  const auto reset = cache.ResetPartition(partition);
  Require(reset.ok && EvidenceHas(reset.evidence, "lookup_cache_reenabled=true"),
          "ODF-056 partition reset did not re-enable cache");
  Require(!cache.PartitionCounters(partition).auto_disabled,
          "ODF-056 partition remained disabled after reset");
  Require(cache.Put(Entry(key)).admitted,
          "ODF-056 admission after partition reset failed");
  Require(cache.Lookup(key).cache_hit,
          "ODF-056 lookup after partition reset did not hit");
}

void AuthorityRefusalCanAutoDisableButNeverProvesFinality() {
  idx::AdaptiveHotPointLookupCache cache({4, 8, 2, 32});
  auto key = BaseKey(idx::HotPointProbeClass::unique_index_lookup);
  auto unsafe = Entry(key);
  unsafe.candidates.front().requires_mga_visibility_recheck = false;
  unsafe.candidates.front().visibility_finality_authority = true;

  const auto refused_one = cache.Put(unsafe);
  const auto refused_two = cache.Put(unsafe);
  Require(!refused_one.admitted && !refused_two.admitted &&
              refused_one.diagnostic_code ==
                  "SB_INDEX_HOT_POINT_LOOKUP_CACHE_AUTHORITY_REFUSED",
          "ODF-056 unsafe finality candidate was admitted");
  Require(EvidenceHas(refused_one.evidence,
                      "cache_visibility_finality_authority=false"),
          "ODF-056 authority refusal did not record cache non-authority");

  const auto disabled = cache.Lookup(key);
  Require(disabled.diagnostic_code ==
              "SB_INDEX_HOT_POINT_LOOKUP_CACHE_PARTITION_AUTO_DISABLED",
          "ODF-056 authority refusal threshold did not disable partition");
  const auto counters = cache.PartitionCounters(cache.PartitionForKey(key));
  Require(counters.authority_refusals >= 2 && counters.auto_disabled,
          "ODF-056 authority refusal counters missing");
}

void RuntimeEvidenceHasNoDocumentationTokens() {
  idx::AdaptiveHotPointLookupCache cache({4, 4, 4, 32});
  const auto key = BaseKey(idx::HotPointProbeClass::row_uuid_lookup);
  std::vector<std::string> values;

  auto append = [&values](const idx::HotPointLookupCacheResult& result) {
    auto result_values = RuntimeValues(result);
    values.insert(values.end(), result_values.begin(), result_values.end());
  };

  append(cache.Lookup(key));
  append(cache.Put(Entry(key)));
  append(cache.Lookup(key));
  idx::HotPointLookupInvalidationEvent event;
  event.event_kind = "index_change";
  event.index_uuid = key.index_uuid;
  const auto invalidated = cache.Invalidate(event);
  values.push_back(invalidated.diagnostic_code);
  values.insert(values.end(), invalidated.evidence.begin(),
                invalidated.evidence.end());
  append(cache.Lookup(key));

  const auto decision = opt::PlanOptimizerHotPointLookup(
      idx::HotPointProbeClass::row_uuid_lookup, true, true, true);
  values.push_back(decision.diagnostic_code);
  values.insert(values.end(), decision.evidence.begin(), decision.evidence.end());

  RequireNoRuntimeDocTokens(values);
}

void DmlTargetAccessUsesLiveHotPointCacheRoute() {
  dml::DmlTargetAccessPlanRequest row_request;
  row_request.mutation_kind = "dml.update_rows";
  row_request.database_uuid = TestUuidText(platform::UuidKind::database, 0x41);
  row_request.relation_uuid = TestUuidText(platform::UuidKind::object, 0x42);
  row_request.predicate_kind = "row_uuid_match";
  row_request.predicate_descriptor_digest = "row.uuid.digest.56";
  row_request.row_uuid = TestUuidText(platform::UuidKind::row, 0x43);
  row_request.access_descriptor_present = true;
  row_request.security_policy_digest = "security.policy.56";
  row_request.redaction_policy_digest = "redaction.policy.56";
  row_request.access_policy_digest = "access.policy.56";
  row_request.collation_profile_digest = "collation.profile.56";
  row_request.observed_catalog_epoch = 1001;
  row_request.current_catalog_epoch = 1001;
  row_request.observed_security_epoch = 1002;
  row_request.current_security_epoch = 1002;
  row_request.observed_policy_epoch = 1003;
  row_request.current_policy_epoch = 1003;
  row_request.observed_stats_epoch = 1004;
  row_request.current_stats_epoch = 1004;
  row_request.index_epoch = 1005;
  row_request.object_epoch = 1006;
  row_request.compatibility_epoch = 1007;
  row_request.local_transaction_id = 1008;
  row_request.estimated_rows = 1;

  const auto row_first = dml::BuildDmlTargetAccessPlan(row_request);
  Require(row_first.ok &&
              row_first.access_kind == dml::DmlTargetAccessKind::row_uuid_singleton,
          "ODF-056 DML row UUID target access was not accepted");
  Require(EvidenceContains(row_first.evidence,
                           "hot_point_lookup_cache_probe_class=row_uuid_lookup"),
          "ODF-056 DML row UUID route did not reach hot-point cache");
  Require(EvidenceContains(row_first.evidence,
                           "hot_point_lookup_cache_lookup=miss") &&
              EvidenceContains(row_first.evidence,
                               "hot_point_lookup_cache_admitted=true"),
          "ODF-056 DML row UUID route did not admit a cacheable point probe");

  const auto row_second = dml::BuildDmlTargetAccessPlan(row_request);
  Require(EvidenceContains(row_second.evidence,
                           "hot_point_lookup_cache_lookup=hit"),
          "ODF-056 DML row UUID route did not reuse the live cache");
  Require(EvidenceContains(row_second.evidence,
                           "hot_point_lookup_cache_evidence=mga_visibility_recheck=required") &&
              EvidenceContains(row_second.evidence,
                               "hot_point_lookup_cache_evidence=security_authorization_recheck=required"),
          "ODF-056 DML cache hit did not preserve MGA/security rechecks");

  dml::DmlTargetAccessPlanRequest unique_request = row_request;
  unique_request.mutation_kind = "dml.delete_rows";
  unique_request.predicate_kind = "unique_eq";
  unique_request.predicate_descriptor_digest = "unique.digest.56";
  unique_request.row_uuid.clear();
  unique_request.index_uuid = TestUuidText(platform::UuidKind::object, 0x44);
  unique_request.index_family = "btree";
  unique_request.index_unique = true;
  unique_request.compatibility_epoch = 2007;
  const auto unique_plan = dml::BuildDmlTargetAccessPlan(unique_request);
  Require(unique_plan.ok &&
              unique_plan.access_kind == dml::DmlTargetAccessKind::unique_index_lookup,
          "ODF-056 DML unique target access was not accepted");
  Require(EvidenceContains(unique_plan.evidence,
                           "hot_point_lookup_cache_probe_class=unique_index_lookup"),
          "ODF-056 DML unique route did not reach hot-point cache");
  Require(EvidenceContains(unique_plan.evidence,
                           "hot_point_lookup_cache_decision=SB_OPTIMIZER_HOT_POINT_LOOKUP_CACHE_ALLOWED"),
          "ODF-056 DML unique route did not use the optimizer hot-point decision");
}

}  // namespace

int main() {
  RowUuidHitMissAndKeyComponents();
  ProbeClassesAndOptimizerDecisionSurface();
  StaleEpochRefusalFailsClosed();
  DependencyInvalidationReportsExactEvidence();
  PartitionCountersContentionDisableAndReset();
  AuthorityRefusalCanAutoDisableButNeverProvesFinality();
  RuntimeEvidenceHasNoDocumentationTokens();
  DmlTargetAccessUsesLiveHotPointCacheRoute();
  return 0;
}
