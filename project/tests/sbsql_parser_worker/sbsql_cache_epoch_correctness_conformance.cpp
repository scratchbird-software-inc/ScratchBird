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

sbsql::CacheKey BaseKey() {
  sbsql::CacheKey key;
  key.shape_hash = 1001;
  key.registry_version = 3;
  key.catalog_epoch = 11;
  key.security_policy_epoch = 22;
  key.grant_epoch = 23;
  key.descriptor_epoch = 33;
  key.udr_epoch = 44;
  key.name_resolution_epoch = 55;
  key.resource_epoch = 66;
  key.parser_package_generation = 77;
  key.protocol_version = 1;
  key.parser_package_version_hash = 78;
  key.disclosure_policy_generation = 88;
  key.redaction_policy_generation = 89;
  key.security_authority_epoch = 99;
  key.cluster_policy_generation = 111;
  key.ttl_generation = 122;
  key.memory_pressure_generation = 133;
  key.normalized_statement_hash = 144;
  key.parameter_type_shape_hash = 155;
  key.connection_uuid = "connection/abc";
  key.transaction_context_hash = "txn/read_committed";
  key.dialect = "sbsql";
  key.role_set_hash = "roles/app_reader";
  key.group_set_hash = "groups/reporting";
  key.search_path_hash = "public_hash";
  key.language_profile = "en-US";
  key.language_tag = "en-US";
  key.common_resource_hash = "common.hash.en-US";
  key.policy_profile = "policy/default";
  key.parser_profile = "sbsql/default";
  key.message_resource_epoch = 166;
  key.result_contract_hash = "result/default";
  return key;
}

void VerifyKeyDimensions() {
  const auto key = BaseKey();
  const auto stable = key.StableKey();
  for (const auto token : {"sbsql-cache-v7", "1001", "3", "11", "22", "23", "33", "44",
                           "55", "66", "77", "1", "78", "88", "89", "99", "111",
                           "122", "133", "144", "155", "166",
                           "connection/abc", "txn/read_committed", "sbsql",
                           "roles/app_reader", "groups/reporting",
                           "public_hash", "en-US", "common.hash.en-US", "policy/default",
                           "sbsql/default", "result/default"}) {
    Require(Contains(stable, token), std::string("stable key missing ") + token);
  }
}

template <typename Mutator>
void VerifyDimensionMiss(std::string_view label, Mutator mutator) {
  sbsql::SblrTemplateCache cache(16);
  auto key = BaseKey();
  cache.Store(key, "trusted-sblr");
  auto changed = key;
  mutator(changed);
  Require(!cache.Lookup(changed).has_value(),
          std::string(label) + " change reused stale SBLR payload");
}

template <typename Mutator, typename Invalidator>
void VerifyInvalidation(std::string_view label, Mutator mutator, Invalidator invalidator) {
  sbsql::SblrTemplateCache cache(16);
  auto stale = BaseKey();
  auto current = stale;
  mutator(current);
  cache.Store(stale, "stale-sblr");
  cache.Store(current, "current-sblr");
  invalidator(cache, current);
  Require(!cache.Lookup(stale).has_value(),
          std::string(label) + " invalidation retained stale entry");
  const auto current_payload = cache.Lookup(current);
  Require(current_payload.has_value() && *current_payload == "current-sblr",
          std::string(label) + " invalidation removed current entry");
}

