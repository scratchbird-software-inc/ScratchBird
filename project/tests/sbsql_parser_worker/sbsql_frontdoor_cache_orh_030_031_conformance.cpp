// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cache/sblr_template_cache.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

bool Contains(std::string_view value, std::string_view needle) {
  return value.find(needle) != std::string_view::npos;
}

sbsql::CacheKey BaseKey(std::uint64_t shape_hash = 1001) {
  sbsql::CacheKey key;
  key.shape_hash = shape_hash;
  key.registry_version = 7;
  key.catalog_epoch = 11;
  key.security_policy_epoch = 13;
  key.grant_epoch = 17;
  key.descriptor_epoch = 19;
  key.udr_epoch = 23;
  key.name_resolution_epoch = 29;
  key.resource_epoch = 31;
  key.parser_package_generation = 37;
  key.protocol_version = 1;
  key.parser_package_version_hash = 41;
  key.disclosure_policy_generation = 43;
  key.redaction_policy_generation = 47;
  key.security_authority_epoch = 53;
  key.cluster_policy_generation = 59;
  key.ttl_generation = 61;
  key.memory_pressure_generation = 67;
  key.normalized_statement_hash = 71 + shape_hash;
  key.parameter_type_shape_hash = 73;
  key.connection_uuid = "connection/orh-cache";
  key.transaction_context_hash = "txn/read_only_prepare";
  key.dialect = "sbsql";
  key.role_set_hash = "roles/app_reader";
  key.group_set_hash = "groups/reporting";
  key.search_path_hash = "search/public";
  key.language_profile = "en-US";
  key.language_tag = "en-US";
  key.input_syntax_profile = "sbsql.syntax.standard";
  key.policy_profile = "policy/default";
  key.parser_profile = "parser/default";
  key.common_resource_hash = "common.hash.en-US";
  key.language_resource_epoch = 31;
  key.localized_name_epoch = 29;
  key.message_resource_epoch = 37;
  key.resource_compatibility_identity = "sbsql.resource.compat.v1";
  key.resource_version_identity = "sbsql.resource-pack.v1";
  key.result_contract_hash = "result/default";
  return key;
}

sbsql::CacheEntry EntryFor(sbsql::CacheKey key, std::string payload) {
  sbsql::CacheEntry entry;
  entry.key = std::move(key);
  entry.sblr_payload = std::move(payload);
  entry.statement_family = "query";
  entry.operation_family = "query.evaluate_projection";
  entry.statement_hash = entry.key.normalized_statement_hash;
  return entry;
}

void VerifyAuthorityRefusalField(std::string_view label,
                                 std::string_view expected_code,
                                 void (*mark)(sbsql::CacheEntry*)) {
  sbsql::SblrTemplateCache cache(8);
  auto key = BaseKey();
  auto entry = EntryFor(key, "payload/refused");
  mark(&entry);

  const auto result = cache.StoreEntry(std::move(entry));
  Require(!result.stored, std::string(label) + " authority entry was stored");
  Require(result.diagnostic_code == expected_code,
          std::string(label) + " refusal diagnostic changed: " +
              result.diagnostic_code);
  Require(Contains(result.diagnostic_detail, label),
          std::string(label) + " diagnostic detail does not name field");
  Require(Contains(result.diagnostic_detail, "parser cache cannot own storage visibility authorization finality rollback commit transaction inventory or recovery authority"),
          std::string(label) + " diagnostic detail lost MGA authority boundary");
  Require(result.stable_key == key.StableKey(),
          std::string(label) + " refusal did not preserve stable diagnostic key");
  Require(result.compact_key == key.CompactKey(),
          std::string(label) + " refusal did not preserve compact key");
  Require(!cache.LookupEntry(key).has_value(),
          std::string(label) + " refused authority entry was retrievable");
  Require(cache.Size() == 0, std::string(label) + " refusal changed cache size");
}

void VerifyAuthorityRefusals() {
  VerifyAuthorityRefusalField(
      "parser_executes_sql",
      "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.PARSER_EXECUTES_SQL",
      [](sbsql::CacheEntry* entry) { entry->parser_executes_sql = true; });
  VerifyAuthorityRefusalField(
      "storage_authority_cached",
      "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.STORAGE_AUTHORITY_CACHED",
      [](sbsql::CacheEntry* entry) { entry->storage_authority_cached = true; });
  VerifyAuthorityRefusalField(
      "visibility_authority_cached",
      "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.VISIBILITY_AUTHORITY_CACHED",
      [](sbsql::CacheEntry* entry) { entry->visibility_authority_cached = true; });
  VerifyAuthorityRefusalField(
      "authorization_authority_cached",
      "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.AUTHORIZATION_AUTHORITY_CACHED",
      [](sbsql::CacheEntry* entry) {
        entry->authorization_authority_cached = true;
      });
  VerifyAuthorityRefusalField(
      "finality_authority_cached",
      "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.FINALITY_AUTHORITY_CACHED",
      [](sbsql::CacheEntry* entry) { entry->finality_authority_cached = true; });
}

