// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hot_point_lookup_cache.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

namespace {

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

platform::TypedUuid Typed(platform::UuidKind kind, std::string_view text) {
  auto parsed = uuid::ParseDurableEngineIdentityUuid(kind, std::string(text));
  Require(parsed.ok(), "failed to parse typed durable UUID");
  return parsed.value;
}

idx::HotPointLookupCacheKey Key() {
  idx::HotPointLookupCacheKey key;
  key.probe_class = idx::HotPointProbeClass::row_uuid_lookup;
  key.database_uuid = Typed(platform::UuidKind::database,
                            "019f2200-0000-7000-8000-000000050001");
  key.object_uuid = Typed(platform::UuidKind::object,
                          "019f2200-0000-7000-8000-000000050002");
  key.encoded_probe_key = "ORH-050:row_uuid_lookup:actual_locator";
  key.statistics_snapshot_id = "stats_epoch:1";
  key.descriptor_set_digest = "descriptor:orh050";
  key.index_definition_digest = "index:none";
  key.security_policy_digest = "security_epoch:1";
  key.redaction_policy_digest = "redaction_epoch:1";
  key.access_policy_digest = "access_epoch:1";
  key.collation_profile_digest = "collation:sbsql_v3";
  key.catalog_epoch = 1;
  key.index_epoch = 1;
  key.statistics_epoch = 1;
  key.security_epoch = 1;
  key.policy_epoch = 1;
  key.object_epoch = 1;
  key.compatibility_epoch = 1;
  return key;
}

idx::HotPointLookupCacheEntry Entry(bool include_row_uuid) {
  idx::HotPointLookupCacheEntry entry;
  entry.key = Key();
  idx::HotPointLookupCandidate candidate;
  candidate.locator.table_uuid = entry.key.object_uuid;
  if (include_row_uuid) {
    candidate.locator.row_uuid = Typed(platform::UuidKind::row,
                                       "019f2200-0000-7000-8000-000000050003");
  }
  candidate.locator.local_transaction_id = 42;
  candidate.proof_kind = "orh050_actual_successful_row_locator";
  candidate.posting_list_digest = "posting:actual-success";
  candidate.candidate_locator_only = true;
  candidate.equality_proof_metadata_only = true;
  candidate.requires_mga_visibility_recheck = true;
  candidate.requires_security_authorization_recheck = true;
  candidate.visibility_finality_authority = false;
  candidate.authorization_finality_authority = false;
  candidate.parser_or_reference_finality_authority = false;
  candidate.timestamp_or_uuid_order_finality_authority = false;
  entry.candidates.push_back(candidate);
  entry.dependency_uuids.push_back(entry.key.object_uuid);
  entry.created_epoch = entry.key.catalog_epoch;
  return entry;
}

bool EvidenceContains(const idx::HotPointLookupCacheResult& result,
                      std::string_view needle) {
  for (const auto& evidence : result.evidence) {
    if (evidence.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  idx::AdaptiveHotPointLookupCache cache;

  auto empty_locator = cache.Put(Entry(false));
  Require(!empty_locator.admitted,
          "ORH-050 empty row UUID locator was admitted");
  Require(empty_locator.diagnostic_code ==
              "SB_INDEX_HOT_POINT_LOOKUP_CACHE_AUTHORITY_REFUSED",
          "ORH-050 empty locator refusal diagnostic mismatch");

  auto admitted = cache.Put(Entry(true));
  Require(admitted.admitted, "ORH-050 actual row locator was not admitted");

  auto lookup = cache.Lookup(Key());
  Require(lookup.cache_hit, "ORH-050 admitted locator did not produce cache hit");
  Require(EvidenceContains(lookup, "mga_visibility_recheck=required"),
          "ORH-050 cache hit missing MGA recheck evidence");
  Require(EvidenceContains(lookup, "security_authorization_recheck=required"),
          "ORH-050 cache hit missing security recheck evidence");
  Require(EvidenceContains(lookup, "cache_visibility_finality_authority=false"),
          "ORH-050 cache hit claimed finality authority");

  auto authority_bearing = Entry(true);
  authority_bearing.key.encoded_probe_key = "ORH-050:authority-bearing";
  authority_bearing.candidates.front().visibility_finality_authority = true;
  auto refused = cache.Put(std::move(authority_bearing));
  Require(!refused.admitted,
          "ORH-050 authority-bearing locator was admitted");
  Require(refused.diagnostic_code ==
              "SB_INDEX_HOT_POINT_LOOKUP_CACHE_AUTHORITY_REFUSED",
          "ORH-050 authority-bearing refusal diagnostic mismatch");

  std::cout << "optimizer_runtime_hot_path_orh_050_cache_gate=passed\n";
  return EXIT_SUCCESS;
}