void VerifyLookupMisses() {
  VerifyDimensionMiss("catalog epoch", [](sbsql::CacheKey& key) { key.catalog_epoch = 12; });
  VerifyDimensionMiss("registry version", [](sbsql::CacheKey& key) { key.registry_version = 4; });
  VerifyDimensionMiss("security epoch", [](sbsql::CacheKey& key) { key.security_policy_epoch = 23; });
  VerifyDimensionMiss("grant epoch", [](sbsql::CacheKey& key) { key.grant_epoch = 24; });
  VerifyDimensionMiss("descriptor epoch", [](sbsql::CacheKey& key) { key.descriptor_epoch = 34; });
  VerifyDimensionMiss("UDR epoch", [](sbsql::CacheKey& key) { key.udr_epoch = 45; });
  VerifyDimensionMiss("name resolution epoch", [](sbsql::CacheKey& key) { key.name_resolution_epoch = 56; });
  VerifyDimensionMiss("resource epoch", [](sbsql::CacheKey& key) { key.resource_epoch = 67; });
  VerifyDimensionMiss("parser package generation", [](sbsql::CacheKey& key) { key.parser_package_generation = 78; });
  VerifyDimensionMiss("protocol version", [](sbsql::CacheKey& key) { key.protocol_version = 2; });
  VerifyDimensionMiss("parser package version", [](sbsql::CacheKey& key) { key.parser_package_version_hash = 79; });
  VerifyDimensionMiss("disclosure policy generation", [](sbsql::CacheKey& key) { key.disclosure_policy_generation = 89; });
  VerifyDimensionMiss("redaction policy generation", [](sbsql::CacheKey& key) { key.redaction_policy_generation = 90; });
  VerifyDimensionMiss("security authority epoch", [](sbsql::CacheKey& key) { key.security_authority_epoch = 100; });
  VerifyDimensionMiss("cluster policy generation", [](sbsql::CacheKey& key) { key.cluster_policy_generation = 112; });
  VerifyDimensionMiss("TTL generation", [](sbsql::CacheKey& key) { key.ttl_generation = 123; });
  VerifyDimensionMiss("memory pressure generation", [](sbsql::CacheKey& key) { key.memory_pressure_generation = 134; });
  VerifyDimensionMiss("normalized statement", [](sbsql::CacheKey& key) { key.normalized_statement_hash = 145; });
  VerifyDimensionMiss("parameter type shape", [](sbsql::CacheKey& key) { key.parameter_type_shape_hash = 156; });
  VerifyDimensionMiss("connection", [](sbsql::CacheKey& key) { key.connection_uuid = "connection/def"; });
  VerifyDimensionMiss("transaction context", [](sbsql::CacheKey& key) { key.transaction_context_hash = "txn/snapshot"; });
  VerifyDimensionMiss("dialect", [](sbsql::CacheKey& key) { key.dialect = "postgres"; });
  VerifyDimensionMiss("role set", [](sbsql::CacheKey& key) { key.role_set_hash = "roles/app_writer"; });
  VerifyDimensionMiss("group set", [](sbsql::CacheKey& key) { key.group_set_hash = "groups/admin"; });
  VerifyDimensionMiss("search path", [](sbsql::CacheKey& key) { key.search_path_hash = "private_hash"; });
  VerifyDimensionMiss("language", [](sbsql::CacheKey& key) { key.language_profile = "fr-CA"; });
  VerifyDimensionMiss("language tag", [](sbsql::CacheKey& key) { key.language_tag = "fr-CA"; });
  VerifyDimensionMiss("common resource hash", [](sbsql::CacheKey& key) { key.common_resource_hash = "common.hash.fr-CA"; });
  VerifyDimensionMiss("policy profile", [](sbsql::CacheKey& key) { key.policy_profile = "policy/restricted"; });
  VerifyDimensionMiss("parser profile", [](sbsql::CacheKey& key) { key.parser_profile = "postgres/profile"; });
  VerifyDimensionMiss("message resource epoch", [](sbsql::CacheKey& key) { key.message_resource_epoch = 167; });
  VerifyDimensionMiss("result contract", [](sbsql::CacheKey& key) { key.result_contract_hash = "result/json"; });
}

