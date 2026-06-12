// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cache/sblr_template_cache.hpp"

#include <array>
#include <sstream>
#include <utility>

namespace scratchbird::parser::sbsql {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t MixByte(std::uint64_t hash, std::uint8_t value) {
  hash ^= static_cast<std::uint64_t>(value);
  return hash * kFnvPrime;
}

std::uint64_t MixUint64(std::uint64_t hash, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    hash = MixByte(hash, static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
  return hash;
}

std::uint64_t MixString(std::uint64_t hash, std::string_view value) {
  hash = MixUint64(hash, static_cast<std::uint64_t>(value.size()));
  for (const unsigned char c : value) {
    hash = MixByte(hash, c);
  }
  return hash;
}

std::string CompactKeyFromHash(std::uint64_t hash) {
  return "sbsql-cache-k1:" + std::to_string(hash);
}

std::optional<CacheStoreResult> ValidateEntryForStore(
    const CacheEntry& entry,
    std::string stable_key,
    std::string compact_key) {
  struct AuthorityRefusal {
    bool active;
    const char* field;
    const char* code;
  };
  const std::array<AuthorityRefusal, 5> refusals{{
      {entry.parser_executes_sql, "parser_executes_sql",
       "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.PARSER_EXECUTES_SQL"},
      {entry.storage_authority_cached, "storage_authority_cached",
       "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.STORAGE_AUTHORITY_CACHED"},
      {entry.visibility_authority_cached, "visibility_authority_cached",
       "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.VISIBILITY_AUTHORITY_CACHED"},
      {entry.authorization_authority_cached, "authorization_authority_cached",
       "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.AUTHORIZATION_AUTHORITY_CACHED"},
      {entry.finality_authority_cached, "finality_authority_cached",
       "SB_ORH_FRONTDOOR_CACHE_AUTHORITY_REFUSAL.FINALITY_AUTHORITY_CACHED"},
  }};
  for (const auto& refusal : refusals) {
    if (!refusal.active) continue;
    CacheStoreResult result;
    result.stored = false;
    result.diagnostic_code = refusal.code;
    result.diagnostic_detail =
        std::string("parser front-door cache entry refused authority field ") +
        refusal.field + "; parser cache cannot own storage visibility "
        "authorization finality rollback commit transaction inventory or "
        "recovery authority";
    result.stable_key = std::move(stable_key);
    result.compact_key = std::move(compact_key);
    return result;
  }
  return std::nullopt;
}

} // namespace

std::string CacheKey::StableKey() const {
  std::ostringstream out;
  out << "sbsql-cache-v8:" << shape_hash << ':' << registry_version << ':'
      << catalog_epoch << ':'
      << security_policy_epoch << ':' << grant_epoch << ':' << descriptor_epoch
      << ':' << udr_epoch << ':' << name_resolution_epoch << ':' << resource_epoch
      << ':' << parser_package_generation << ':' << protocol_version
      << ':' << parser_package_version_hash << ':' << disclosure_policy_generation
      << ':' << redaction_policy_generation
      << ':' << security_authority_epoch << ':' << cluster_policy_generation
      << ':' << ttl_generation << ':' << memory_pressure_generation
      << ':' << normalized_statement_hash << ':' << parameter_type_shape_hash
      << ':' << connection_uuid << ':' << transaction_context_hash
      << ':' << dialect
      << ':' << role_set_hash << ':' << group_set_hash
      << ':' << search_path_hash << ':' << language_profile << ':'
      << language_tag << ':' << input_syntax_profile << ':'
      << input_language_fallback_tag << ':' << common_resource_hash
      << ':' << language_resource_epoch << ':' << localized_name_epoch
      << ':' << policy_profile << ':' << parser_profile << ':'
      << message_resource_epoch << ':' << resource_compatibility_identity
      << ':' << resource_version_identity << ':'
      << result_contract_hash;
  return out.str();
}

std::string CacheKey::CompactKey() const {
  std::uint64_t hash = kFnvOffset;
  hash = MixString(hash, "sbsql-cache-v8");
  hash = MixUint64(hash, shape_hash);
  hash = MixUint64(hash, registry_version);
  hash = MixUint64(hash, catalog_epoch);
  hash = MixUint64(hash, security_policy_epoch);
  hash = MixUint64(hash, grant_epoch);
  hash = MixUint64(hash, descriptor_epoch);
  hash = MixUint64(hash, udr_epoch);
  hash = MixUint64(hash, name_resolution_epoch);
  hash = MixUint64(hash, resource_epoch);
  hash = MixUint64(hash, parser_package_generation);
  hash = MixUint64(hash, protocol_version);
  hash = MixUint64(hash, parser_package_version_hash);
  hash = MixUint64(hash, disclosure_policy_generation);
  hash = MixUint64(hash, redaction_policy_generation);
  hash = MixUint64(hash, security_authority_epoch);
  hash = MixUint64(hash, cluster_policy_generation);
  hash = MixUint64(hash, ttl_generation);
  hash = MixUint64(hash, memory_pressure_generation);
  hash = MixUint64(hash, normalized_statement_hash);
  hash = MixUint64(hash, parameter_type_shape_hash);
  hash = MixString(hash, connection_uuid);
  hash = MixString(hash, transaction_context_hash);
  hash = MixString(hash, dialect);
  hash = MixString(hash, role_set_hash);
  hash = MixString(hash, group_set_hash);
  hash = MixString(hash, search_path_hash);
  hash = MixString(hash, language_profile);
  hash = MixString(hash, language_tag);
  hash = MixString(hash, input_syntax_profile);
  hash = MixString(hash, input_language_fallback_tag);
  hash = MixString(hash, common_resource_hash);
  hash = MixUint64(hash, language_resource_epoch);
  hash = MixUint64(hash, localized_name_epoch);
  hash = MixString(hash, policy_profile);
  hash = MixString(hash, parser_profile);
  hash = MixUint64(hash, message_resource_epoch);
  hash = MixString(hash, resource_compatibility_identity);
  hash = MixString(hash, resource_version_identity);
  hash = MixString(hash, result_contract_hash);
  return CompactKeyFromHash(hash);
}

SblrTemplateCache::SblrTemplateCache(std::size_t max_entries) : max_entries_(max_entries) {}

std::optional<std::string> SblrTemplateCache::Lookup(const CacheKey& key) {
  auto entry = LookupEntry(key);
  if (!entry) return std::nullopt;
  return entry->sblr_payload;
}

std::optional<CacheEntry> SblrTemplateCache::LookupEntry(const CacheKey& key) {
  std::lock_guard lock(mutex_);
  auto it = entries_.find(key.CompactKey());
  if (it == entries_.end()) return std::nullopt;
  const auto stable_key = key.StableKey();
  if (it->second.stable_key != stable_key) return std::nullopt;
  it->second.entry.hits += 1;
  TouchLocked(it);
  return it->second.entry;
}

CacheStoreResult SblrTemplateCache::Store(CacheKey key, std::string sblr_payload) {
  CacheEntry entry;
  entry.key = std::move(key);
  entry.sblr_payload = std::move(sblr_payload);
  return StoreEntry(std::move(entry));
}

CacheStoreResult SblrTemplateCache::StoreEntry(CacheEntry entry) {
  std::string stable_key = entry.key.StableKey();
  std::string compact_key = entry.key.CompactKey();
  if (auto refusal = ValidateEntryForStore(entry, stable_key, compact_key)) {
    return *refusal;
  }

  std::lock_guard lock(mutex_);
  if (max_entries_ == 0) {
    CacheStoreResult result;
    result.stored = false;
    result.diagnostic_code = "SB_ORH_FRONTDOOR_CACHE_DISABLED.CAPACITY_ZERO";
    result.diagnostic_detail =
        "parser front-door cache entry not stored because max_entries is zero";
    result.stable_key = std::move(stable_key);
    result.compact_key = std::move(compact_key);
    return result;
  }

  auto existing = entries_.find(compact_key);
  if (existing == entries_.end() && entries_.size() >= max_entries_) {
    EvictOneLocked();
  } else if (existing != entries_.end()) {
    lru_order_.erase(existing->second.lru_position);
    entries_.erase(existing);
  }

  entry.hits = 0;
  lru_order_.push_front(compact_key);
  entries_.emplace(compact_key,
                   CacheRecord{std::move(entry), stable_key, lru_order_.begin()});

  CacheStoreResult result;
  result.stored = true;
  result.stable_key = std::move(stable_key);
  result.compact_key = std::move(compact_key);
  return result;
}

void SblrTemplateCache::Flush() {
  std::lock_guard lock(mutex_);
  entries_.clear();
  lru_order_.clear();
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateCatalogEpoch(std::uint64_t new_epoch) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_epoch](const CacheKey& key) {
    return key.catalog_epoch != new_epoch;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateSecurityPolicyEpoch(std::uint64_t new_epoch) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_epoch](const CacheKey& key) {
    return key.security_policy_epoch != new_epoch;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateGrantEpoch(std::uint64_t new_epoch) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_epoch](const CacheKey& key) {
    return key.grant_epoch != new_epoch;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateDescriptorEpoch(std::uint64_t new_epoch) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_epoch](const CacheKey& key) {
    return key.descriptor_epoch != new_epoch;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateUdrEpoch(std::uint64_t new_epoch) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_epoch](const CacheKey& key) {
    return key.udr_epoch != new_epoch;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateNameResolutionEpoch(std::uint64_t new_epoch) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_epoch](const CacheKey& key) {
    return key.name_resolution_epoch != new_epoch;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateResourceEpoch(std::uint64_t new_epoch) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_epoch](const CacheKey& key) {
    return key.resource_epoch != new_epoch;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateParserPackageGeneration(std::uint64_t new_generation) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_generation](const CacheKey& key) {
    return key.parser_package_generation != new_generation;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateProtocolVersion(std::uint32_t new_protocol_version) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_protocol_version](const CacheKey& key) {
    return key.protocol_version != new_protocol_version;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateParserPackageVersionHash(std::uint64_t new_version_hash) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_version_hash](const CacheKey& key) {
    return key.parser_package_version_hash != new_version_hash;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateDisclosurePolicyGeneration(std::uint64_t new_generation) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_generation](const CacheKey& key) {
    return key.disclosure_policy_generation != new_generation;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateRedactionPolicyGeneration(std::uint64_t new_generation) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_generation](const CacheKey& key) {
    return key.redaction_policy_generation != new_generation;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateSecurityAuthorityEpoch(std::uint64_t new_epoch) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_epoch](const CacheKey& key) {
    return key.security_authority_epoch != new_epoch;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateClusterPolicyGeneration(std::uint64_t new_generation) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_generation](const CacheKey& key) {
    return key.cluster_policy_generation != new_generation;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateTtlGeneration(std::uint64_t new_generation) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_generation](const CacheKey& key) {
    return key.ttl_generation != new_generation;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateMemoryPressureGeneration(std::uint64_t new_generation) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_generation](const CacheKey& key) {
    return key.memory_pressure_generation != new_generation;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateNormalizedStatementHash(std::uint64_t new_hash) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_hash](const CacheKey& key) {
    return key.normalized_statement_hash != new_hash;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateParameterTypeShapeHash(std::uint64_t new_hash) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_hash](const CacheKey& key) {
    return key.parameter_type_shape_hash != new_hash;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateConnection(std::string_view connection_uuid) {
  const std::string stable(connection_uuid);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.connection_uuid != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateTransactionContext(std::string_view transaction_context_hash) {
  const std::string stable(transaction_context_hash);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.transaction_context_hash != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateDialect(std::string_view dialect) {
  const std::string stable(dialect);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.dialect != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateRoleSetHash(std::string_view new_hash) {
  const std::string stable(new_hash);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.role_set_hash != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateGroupSetHash(std::string_view new_hash) {
  const std::string stable(new_hash);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.group_set_hash != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateSearchPathHash(std::string_view new_hash) {
  const std::string stable(new_hash);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.search_path_hash != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateLanguageProfile(std::string_view new_language_profile) {
  const std::string stable(new_language_profile);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.language_profile != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateLanguageTag(std::string_view new_language_tag) {
  const std::string stable(new_language_tag);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.language_tag != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateInputSyntaxProfile(
    std::string_view new_input_syntax_profile) {
  const std::string stable(new_input_syntax_profile);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.input_syntax_profile != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateInputLanguageFallbackTag(
    std::string_view new_fallback_tag) {
  const std::string stable(new_fallback_tag);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.input_language_fallback_tag != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateCommonResourceHash(std::string_view new_common_resource_hash) {
  const std::string stable(new_common_resource_hash);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.common_resource_hash != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateLanguageResourceEpoch(std::uint64_t new_epoch) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_epoch](const CacheKey& key) {
    return key.language_resource_epoch != new_epoch;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateLocalizedNameEpoch(std::uint64_t new_epoch) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_epoch](const CacheKey& key) {
    return key.localized_name_epoch != new_epoch;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidatePolicyProfile(std::string_view new_policy_profile) {
  const std::string stable(new_policy_profile);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.policy_profile != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateParserProfile(std::string_view new_parser_profile) {
  const std::string stable(new_parser_profile);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.parser_profile != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateMessageResourceEpoch(std::uint64_t new_epoch) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_epoch](const CacheKey& key) {
    return key.message_resource_epoch != new_epoch;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateResourceCompatibilityIdentity(
    std::string_view new_identity) {
  const std::string stable(new_identity);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.resource_compatibility_identity != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateResourceVersionIdentity(
    std::string_view new_identity) {
  const std::string stable(new_identity);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.resource_version_identity != stable;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateRegistryVersion(std::uint32_t new_registry_version) {
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([new_registry_version](const CacheKey& key) {
    return key.registry_version != new_registry_version;
  });
  ++invalidation_count_;
}

void SblrTemplateCache::InvalidateResultContractHash(
    std::string_view new_result_contract_hash) {
  const std::string stable(new_result_contract_hash);
  std::lock_guard lock(mutex_);
  EraseMatchingLocked([&stable](const CacheKey& key) {
    return key.result_contract_hash != stable;
  });
  ++invalidation_count_;
}

std::size_t SblrTemplateCache::Size() const {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

std::string SblrTemplateCache::SnapshotJson() const {
  std::lock_guard lock(mutex_);
  std::ostringstream out;
  out << "{\"entries\":" << entries_.size()
      << ",\"max_entries\":" << max_entries_
      << ",\"invalidations\":" << invalidation_count_
      << ",\"map_key\":\"compact_fnv1a64\""
      << ",\"key_dimensions\":[\"shape_hash\",\"registry_version\",\"catalog_epoch\","
         "\"security_policy_epoch\",\"grant_epoch\",\"descriptor_epoch\","
         "\"udr_epoch\",\"name_resolution_epoch\",\"resource_epoch\","
         "\"parser_package_generation\",\"protocol_version\","
         "\"parser_package_version_hash\",\"disclosure_policy_generation\","
         "\"redaction_policy_generation\","
         "\"security_authority_epoch\",\"cluster_policy_generation\","
         "\"ttl_generation\",\"memory_pressure_generation\","
         "\"normalized_statement_hash\",\"parameter_type_shape_hash\","
         "\"connection_uuid\",\"transaction_context_hash\",\"dialect\","
         "\"role_set_hash\",\"group_set_hash\",\"search_path_hash\","
         "\"language_profile\",\"language_tag\",\"input_syntax_profile\","
         "\"input_language_fallback_tag\",\"common_resource_hash\","
         "\"language_resource_epoch\",\"localized_name_epoch\","
         "\"policy_profile\",\"parser_profile\",\"message_resource_epoch\","
         "\"resource_compatibility_identity\",\"resource_version_identity\","
         "\"result_contract_hash\"],\"authority_cached\":{"
         "\"storage\":false,\"visibility\":false,\"authorization\":false,"
         "\"finality\":false},\"stable_keys\":[";
  bool first = true;
  for (const auto& compact_key : lru_order_) {
    const auto it = entries_.find(compact_key);
    if (it == entries_.end()) continue;
    if (!first) out << ',';
    first = false;
    out << '\"' << EscapeJson(it->second.stable_key) << '\"';
  }
  out << "]}";
  return out.str();
}

void SblrTemplateCache::EraseMatchingLocked(
    const std::function<bool(const CacheKey&)>& predicate) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (predicate(it->second.entry.key)) {
      lru_order_.erase(it->second.lru_position);
      it = entries_.erase(it);
    } else {
      ++it;
    }
  }
}

void SblrTemplateCache::TouchLocked(
    std::unordered_map<std::string, CacheRecord>::iterator it) {
  lru_order_.erase(it->second.lru_position);
  lru_order_.push_front(it->first);
  it->second.lru_position = lru_order_.begin();
}

void SblrTemplateCache::EvictOneLocked() {
  if (lru_order_.empty()) return;
  const auto victim = lru_order_.back();
  lru_order_.pop_back();
  entries_.erase(victim);
}

} // namespace scratchbird::parser::sbsql