void VerifyDeterministicLruEviction() {
  sbsql::SblrTemplateCache cache(3);
  auto key_a = BaseKey(1);
  auto key_b = BaseKey(2);
  auto key_c = BaseKey(3);
  auto key_d = BaseKey(4);

  Require(cache.Store(key_a, "payload/a").stored, "entry A was not stored");
  Require(cache.Store(key_b, "payload/b").stored, "entry B was not stored");
  Require(cache.Store(key_c, "payload/c").stored, "entry C was not stored");

  const auto hot = cache.LookupEntry(key_a);
  Require(hot.has_value() && hot->sblr_payload == "payload/a",
          "hot lookup did not return entry A before pressure");
  Require(hot->hits == 1, "hot lookup did not record hit count");

  Require(cache.Store(key_d, "payload/d").stored, "entry D was not stored");
  Require(cache.Size() == 3, "cache exceeded configured capacity");
  Require(cache.Lookup(key_a).value_or("") == "payload/a",
          "hot LRU entry A was evicted under pressure");
  Require(!cache.Lookup(key_b).has_value(),
          "cold LRU entry B survived deterministic capacity pressure");
  Require(cache.Lookup(key_c).value_or("") == "payload/c",
          "entry C was unexpectedly evicted");
  Require(cache.Lookup(key_d).value_or("") == "payload/d",
          "new entry D was not retrievable");
}

void VerifyCompactKeyAndStableDiagnostics() {
  const auto key = BaseKey(91);
  const auto stable_key = key.StableKey();
  const auto compact_key = key.CompactKey();
  Require(Contains(stable_key, "sbsql-cache-v8"),
          "stable diagnostic key lost cache version");
  Require(Contains(stable_key, "connection/orh-cache"),
          "stable diagnostic key lost full connection dimension");
  Require(Contains(compact_key, "sbsql-cache-k1:"),
          "compact key lost deterministic prefix");
  Require(compact_key == key.CompactKey(),
          "compact key is not deterministic across calls");
  Require(compact_key != stable_key, "compact key reused long stable key");

  sbsql::SblrTemplateCache cache(2);
  const auto store = cache.Store(key, "payload/stable");
  Require(store.stored, "stable diagnostic entry was not stored");
  Require(store.stable_key == stable_key, "store result lost stable key");
  Require(store.compact_key == compact_key, "store result lost compact key");

  const auto snapshot = cache.SnapshotJson();
  Require(Contains(snapshot, "\"map_key\":\"compact_fnv1a64\""),
          "snapshot did not disclose compact map key strategy");
  Require(Contains(snapshot, stable_key),
          "snapshot did not retain stable diagnostic key rendering");
  Require(Contains(snapshot, "\"visibility\":false"),
          "snapshot did not report visibility authority refusal state");
}

void VerifyEpochInvalidationStillRemovesStaleEntries() {
  sbsql::SblrTemplateCache cache(4);
  auto stale = BaseKey(201);
  stale.catalog_epoch = 100;
  auto current = stale;
  current.shape_hash = 202;
  current.normalized_statement_hash = 202;
  current.catalog_epoch = 101;

  Require(cache.Store(stale, "payload/stale").stored, "stale seed not stored");
  Require(cache.Store(current, "payload/current").stored, "current seed not stored");
  cache.InvalidateCatalogEpoch(101);

  Require(!cache.Lookup(stale).has_value(),
          "catalog epoch invalidation retained stale parser cache entry");
  Require(cache.Lookup(current).value_or("") == "payload/current",
          "catalog epoch invalidation removed current parser cache entry");
}

} // namespace

int main() {
  VerifyAuthorityRefusals();
  VerifyDeterministicLruEviction();
  VerifyCompactKeyAndStableDiagnostics();
  VerifyEpochInvalidationStillRemovesStaleEntries();
  std::cout << "sbsql_frontdoor_cache_orh_030_031_conformance=passed\n";
  return EXIT_SUCCESS;
}
