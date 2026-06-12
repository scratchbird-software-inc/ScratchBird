// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/common.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace scratchbird::parser::sbsql {

struct CacheKey {
  std::uint64_t shape_hash{0};
  std::uint32_t registry_version{0};
  std::uint64_t catalog_epoch{0};
  std::uint64_t security_policy_epoch{0};
  std::uint64_t grant_epoch{0};
  std::uint64_t descriptor_epoch{0};
  std::uint64_t udr_epoch{0};
  std::uint64_t name_resolution_epoch{0};
  std::uint64_t resource_epoch{0};
  std::uint64_t parser_package_generation{0};
  std::uint32_t protocol_version{0};
  std::uint64_t parser_package_version_hash{0};
  std::uint64_t disclosure_policy_generation{0};
  std::uint64_t redaction_policy_generation{0};
  std::uint64_t security_authority_epoch{0};
  std::uint64_t cluster_policy_generation{0};
  std::uint64_t ttl_generation{0};
  std::uint64_t memory_pressure_generation{0};
  std::uint64_t normalized_statement_hash{0};
  std::uint64_t parameter_type_shape_hash{0};
  std::string connection_uuid;
  std::string transaction_context_hash;
  std::string dialect;
  std::string role_set_hash;
  std::string group_set_hash;
  std::string search_path_hash;
  std::string language_profile;
  std::string language_tag;
  std::string input_syntax_profile;
  std::string input_language_fallback_tag;
  std::string common_resource_hash;
  std::uint64_t language_resource_epoch{0};
  std::uint64_t localized_name_epoch{0};
  std::string policy_profile;
  std::string parser_profile;
  std::uint64_t message_resource_epoch{0};
  std::string resource_compatibility_identity;
  std::string resource_version_identity;
  std::string result_contract_hash;

  [[nodiscard]] std::string StableKey() const;
  [[nodiscard]] std::string CompactKey() const;
};

struct CacheEntry {
  CacheKey key;
  std::string sblr_payload;
  std::string statement_family;
  std::string operation_family;
  std::uint64_t statement_hash{0};
  bool parser_executes_sql{false};
  bool storage_authority_cached{false};
  bool visibility_authority_cached{false};
  bool authorization_authority_cached{false};
  bool finality_authority_cached{false};
  std::uint64_t hits{0};
};

struct CacheStoreResult {
  bool stored{false};
  std::string diagnostic_code;
  std::string diagnostic_detail;
  std::string stable_key;
  std::string compact_key;
};

class SblrTemplateCache {
 public:
  explicit SblrTemplateCache(std::size_t max_entries = 256);
  std::optional<std::string> Lookup(const CacheKey& key);
  std::optional<CacheEntry> LookupEntry(const CacheKey& key);
  CacheStoreResult Store(CacheKey key, std::string sblr_payload);
  CacheStoreResult StoreEntry(CacheEntry entry);
  void Flush();
  void InvalidateCatalogEpoch(std::uint64_t new_epoch);
  void InvalidateSecurityPolicyEpoch(std::uint64_t new_epoch);
  void InvalidateGrantEpoch(std::uint64_t new_epoch);
  void InvalidateDescriptorEpoch(std::uint64_t new_epoch);
  void InvalidateUdrEpoch(std::uint64_t new_epoch);
  void InvalidateNameResolutionEpoch(std::uint64_t new_epoch);
  void InvalidateResourceEpoch(std::uint64_t new_epoch);
  void InvalidateParserPackageGeneration(std::uint64_t new_generation);
  void InvalidateProtocolVersion(std::uint32_t new_protocol_version);
  void InvalidateParserPackageVersionHash(std::uint64_t new_version_hash);
  void InvalidateDisclosurePolicyGeneration(std::uint64_t new_generation);
  void InvalidateRedactionPolicyGeneration(std::uint64_t new_generation);
  void InvalidateSecurityAuthorityEpoch(std::uint64_t new_epoch);
  void InvalidateClusterPolicyGeneration(std::uint64_t new_generation);
  void InvalidateTtlGeneration(std::uint64_t new_generation);
  void InvalidateMemoryPressureGeneration(std::uint64_t new_generation);
  void InvalidateNormalizedStatementHash(std::uint64_t new_hash);
  void InvalidateParameterTypeShapeHash(std::uint64_t new_hash);
  void InvalidateConnection(std::string_view connection_uuid);
  void InvalidateTransactionContext(std::string_view transaction_context_hash);
  void InvalidateDialect(std::string_view dialect);
  void InvalidateRoleSetHash(std::string_view new_hash);
  void InvalidateGroupSetHash(std::string_view new_hash);
  void InvalidateSearchPathHash(std::string_view new_hash);
  void InvalidateLanguageProfile(std::string_view new_language_profile);
  void InvalidateLanguageTag(std::string_view new_language_tag);
  void InvalidateInputSyntaxProfile(std::string_view new_input_syntax_profile);
  void InvalidateInputLanguageFallbackTag(std::string_view new_fallback_tag);
  void InvalidateCommonResourceHash(std::string_view new_common_resource_hash);
  void InvalidateLanguageResourceEpoch(std::uint64_t new_epoch);
  void InvalidateLocalizedNameEpoch(std::uint64_t new_epoch);
  void InvalidatePolicyProfile(std::string_view new_policy_profile);
  void InvalidateParserProfile(std::string_view new_parser_profile);
  void InvalidateMessageResourceEpoch(std::uint64_t new_epoch);
  void InvalidateResourceCompatibilityIdentity(std::string_view new_identity);
  void InvalidateResourceVersionIdentity(std::string_view new_identity);
  void InvalidateRegistryVersion(std::uint32_t new_registry_version);
  void InvalidateResultContractHash(std::string_view new_result_contract_hash);
  [[nodiscard]] std::size_t Size() const;
  [[nodiscard]] std::string SnapshotJson() const;

 private:
  struct CacheRecord {
    CacheEntry entry;
    std::string stable_key;
    std::list<std::string>::iterator lru_position;
  };

  void EraseMatchingLocked(
      const std::function<bool(const CacheKey&)>& predicate);
  void TouchLocked(std::unordered_map<std::string, CacheRecord>::iterator it);
  void EvictOneLocked();

  std::size_t max_entries_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, CacheRecord> entries_;
  std::list<std::string> lru_order_;
  std::uint64_t invalidation_count_{0};
};

} // namespace scratchbird::parser::sbsql