void VerifyInvalidations() {
  VerifyInvalidation("catalog epoch",
                     [](sbsql::CacheKey& key) { key.catalog_epoch = 12; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateCatalogEpoch(key.catalog_epoch);
                     });
  VerifyInvalidation("registry version",
                     [](sbsql::CacheKey& key) { key.registry_version = 4; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateRegistryVersion(key.registry_version);
                     });
  VerifyInvalidation("security epoch",
                     [](sbsql::CacheKey& key) { key.security_policy_epoch = 23; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateSecurityPolicyEpoch(key.security_policy_epoch);
                     });
  VerifyInvalidation("grant epoch",
                     [](sbsql::CacheKey& key) { key.grant_epoch = 24; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateGrantEpoch(key.grant_epoch);
                     });
  VerifyInvalidation("descriptor epoch",
                     [](sbsql::CacheKey& key) { key.descriptor_epoch = 34; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateDescriptorEpoch(key.descriptor_epoch);
                     });
  VerifyInvalidation("UDR epoch",
                     [](sbsql::CacheKey& key) { key.udr_epoch = 45; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateUdrEpoch(key.udr_epoch);
                     });
  VerifyInvalidation("name resolution epoch",
                     [](sbsql::CacheKey& key) { key.name_resolution_epoch = 56; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateNameResolutionEpoch(key.name_resolution_epoch);
                     });
  VerifyInvalidation("resource epoch",
                     [](sbsql::CacheKey& key) { key.resource_epoch = 67; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateResourceEpoch(key.resource_epoch);
                     });
  VerifyInvalidation("parser package generation",
                     [](sbsql::CacheKey& key) { key.parser_package_generation = 78; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateParserPackageGeneration(key.parser_package_generation);
                     });
  VerifyInvalidation("protocol version",
                     [](sbsql::CacheKey& key) { key.protocol_version = 2; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateProtocolVersion(key.protocol_version);
                     });
  VerifyInvalidation("parser package version",
                     [](sbsql::CacheKey& key) { key.parser_package_version_hash = 79; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateParserPackageVersionHash(key.parser_package_version_hash);
                     });
  VerifyInvalidation("disclosure policy generation",
                     [](sbsql::CacheKey& key) { key.disclosure_policy_generation = 89; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateDisclosurePolicyGeneration(key.disclosure_policy_generation);
                     });
  VerifyInvalidation("redaction policy generation",
                     [](sbsql::CacheKey& key) { key.redaction_policy_generation = 90; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateRedactionPolicyGeneration(key.redaction_policy_generation);
                     });
  VerifyInvalidation("security authority epoch",
                     [](sbsql::CacheKey& key) { key.security_authority_epoch = 100; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateSecurityAuthorityEpoch(key.security_authority_epoch);
                     });
  VerifyInvalidation("cluster policy generation",
                     [](sbsql::CacheKey& key) { key.cluster_policy_generation = 112; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateClusterPolicyGeneration(key.cluster_policy_generation);
                     });
  VerifyInvalidation("TTL generation",
                     [](sbsql::CacheKey& key) { key.ttl_generation = 123; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateTtlGeneration(key.ttl_generation);
                     });
  VerifyInvalidation("memory pressure generation",
                     [](sbsql::CacheKey& key) { key.memory_pressure_generation = 134; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateMemoryPressureGeneration(key.memory_pressure_generation);
                     });
  VerifyInvalidation("normalized statement",
                     [](sbsql::CacheKey& key) { key.normalized_statement_hash = 145; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateNormalizedStatementHash(key.normalized_statement_hash);
                     });
  VerifyInvalidation("parameter type shape",
                     [](sbsql::CacheKey& key) { key.parameter_type_shape_hash = 156; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateParameterTypeShapeHash(key.parameter_type_shape_hash);
                     });
  VerifyInvalidation("connection",
                     [](sbsql::CacheKey& key) { key.connection_uuid = "connection/def"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateConnection(key.connection_uuid);
                     });
  VerifyInvalidation("transaction context",
                     [](sbsql::CacheKey& key) { key.transaction_context_hash = "txn/snapshot"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateTransactionContext(key.transaction_context_hash);
                     });
  VerifyInvalidation("dialect",
                     [](sbsql::CacheKey& key) { key.dialect = "postgres"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateDialect(key.dialect);
                     });
  VerifyInvalidation("role set",
                     [](sbsql::CacheKey& key) { key.role_set_hash = "roles/app_writer"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateRoleSetHash(key.role_set_hash);
                     });
  VerifyInvalidation("group set",
                     [](sbsql::CacheKey& key) { key.group_set_hash = "groups/admin"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateGroupSetHash(key.group_set_hash);
                     });
  VerifyInvalidation("search path",
                     [](sbsql::CacheKey& key) { key.search_path_hash = "private_hash"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateSearchPathHash(key.search_path_hash);
                     });
  VerifyInvalidation("language",
                     [](sbsql::CacheKey& key) { key.language_profile = "fr-CA"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateLanguageProfile(key.language_profile);
                     });
  VerifyInvalidation("language tag",
                     [](sbsql::CacheKey& key) { key.language_tag = "fr-CA"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateLanguageTag(key.language_tag);
                     });
  VerifyInvalidation("common resource hash",
                     [](sbsql::CacheKey& key) { key.common_resource_hash = "common.hash.fr-CA"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateCommonResourceHash(key.common_resource_hash);
                     });
  VerifyInvalidation("policy profile",
                     [](sbsql::CacheKey& key) { key.policy_profile = "policy/restricted"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidatePolicyProfile(key.policy_profile);
                     });
  VerifyInvalidation("parser profile",
                     [](sbsql::CacheKey& key) { key.parser_profile = "postgres/profile"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateParserProfile(key.parser_profile);
                     });
  VerifyInvalidation("message resource epoch",
                     [](sbsql::CacheKey& key) { key.message_resource_epoch = 167; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateMessageResourceEpoch(key.message_resource_epoch);
                     });
  VerifyInvalidation("result contract",
                     [](sbsql::CacheKey& key) { key.result_contract_hash = "result/json"; },
                     [](sbsql::SblrTemplateCache& cache, const sbsql::CacheKey& key) {
                       cache.InvalidateResultContractHash(key.result_contract_hash);
                     });
}

void VerifyFlushAndMetrics() {
  sbsql::SblrTemplateCache cache(4);
  cache.Store(BaseKey(), "payload");
  Require(cache.Size() == 1, "cache did not store payload before flush");
  cache.Flush();
  Require(cache.Size() == 0, "cache flush retained payload");
  const auto snapshot = cache.SnapshotJson();
  for (const auto token : {"\"invalidations\":1", "shape_hash", "catalog_epoch",
                           "registry_version",
                           "security_policy_epoch", "grant_epoch", "descriptor_epoch",
                           "udr_epoch", "name_resolution_epoch", "resource_epoch",
                           "parser_package_generation", "protocol_version",
                           "parser_package_version_hash", "disclosure_policy_generation",
                           "redaction_policy_generation",
                           "security_authority_epoch", "cluster_policy_generation",
                           "ttl_generation", "memory_pressure_generation",
                           "normalized_statement_hash", "parameter_type_shape_hash",
                           "connection_uuid", "transaction_context_hash", "dialect",
                           "role_set_hash", "group_set_hash",
                           "search_path_hash", "language_profile", "language_tag",
                           "common_resource_hash", "policy_profile", "parser_profile",
                           "message_resource_epoch",
                           "result_contract_hash"}) {
    Require(Contains(snapshot, token), std::string("cache snapshot missing ") + token);
  }
}

} // namespace

int main() {
  VerifyKeyDimensions();
  VerifyLookupMisses();
  VerifyInvalidations();
  VerifyFlushAndMetrics();
  std::cout << "sbsql_cache_epoch_correctness_conformance=passed\n";
  return EXIT_SUCCESS;
}
