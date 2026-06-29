// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/name_registry.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/schema_tree_api.hpp"
#include "crud_support/crud_store.hpp"
#include "domain_support/domain_store.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace scratchbird::engine::internal_api {
namespace {

std::vector<std::string> SplitNameRegistryLine(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string DecodeNameRegistryText(const std::string& value) {
  std::string out;
  if ((value.size() % 2) != 0) { return out; }
  out.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const int hi = HexValue(value[i]);
    const int lo = HexValue(value[i + 1]);
    if (hi < 0 || lo < 0) { return {}; }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

std::uint64_t ParseNameRegistryU64(const std::string& value) {
  try { return static_cast<std::uint64_t>(std::stoull(value)); } catch (...) { return 0; }
}

bool ParseNameRegistryBool(const std::string& value) {
  return value == "1" || value == "true" || value == "TRUE";
}

std::string UpperAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
  return value;
}

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

bool StartsWithNameRegistry(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string NameRegistryFileFingerprint(const std::string& path) {
  std::error_code ec;
  const std::filesystem::path fs_path(path);
  if (!std::filesystem::exists(fs_path, ec) || ec) return path + ":missing";
  const auto size = std::filesystem::file_size(fs_path, ec);
  if (ec) return path + ":size_error";
  const auto mtime = std::filesystem::last_write_time(fs_path, ec);
  if (ec) return path + ":" + std::to_string(size) + ":mtime_error";
  return path + ":" + std::to_string(size) + ":" +
         std::to_string(mtime.time_since_epoch().count());
}

std::string NameRegistryLoadCacheKey(const EngineRequestContext& context,
                                     std::uint64_t observer_tx) {
  std::ostringstream key;
  key << "db=" << context.database_path
      << "|observer_tx=" << observer_tx
      << "|local_tx=" << context.local_transaction_id
      << "|catalog=" << context.catalog_generation_id
      << "|security=" << context.security_epoch
      << "|resource=" << context.resource_epoch
      << "|name_resolution=" << context.name_resolution_epoch
      << "|api=" << NameRegistryFileFingerprint(context.database_path + ".sb.api_events")
      << "|crud=" << NameRegistryFileFingerprint(context.database_path + ".sb.crud_events")
      << "|mga_meta=" << NameRegistryFileFingerprint(context.database_path + ".sb.mga_relation_metadata")
      << "|mga_desc=" << NameRegistryFileFingerprint(context.database_path + ".sb.mga_relation_descriptors")
      << "|mga_scope=" << NameRegistryFileFingerprint(context.database_path + ".sb.mga_relation_scope")
      << "|domains=" << NameRegistryFileFingerprint(context.database_path + ".sb.domain_catalog")
      << "|domain_events=" << NameRegistryFileFingerprint(context.database_path + ".sb.domain_events")
      << "|catalog_objects=" << NameRegistryFileFingerprint(context.database_path + ".sb.catalog_object_events");
  return key.str();
}

std::mutex& NameRegistryLoadCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, NameRegistryLoadResult>& NameRegistryLoadCache() {
  static std::map<std::string, NameRegistryLoadResult> cache;
  return cache;
}

std::optional<NameRegistryLoadResult> LookupNameRegistryLoadCache(
    const std::string& cache_key) {
  std::lock_guard<std::mutex> guard(NameRegistryLoadCacheMutex());
  const auto found = NameRegistryLoadCache().find(cache_key);
  if (found == NameRegistryLoadCache().end()) return std::nullopt;
  return found->second;
}

void StoreNameRegistryLoadCache(const std::string& cache_key,
                                const NameRegistryLoadResult& result) {
  if (cache_key.empty() || !result.ok) return;
  std::lock_guard<std::mutex> guard(NameRegistryLoadCacheMutex());
  auto& cache = NameRegistryLoadCache();
  cache[cache_key] = result;
  constexpr std::size_t kMaxNameRegistryLoadCacheEntries = 64;
  while (cache.size() > kMaxNameRegistryLoadCacheEntries) {
    cache.erase(cache.begin());
  }
}

void MergeMgaRelationCatalogState(CrudState* base, const CrudState& mga_state) {
  if (base == nullptr) { return; }
  for (const auto& [tx, state] : mga_state.transactions) {
    base->transactions[tx] = state;
    base->max_transaction_id = std::max(base->max_transaction_id, tx);
  }
  for (const auto& table : mga_state.tables) {
    auto existing = std::find_if(base->tables.begin(), base->tables.end(),
                                 [&table](const CrudTableRecord& candidate) {
                                   return candidate.table_uuid == table.table_uuid;
                                 });
    if (existing == base->tables.end()) {
      base->tables.push_back(table);
    } else {
      *existing = table;
    }
  }
  for (const auto& index : mga_state.indexes) {
    auto existing = std::find_if(base->indexes.begin(), base->indexes.end(),
                                 [&index](const CrudIndexRecord& candidate) {
                                   return candidate.index_uuid == index.index_uuid;
                                 });
    if (existing == base->indexes.end()) {
      base->indexes.push_back(index);
    } else {
      *existing = index;
    }
  }
  base->max_event_sequence =
      std::max(base->max_event_sequence, mga_state.max_event_sequence);
}

std::string ProfileName(const std::string& identifier_profile_uuid) {
  if (identifier_profile_uuid.empty()) { return "sbsql_v3"; }
  return LowerAscii(identifier_profile_uuid);
}

bool ProfileFoldsLower(const std::string& profile) {
  const std::string p = ProfileName(profile);
  return p.find("postgres") != std::string::npos || p.find("cockroach") != std::string::npos ||
         p.find("yugabyte") != std::string::npos || p.find("xtdb") != std::string::npos ||
         p == "postgresql_family";
}

bool ProfileFoldsUpper(const std::string& profile) {
  const std::string p = ProfileName(profile);
  return p.empty() || p == "sbsql_v3" || p.find("firebird") != std::string::npos ||
         p == "native" || p == "sbsql";
}

bool ObjectClassMatchesRequest(const std::string& entry_class,
                               const std::string& requested_class) {
  if (requested_class.empty() || entry_class == requested_class) {
    return true;
  }
  if (requested_class == "relation") {
    return entry_class == "table" ||
           entry_class == "view" ||
           entry_class == "materialized_view" ||
           entry_class == "external_table" ||
           entry_class == "foreign_table";
  }
  if (requested_class == "role") {
    return entry_class == "security_role" || entry_class == "principal";
  }
  if (requested_class == "group") {
    return entry_class == "security_group" || entry_class == "principal";
  }
  if (requested_class == "principal") {
    return entry_class == "user" ||
           entry_class == "security_role" ||
           entry_class == "security_group" ||
           entry_class == "role" ||
           entry_class == "group";
  }
  if (requested_class == "policy") {
    return entry_class == "security_policy";
  }
  if (requested_class == "mask" || requested_class == "rls") {
    return entry_class == "security_policy";
  }
  return false;
}

bool NameClassIsCanonical(const std::string& name_class) {
  return name_class.empty() || name_class == "primary" || name_class == "default" ||
         name_class == "canonical";
}

bool NameRegistryDefaultLanguageCanonical(const NameRegistryEntry& entry,
                                          const std::string& default_language) {
  return entry.language_tag == default_language && NameClassIsCanonical(entry.name_class);
}

bool NameRegistryAliasScoped(const NameRegistryEntry& entry,
                             const std::string& default_language) {
  return entry.language_tag != default_language || !NameClassIsCanonical(entry.name_class);
}

bool NameRegistryCollisionScopeMatches(const NameRegistryEntry& existing,
                                       const NameRegistryEntry& wanted,
                                       const std::string& default_language) {
  if (existing.language_tag == wanted.language_tag) { return true; }
  if (NameRegistryDefaultLanguageCanonical(existing, default_language) ||
      NameRegistryDefaultLanguageCanonical(wanted, default_language)) {
    return true;
  }
  return NameRegistryAliasScoped(existing, default_language) ||
         NameRegistryAliasScoped(wanted, default_language);
}

bool NameRegistryLookupKeysCollide(const NameRegistryEntry& wanted,
                                   const NameRegistryEntry& existing) {
  if (!wanted.requires_exact_match && existing.requires_exact_match) { return false; }
  const std::string left_key = wanted.requires_exact_match ? wanted.exact_lookup_key
                                                           : wanted.normalized_lookup_key;
  const std::string right_key = wanted.requires_exact_match ? existing.exact_lookup_key
                                                            : existing.normalized_lookup_key;
  return left_key == right_key;
}

int NameRegistryNameClassRank(const std::string& name_class) {
  if (NameClassIsCanonical(name_class)) { return 0; }
  if (name_class == "alias") { return 1; }
  return 2;
}

bool NameRegistryDisplayOrderLess(const NameRegistryEntry& lhs,
                                  const NameRegistryEntry& rhs) {
  if (lhs.language_tag != rhs.language_tag) { return lhs.language_tag < rhs.language_tag; }
  const int lhs_rank = NameRegistryNameClassRank(lhs.name_class);
  const int rhs_rank = NameRegistryNameClassRank(rhs.name_class);
  if (lhs_rank != rhs_rank) { return lhs_rank < rhs_rank; }
  if (ProfileName(lhs.identifier_profile_uuid) != ProfileName(rhs.identifier_profile_uuid)) {
    return ProfileName(lhs.identifier_profile_uuid) < ProfileName(rhs.identifier_profile_uuid);
  }
  if (lhs.display_name != rhs.display_name) { return lhs.display_name < rhs.display_name; }
  if (lhs.raw_name_text != rhs.raw_name_text) { return lhs.raw_name_text < rhs.raw_name_text; }
  return lhs.name_entry_uuid < rhs.name_entry_uuid;
}

bool ProfileFoldsMysqlInsensitive(const std::string& profile) {
  const std::string p = ProfileName(profile);
  return p.find("mysql_case_insensitive") != std::string::npos ||
         p.find("mariadb_case_insensitive") != std::string::npos ||
         p.find("vitess_case_insensitive") != std::string::npos ||
         p.find("tidb_case_insensitive") != std::string::npos ||
         p.find("dolt_case_insensitive") != std::string::npos;
}

bool EntryVisible(const CrudState& crud_state, const NameRegistryEntry& entry, std::uint64_t observer_tx) {
  if (entry.deleted || entry.lifecycle_state == "dropped") { return false; }
  return CrudCreatorVisible(crud_state, entry.creator_tx, entry.event_sequence, observer_tx);
}

bool MgaCreatorVisible(const scratchbird::transaction::mga::LocalTransactionInventory& inventory,
                       std::uint64_t creator_tx,
                       std::uint64_t observer_tx) {
  if (creator_tx == 0) { return true; }
  for (const auto& entry : inventory.entries) {
    if (!entry.identity.local_id.valid() || entry.identity.local_id.value != creator_tx) { continue; }
    using scratchbird::transaction::mga::TransactionState;
    if (entry.state == TransactionState::committed || entry.state == TransactionState::archived) {
      return true;
    }
    return creator_tx == observer_tx &&
           (entry.state == TransactionState::active ||
            entry.state == TransactionState::read_only_active ||
            entry.state == TransactionState::preparing ||
            entry.state == TransactionState::prepared);
  }
  return false;
}

bool CreatorVisible(const CrudState& crud_state,
                    const scratchbird::storage::database::LocalTransactionStoreResult& transaction_inventory,
                    std::uint64_t creator_tx,
                    std::uint64_t event_sequence,
                    std::uint64_t observer_tx) {
  if (CrudCreatorVisible(crud_state, creator_tx, event_sequence, observer_tx)) { return true; }
  return transaction_inventory.ok() &&
         MgaCreatorVisible(transaction_inventory.inventory, creator_tx, observer_tx);
}

bool EntryVisible(const CrudState& crud_state,
                  const scratchbird::storage::database::LocalTransactionStoreResult& transaction_inventory,
                  const NameRegistryEntry& entry,
                  std::uint64_t observer_tx) {
  if (entry.deleted || entry.lifecycle_state == "dropped") { return false; }
  return CreatorVisible(crud_state,
                        transaction_inventory,
                        entry.creator_tx,
                        entry.event_sequence,
                        observer_tx);
}

std::string ApiBehaviorPayloadField(const std::string& payload, const std::string& field_name) {
  const std::string prefix = field_name + "=";
  for (const auto& part : SplitNameRegistryLine(payload, ';')) {
    if (part.rfind(prefix, 0) == 0) { return part.substr(prefix.size()); }
  }
  return {};
}

std::string EntryKey(const NameRegistryEntry& entry) {
  return entry.object_uuid + "\t" + entry.language_tag + "\t" + entry.name_class + "\t" +
         entry.identifier_profile_uuid + "\t" + entry.raw_name_text;
}

void AddIfNoExplicit(NameRegistryState* state,
                     std::set<std::string>* added_entries,
                     NameRegistryEntry entry) {
  const std::string key = EntryKey(entry);
  if (entry.object_uuid.empty() || added_entries->count(key) != 0) { return; }
  entry.derived_from_legacy_name = true;
  added_entries->insert(key);
  state->entries.push_back(std::move(entry));
}

void RemoveObjectEntries(NameRegistryState* state,
                         std::set<std::string>* added_entries,
                         const std::string& object_uuid) {
  for (auto it = state->entries.begin(); it != state->entries.end();) {
    if (it->object_uuid == object_uuid) {
      added_entries->erase(EntryKey(*it));
      it = state->entries.erase(it);
    } else {
      ++it;
    }
  }
}

void ReplaceEntry(NameRegistryState* state,
                  std::set<std::string>* added_entries,
                  NameRegistryEntry entry) {
  const std::string key = EntryKey(entry);
  for (auto it = state->entries.begin(); it != state->entries.end();) {
    if (EntryKey(*it) == key) { it = state->entries.erase(it); }
    else { ++it; }
  }
  added_entries->erase(key);
  added_entries->insert(key);
  state->entries.push_back(std::move(entry));
}

NameRegistryEntry EntryFromSimpleName(const EngineRequestContext& context,
                                      const std::string& object_uuid,
                                      const std::string& object_class,
                                      const std::string& scope_uuid,
                                      const std::string& name,
                                      const std::string& language_tag = "en") {
  EngineLocalizedName localized;
  localized.language_tag = language_tag.empty() ? "en" : language_tag;
  localized.name_class = "primary";
  localized.name = name;
  localized.raw_name_text = name;
  localized.display_name = name;
  localized.default_name = true;
  return MakeNameRegistryEntry(context, object_uuid, object_class, scope_uuid, localized, name);
}

std::string RequestedNameFromOptions(const EngineApiRequest& request) {
  for (const auto& option : request.option_envelopes) {
    if (StartsWithNameRegistry(option, "name:")) { return option.substr(5); }
  }
  return {};
}

std::vector<std::string> LanguageCandidates(const EngineRequestContext& context) {
  std::vector<std::string> languages;
  auto push = [&](std::string value) {
    if (value.empty()) { return; }
    if (std::find(languages.begin(), languages.end(), value) == languages.end()) { languages.push_back(std::move(value)); }
  };
  push(NameRegistrySessionLanguage(context));
  push(NameRegistryDefaultLanguage(context));
  push("und");
  return languages;
}

std::vector<std::string> ScopeCandidates(const EngineApiRequest& request) {
  std::vector<std::string> scopes;
  auto push = [&](std::string value) {
    if (std::find(scopes.begin(), scopes.end(), value) == scopes.end()) { scopes.push_back(std::move(value)); }
  };
  push(request.target_schema.uuid.canonical);
  push(request.context.current_schema_uuid.canonical);
  for (const auto& schema : request.context.search_path_schema_uuids) { push(schema.canonical); }
  push(request.context.default_root_uuid.canonical);
  push({});
  return scopes;
}

std::vector<std::string> ResolveQualifiedNameRegistryScopes(
    const EngineApiRequest& request,
    const NameRegistryState& state,
    const std::vector<std::string>& languages) {
  if (request.sql_object_reference.path_components.empty()) {
    return ScopeCandidates(request);
  }

  std::vector<std::string> current_scopes = ScopeCandidates(request);
  for (const auto& segment : request.sql_object_reference.path_components) {
    const std::string raw_name = segment.raw_text;
    if (raw_name.empty()) { return {}; }
    const std::string profile = segment.identifier_profile_uuid.empty()
                                    ? NameRegistryDefaultIdentifierProfile(request.context)
                                    : segment.identifier_profile_uuid;
    const bool exact = segment.requires_exact_match || segment.was_quoted;
    const std::string lookup_key = exact ? NameRegistryLookupKey(raw_name, profile, true)
                                         : NameRegistryLookupKey(raw_name, profile, false);

    std::vector<std::string> next_scopes;
    auto push_next = [&](const std::string& value) {
      if (value.empty()) { return; }
      if (std::find(next_scopes.begin(), next_scopes.end(), value) == next_scopes.end()) {
        next_scopes.push_back(value);
      }
    };

    for (const auto& language : languages) {
      for (const auto& scope : current_scopes) {
        for (const auto& entry : state.entries) {
          if (entry.deleted || entry.lifecycle_state != "active") { continue; }
          if (entry.object_class != "schema") { continue; }
          if (entry.scope_uuid != scope) { continue; }
          if (entry.language_tag != language) { continue; }
          const std::string entry_profile = entry.identifier_profile_uuid.empty() ? "sbsql_v3" : entry.identifier_profile_uuid;
          if (ProfileName(entry_profile) != ProfileName(profile)) { continue; }
          if (!exact && entry.requires_exact_match) { continue; }
          const std::string entry_key = exact ? entry.exact_lookup_key : entry.normalized_lookup_key;
          if (entry_key != lookup_key) { continue; }
          push_next(entry.object_uuid);
        }
      }
      if (!next_scopes.empty()) { break; }
    }

    current_scopes = std::move(next_scopes);
    if (current_scopes.empty()) { return {}; }
  }

  return current_scopes;
}

std::string StandardNameNotFoundMessageKey() { return "message_vector.item_not_found_or_does_not_exist"; }

}  // namespace

std::string NameRegistryDefaultLanguage(const EngineRequestContext& context) {
  return context.language_context.default_language_tag.empty() ? "en" : context.language_context.default_language_tag;
}

std::string NameRegistrySessionLanguage(const EngineRequestContext& context) {
  return context.language_context.language_tag.empty() ? NameRegistryDefaultLanguage(context) : context.language_context.language_tag;
}

std::string NameRegistryDefaultIdentifierProfile(const EngineRequestContext& context) {
  return context.identifier_profile_uuid.empty() ? "sbsql_v3" : context.identifier_profile_uuid;
}

std::string NameRegistryLookupKey(std::string text,
                                  const std::string& identifier_profile_uuid,
                                  bool requires_exact_match) {
  if (requires_exact_match) { return text; }
  if (ProfileFoldsLower(identifier_profile_uuid) || ProfileFoldsMysqlInsensitive(identifier_profile_uuid) ||
      ProfileName(identifier_profile_uuid) == "sqlite") {
    return LowerAscii(std::move(text));
  }
  if (ProfileFoldsUpper(identifier_profile_uuid)) { return UpperAscii(std::move(text)); }
  return text;
}

NameRegistryEntry MakeNameRegistryEntry(const EngineRequestContext& context,
                                        const std::string& object_uuid,
                                        const std::string& object_class,
                                        const std::string& scope_uuid,
                                        const EngineLocalizedName& name,
                                        const std::string& fallback_name) {
  NameRegistryEntry entry;
  entry.creator_tx = context.local_transaction_id;
  entry.name_entry_uuid = GenerateCrudEngineUuid("name");
  entry.object_uuid = object_uuid;
  entry.object_class = object_class;
  entry.scope_uuid = scope_uuid;
  entry.parent_schema_uuid = scope_uuid;
  entry.language_tag = name.language_tag.empty() ? NameRegistryDefaultLanguage(context) : name.language_tag;
  entry.name_class = name.name_class.empty() ? (name.default_name ? "primary" : "alias") : name.name_class;
  entry.reference_id = name.reference_id;
  entry.dialect_profile_uuid = name.dialect_profile_uuid;
  entry.identifier_profile_uuid = name.identifier_profile_uuid.empty() ? NameRegistryDefaultIdentifierProfile(context) : name.identifier_profile_uuid;
  entry.case_fold_profile_uuid = name.case_fold_profile_uuid;
  entry.quoted_identifier_profile_uuid = name.quoted_identifier_profile_uuid;
  entry.raw_name_text = !name.raw_name_text.empty() ? name.raw_name_text : (!name.name.empty() ? name.name : fallback_name);
  entry.display_name = !name.display_name.empty() ? name.display_name : entry.raw_name_text;
  entry.was_quoted = name.was_quoted;
  entry.quote_style = name.quote_style.empty() ? (name.was_quoted ? "double_quote" : "none") : name.quote_style;
  entry.requires_exact_match = name.requires_exact_match || name.was_quoted;
  entry.normalized_lookup_key = !name.normalized_lookup_key.empty()
                                    ? name.normalized_lookup_key
                                    : NameRegistryLookupKey(entry.raw_name_text, entry.identifier_profile_uuid, false);
  entry.exact_lookup_key = !name.exact_lookup_key.empty()
                               ? name.exact_lookup_key
                               : NameRegistryLookupKey(entry.raw_name_text, entry.identifier_profile_uuid, true);
  entry.full_path_lookup_key = !name.full_path_lookup_key.empty()
                                   ? name.full_path_lookup_key
                                   : (name.path.empty() ? entry.normalized_lookup_key
                                                        : NameRegistryLookupKey(name.path, entry.identifier_profile_uuid, entry.requires_exact_match));
  entry.catalog_generation_id = context.catalog_generation_id;
  entry.resource_epoch = context.resource_epoch;
  entry.name_resolution_epoch = context.name_resolution_epoch;
  entry.lifecycle_state = "active";
  return entry;
}

EngineApiDiagnostic AppendNameRegistryEntry(const EngineRequestContext& context,
                                            const NameRegistryEntry& entry,
                                            const std::string& operation_id) {
  const auto context_status = ValidateApiBehaviorContext(context, operation_id, false, true);
  if (context_status.error) { return context_status; }
  std::string event = std::string(kNameRegistryEventMagic) + "\tENTRY\t" +
                      std::to_string(entry.creator_tx) + "\t" +
                      entry.name_entry_uuid + "\t" + entry.object_uuid + "\t" + entry.object_class + "\t" +
                      entry.scope_uuid + "\t" + entry.parent_object_uuid + "\t" + entry.parent_schema_uuid + "\t" +
                      entry.language_tag + "\t" + entry.name_class + "\t" + entry.reference_id + "\t" +
                      entry.dialect_profile_uuid + "\t" + entry.identifier_profile_uuid + "\t" +
                      entry.case_fold_profile_uuid + "\t" + entry.quoted_identifier_profile_uuid + "\t" +
                      EncodeCrudText(entry.raw_name_text) + "\t" + EncodeCrudText(entry.display_name) + "\t" +
                      (entry.was_quoted ? "1" : "0") + "\t" + entry.quote_style + "\t" +
                      (entry.requires_exact_match ? "1" : "0") + "\t" +
                      EncodeCrudText(entry.normalized_lookup_key) + "\t" +
                      EncodeCrudText(entry.exact_lookup_key) + "\t" +
                      EncodeCrudText(entry.full_path_lookup_key) + "\t" +
                      std::to_string(entry.catalog_generation_id) + "\t" +
                      std::to_string(entry.resource_epoch) + "\t" +
                      std::to_string(entry.name_resolution_epoch) + "\t" +
                      entry.lifecycle_state + "\t" + (entry.deleted ? "1" : "0");
  const auto appended = AppendApiBehaviorEvent(context, event);
  if (appended.error) { return appended; }
  const std::string invalidate = std::string(kNameRegistryEventMagic) + "\tCACHE_INVALIDATE\t" +
                                 std::to_string(context.local_transaction_id) + "\t" +
                                 EncodeCrudText(operation_id) + "\tENTRY\t" +
                                 entry.object_uuid + "\t" +
                                 EncodeCrudText(entry.object_class) + "\t" +
                                 EncodeCrudText(entry.scope_uuid) + "\t" +
                                 std::to_string(entry.catalog_generation_id) + "\t" +
                                 std::to_string(entry.resource_epoch) + "\t" +
                                 std::to_string(entry.name_resolution_epoch);
  return AppendApiBehaviorEvent(context, invalidate);
}

EngineApiDiagnostic PersistNameRegistryEntriesForObject(const EngineRequestContext& context,
                                                        const std::string& operation_id,
                                                        const std::string& object_uuid,
                                                        const std::string& object_class,
                                                        const std::string& scope_uuid,
                                                        const std::vector<EngineLocalizedName>& names,
                                                        const std::string& fallback_name) {
  std::vector<EngineLocalizedName> effective_names = names;
  if (effective_names.empty()) {
    EngineLocalizedName generated;
    generated.language_tag = NameRegistryDefaultLanguage(context);
    generated.name_class = "primary";
    generated.name = fallback_name;
    generated.raw_name_text = fallback_name;
    generated.display_name = fallback_name;
    generated.default_name = true;
    effective_names.push_back(generated);
  }
  for (const auto& name : effective_names) {
    const auto diagnostic = AppendNameRegistryEntry(context,
                                                   MakeNameRegistryEntry(context, object_uuid, object_class, scope_uuid, name, fallback_name),
                                                   operation_id);
    if (diagnostic.error) { return diagnostic; }
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

EngineApiDiagnostic RetireNameRegistryEntriesForObject(const EngineRequestContext& context,
                                                       const std::string& operation_id,
                                                       const std::string& object_uuid) {
  const auto context_status = ValidateApiBehaviorContext(context, operation_id, false, true);
  if (context_status.error) { return context_status; }
  if (object_uuid.empty()) { return MakeInvalidRequestDiagnostic(operation_id, "object_uuid_required"); }
  const std::string event = std::string(kNameRegistryEventMagic) + "\tRETIRE_OBJECT\t" +
                            std::to_string(context.local_transaction_id) + "\t" + object_uuid;
  const auto appended = AppendApiBehaviorEvent(context, event);
  if (appended.error) { return appended; }
  const std::string invalidate = std::string(kNameRegistryEventMagic) + "\tCACHE_INVALIDATE\t" +
                                 std::to_string(context.local_transaction_id) + "\t" +
                                 EncodeCrudText(operation_id) + "\tRETIRE_OBJECT\t" +
                                 object_uuid + "\t\t\t" +
                                 std::to_string(context.catalog_generation_id) + "\t" +
                                 std::to_string(context.resource_epoch) + "\t" +
                                 std::to_string(context.name_resolution_epoch);
  return AppendApiBehaviorEvent(context, invalidate);
}

NameRegistryLoadResult LoadNameRegistryState(const EngineRequestContext& context,
                                             std::uint64_t observer_tx) {
  NameRegistryLoadResult result;
  const auto path_status = ValidateApiBehaviorContext(context, "catalog.name_registry.load", false, true);
  if (path_status.error) {
    result.diagnostic = path_status;
    return result;
  }
  const std::string load_cache_key =
      NameRegistryLoadCacheKey(context, observer_tx);
  if (auto cached = LookupNameRegistryLoadCache(load_cache_key)) {
    return *cached;
  }
  const auto crud = LoadCrudState(context);
  if (!crud.ok) {
    result.diagnostic = crud.diagnostic;
    return result;
  }
  CrudState catalog_state = crud.state;
  const auto mga_relations = LoadMgaRelationStoreState(context);
  if (mga_relations.ok) {
    MergeMgaRelationCatalogState(
        &catalog_state,
        BuildCrudCompatibilityStateFromMga(mga_relations.state));
  }
  const auto transaction_inventory =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(context.database_path);
  std::ifstream in(context.database_path + ".sb.api_events", std::ios::binary);
  if (!in) { in.open(context.database_path, std::ios::binary); }
  if (!in) {
    result.diagnostic = MakeInvalidRequestDiagnostic("catalog.name_registry.load", "database_path_unreadable");
    return result;
  }
  std::set<std::string> added_entries;
  std::set<std::string> suppress_legacy_objects;
  std::uint64_t event_sequence = 0;
  std::string line;
  while (std::getline(in, line)) {
    ++event_sequence;
    if (!StartsWithNameRegistry(line, kNameRegistryEventMagic)) { continue; }
    const auto parts = SplitNameRegistryLine(line, '\t');
    if (parts.size() >= 4 && parts[1] == "RETIRE_OBJECT") {
      const std::uint64_t creator_tx = ParseNameRegistryU64(parts[2]);
      const std::string object_uuid = parts[3];
      if (CreatorVisible(catalog_state,
                         transaction_inventory,
                         creator_tx,
                         event_sequence,
                         observer_tx)) {
        suppress_legacy_objects.insert(object_uuid);
        RemoveObjectEntries(&result.state, &added_entries, object_uuid);
      }
      continue;
    }
    if (parts.size() < 29 || parts[1] != "ENTRY") { continue; }
    NameRegistryEntry entry;
    entry.event_sequence = event_sequence;
    entry.creator_tx = ParseNameRegistryU64(parts[2]);
    entry.name_entry_uuid = parts[3];
    entry.object_uuid = parts[4];
    entry.object_class = parts[5];
    entry.scope_uuid = parts[6];
    entry.parent_object_uuid = parts[7];
    entry.parent_schema_uuid = parts[8];
    entry.language_tag = parts[9].empty() ? "en" : parts[9];
    entry.name_class = parts[10].empty() ? "primary" : parts[10];
    entry.reference_id = parts[11];
    entry.dialect_profile_uuid = parts[12];
    entry.identifier_profile_uuid = parts[13].empty() ? "sbsql_v3" : parts[13];
    entry.case_fold_profile_uuid = parts[14];
    entry.quoted_identifier_profile_uuid = parts[15];
    entry.raw_name_text = DecodeNameRegistryText(parts[16]);
    entry.display_name = DecodeNameRegistryText(parts[17]);
    entry.was_quoted = ParseNameRegistryBool(parts[18]);
    entry.quote_style = parts[19].empty() ? "none" : parts[19];
    entry.requires_exact_match = ParseNameRegistryBool(parts[20]);
    entry.normalized_lookup_key = DecodeNameRegistryText(parts[21]);
    entry.exact_lookup_key = DecodeNameRegistryText(parts[22]);
    entry.full_path_lookup_key = DecodeNameRegistryText(parts[23]);
    entry.catalog_generation_id = ParseNameRegistryU64(parts[24]);
    entry.resource_epoch = ParseNameRegistryU64(parts[25]);
    entry.name_resolution_epoch = ParseNameRegistryU64(parts[26]);
    entry.lifecycle_state = parts[27].empty() ? "active" : parts[27];
    entry.deleted = ParseNameRegistryBool(parts[28]);
    if (EntryVisible(catalog_state, transaction_inventory, entry, observer_tx)) {
      suppress_legacy_objects.insert(entry.object_uuid);
      ReplaceEntry(&result.state, &added_entries, std::move(entry));
    }
  }
  for (const auto& schema : VisibleSchemaTreeRecords(context, observer_tx)) {
    if (suppress_legacy_objects.count(schema.schema_uuid) != 0) { continue; }
    for (const auto& localized : schema.localized_names) {
      AddIfNoExplicit(&result.state,
                      &added_entries,
                      MakeNameRegistryEntry(context, schema.schema_uuid, "schema", schema.parent_schema_uuid, localized, schema.default_name));
    }
  }
  for (const auto& table : catalog_state.tables) {
    if (suppress_legacy_objects.count(table.table_uuid) != 0) { continue; }
    if (CrudCreatorVisible(catalog_state, table.creator_tx, table.event_sequence, observer_tx)) {
      AddIfNoExplicit(&result.state, &added_entries, EntryFromSimpleName(context, table.table_uuid, "table", {}, table.default_name));
    }
  }
  for (const auto& index : catalog_state.indexes) {
    if (suppress_legacy_objects.count(index.index_uuid) != 0) { continue; }
    if (CrudCreatorVisible(catalog_state, index.creator_tx, index.event_sequence, observer_tx)) {
      AddIfNoExplicit(&result.state, &added_entries, EntryFromSimpleName(context, index.index_uuid, "index", index.table_uuid, index.default_name));
    }
  }
  for (const auto& record : VisibleApiBehaviorRecords(context, {}, observer_tx)) {
    if (suppress_legacy_objects.count(record.object_uuid) != 0) { continue; }
    if (!record.default_name.empty()) {
      std::string scope_uuid = ApiBehaviorPayloadField(record.payload, "schema");
      if (record.object_kind == "database" || record.object_kind == "filespace") {
        scope_uuid.clear();
      }
      AddIfNoExplicit(&result.state,
                      &added_entries,
                      EntryFromSimpleName(context,
                                          record.object_uuid,
                                          record.object_kind,
                                          scope_uuid,
                                          record.default_name));
    }
  }
  const auto domains = LoadDomainState(context);
  if (domains.ok) {
    for (const auto& domain : domains.domains) {
      if (const auto visible = FindVisibleDomain(context, domain.domain_uuid, observer_tx)) {
        if (suppress_legacy_objects.count(visible->domain_uuid) != 0) { continue; }
        AddIfNoExplicit(&result.state,
                        &added_entries,
                        EntryFromSimpleName(context, visible->domain_uuid, "domain", visible->schema_uuid, visible->default_name));
      }
    }
  }
  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  StoreNameRegistryLoadCache(load_cache_key, result);
  return result;
}

NameRegistryResolveResult ResolveNameRegistryPrivate(const EngineApiRequest& request,
                                                     const std::string& requested_object_class) {
  NameRegistryResolveResult result;
  const auto loaded = LoadNameRegistryState(request.context, request.context.local_transaction_id);
  if (!loaded.ok) {
    result.diagnostic = loaded.diagnostic;
    return result;
  }
  std::vector<EngineLocalizedName> wanted_names = request.localized_names;
  if (!request.sql_object_reference.object_name.raw_text.empty()) {
    EngineLocalizedName from_reference;
    from_reference.name = request.sql_object_reference.object_name.raw_text;
    from_reference.raw_name_text = request.sql_object_reference.object_name.raw_text;
    from_reference.was_quoted = request.sql_object_reference.object_name.was_quoted;
    from_reference.quote_style = request.sql_object_reference.object_name.quote_style;
    from_reference.requires_exact_match = request.sql_object_reference.object_name.requires_exact_match ||
                                          request.sql_object_reference.object_name.was_quoted;
    from_reference.identifier_profile_uuid = request.sql_object_reference.object_name.identifier_profile_uuid;
    wanted_names.push_back(from_reference);
  }
  const std::string option_name = RequestedNameFromOptions(request);
  if (!option_name.empty()) {
    EngineLocalizedName from_option;
    from_option.name = option_name;
    from_option.raw_name_text = option_name;
    wanted_names.push_back(from_option);
  }
  if (wanted_names.empty()) {
    result.diagnostic = MakeInvalidRequestDiagnostic("catalog.resolve_name", "name_required");
    return result;
  }
  const auto languages = LanguageCandidates(request.context);
  const auto scopes = ResolveQualifiedNameRegistryScopes(request, loaded.state, languages);
  std::set<std::string> seen_objects;
  for (const auto& wanted : wanted_names) {
    const std::string raw_name = !wanted.raw_name_text.empty() ? wanted.raw_name_text : wanted.name;
    if (raw_name.empty()) { continue; }
    const std::string profile = wanted.identifier_profile_uuid.empty()
                                    ? NameRegistryDefaultIdentifierProfile(request.context)
                                    : wanted.identifier_profile_uuid;
    const bool exact = wanted.requires_exact_match || wanted.was_quoted;
    const std::string lookup_key = exact ? NameRegistryLookupKey(raw_name, profile, true)
                                         : NameRegistryLookupKey(raw_name, profile, false);
    for (const auto& language : languages) {
      for (const auto& scope : scopes) {
        for (const auto& entry : loaded.state.entries) {
          if (!ObjectClassMatchesRequest(entry.object_class, requested_object_class)) { continue; }
          if (entry.scope_uuid != scope) { continue; }
          if (entry.language_tag != language) { continue; }
          const std::string entry_profile = entry.identifier_profile_uuid.empty() ? "sbsql_v3" : entry.identifier_profile_uuid;
          if (ProfileName(entry_profile) != ProfileName(profile)) { continue; }
          if (!exact && entry.requires_exact_match) { continue; }
          const std::string entry_key = exact ? entry.exact_lookup_key : entry.normalized_lookup_key;
          if (entry_key != lookup_key) { continue; }
          if (seen_objects.insert(entry.object_uuid).second) { result.matches.push_back(entry); }
        }
      }
      if (!result.matches.empty()) {
        result.ok = true;
        result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
        return result;
      }
    }
  }
  result.diagnostic = MakeEngineApiDiagnostic("CATALOG.NAME.NOT_FOUND", StandardNameNotFoundMessageKey(), "item_not_found_or_does_not_exist");
  return result;
}

namespace {

bool NameRegistryPolicyStartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool PublicNameRegistryEntryVisible(const EngineApiRequest& request,
                                    const NameRegistryEntry& entry) {
  for (const auto& policy : request.policy_profile.encoded_profiles) {
    if (policy == "metadata_visibility:hide_all") { return false; }

    const std::string hide_object_prefix = "metadata_visibility:hide_object:";
    if (NameRegistryPolicyStartsWith(policy, hide_object_prefix) &&
        policy.substr(hide_object_prefix.size()) == entry.object_uuid) {
      return false;
    }

    const std::string hide_class_prefix = "metadata_visibility:hide_class:";
    if (NameRegistryPolicyStartsWith(policy, hide_class_prefix) &&
        policy.substr(hide_class_prefix.size()) == entry.object_class) {
      return false;
    }
  }
  return true;
}

}  // namespace

NameRegistryResolveResult ResolveNameRegistryPublic(const EngineApiRequest& request,
                                                    const std::string& requested_object_class) {
  auto result = ResolveNameRegistryPrivate(request, requested_object_class);
  if (!result.ok) { return result; }

  std::vector<NameRegistryEntry> visible;
  for (const auto& match : result.matches) {
    if (PublicNameRegistryEntryVisible(request, match)) { visible.push_back(match); }
  }

  if (visible.empty()) {
    result.ok = false;
    result.matches.clear();
    result.diagnostic = MakeEngineApiDiagnostic("CATALOG.NAME.NOT_FOUND",
                                                StandardNameNotFoundMessageKey(),
                                                "item_not_found_or_does_not_exist");
    return result;
  }

  result.matches = std::move(visible);
  return result;
}

NameRegistryResolveResult ResolveNameRegistry(const EngineApiRequest& request,
                                              const std::string& requested_object_class) {
  return ResolveNameRegistryPublic(request, requested_object_class);
}

namespace {

std::string UuidToNameTargetUuid(const EngineApiRequest& request,
                                 const std::string& object_uuid) {
  if (!object_uuid.empty()) { return object_uuid; }
  if (!request.target_object.uuid.canonical.empty()) { return request.target_object.uuid.canonical; }
  if (!request.bound_object_identity.object_uuid.canonical.empty()) {
    return request.bound_object_identity.object_uuid.canonical;
  }
  return {};
}

NameRegistryNameResult SelectUuidToNameCandidate(const EngineApiRequest& request,
                                                 const std::vector<NameRegistryEntry>& candidates,
                                                 const std::string& diagnostic_operation) {
  NameRegistryNameResult result;
  if (candidates.empty()) {
    result.diagnostic = MakeEngineApiDiagnostic("CATALOG.NAME.NOT_FOUND",
                                                StandardNameNotFoundMessageKey(),
                                                "item_not_found_or_does_not_exist");
    return result;
  }

  std::vector<NameRegistryEntry> ordered = candidates;
  std::stable_sort(ordered.begin(), ordered.end(), NameRegistryDisplayOrderLess);

  const auto languages = LanguageCandidates(request.context);
  const std::string requested_profile = ProfileName(NameRegistryDefaultIdentifierProfile(request.context));
  for (const auto& language : languages) {
    for (const auto& candidate : ordered) {
      if (candidate.language_tag == language && candidate.name_class == "primary" &&
          ProfileName(candidate.identifier_profile_uuid) == requested_profile) {
        result.ok = true;
        result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
        result.entry = candidate;
        return result;
      }
    }
    for (const auto& candidate : ordered) {
      if (candidate.language_tag == language && ProfileName(candidate.identifier_profile_uuid) == requested_profile) {
        result.ok = true;
        result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
        result.entry = candidate;
        return result;
      }
    }
    for (const auto& candidate : ordered) {
      if (candidate.language_tag == language && candidate.name_class == "primary") {
        result.ok = true;
        result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
        result.entry = candidate;
        return result;
      }
    }
    for (const auto& candidate : ordered) {
      if (candidate.language_tag == language) {
        result.ok = true;
        result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
        result.entry = candidate;
        return result;
      }
    }
  }

  for (const auto& candidate : ordered) {
    if (candidate.name_class == "primary") {
      result.ok = true;
      result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
      result.entry = candidate;
      return result;
    }
  }

  result.ok = true;
  result.diagnostic = MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
  result.entry = ordered.front();
  (void)diagnostic_operation;
  return result;
}

}  // namespace

NameRegistryNameResult MapNameRegistryUuidToNamePrivate(const EngineApiRequest& request,
                                                        const std::string& object_uuid,
                                                        const std::string& requested_object_class) {
  const std::string target_uuid = UuidToNameTargetUuid(request, object_uuid);
  if (target_uuid.empty()) {
    NameRegistryNameResult result;
    result.diagnostic = MakeInvalidRequestDiagnostic("catalog.map_uuid_to_name", "object_uuid_required");
    return result;
  }

  const auto loaded = LoadNameRegistryState(request.context, request.context.local_transaction_id);
  if (!loaded.ok) {
    NameRegistryNameResult result;
    result.diagnostic = loaded.diagnostic;
    return result;
  }

  std::vector<NameRegistryEntry> candidates;
  for (const auto& entry : loaded.state.entries) {
    if (entry.object_uuid != target_uuid) { continue; }
    if (!ObjectClassMatchesRequest(entry.object_class, requested_object_class)) { continue; }
    if (entry.deleted || entry.lifecycle_state != "active") { continue; }
    candidates.push_back(entry);
  }
  return SelectUuidToNameCandidate(request, candidates, "catalog.map_uuid_to_name.private");
}

NameRegistryNameResult MapNameRegistryUuidToNamePublic(const EngineApiRequest& request,
                                                       const std::string& object_uuid,
                                                       const std::string& requested_object_class) {
  const std::string target_uuid = UuidToNameTargetUuid(request, object_uuid);
  if (target_uuid.empty()) {
    NameRegistryNameResult result;
    result.diagnostic = MakeInvalidRequestDiagnostic("catalog.map_uuid_to_name", "object_uuid_required");
    return result;
  }

  const auto loaded = LoadNameRegistryState(request.context, request.context.local_transaction_id);
  if (!loaded.ok) {
    NameRegistryNameResult result;
    result.diagnostic = loaded.diagnostic;
    return result;
  }

  std::vector<NameRegistryEntry> candidates;
  for (const auto& entry : loaded.state.entries) {
    if (entry.object_uuid != target_uuid) { continue; }
    if (!ObjectClassMatchesRequest(entry.object_class, requested_object_class)) { continue; }
    if (entry.deleted || entry.lifecycle_state != "active") { continue; }
    if (!PublicNameRegistryEntryVisible(request, entry)) { continue; }
    candidates.push_back(entry);
  }
  return SelectUuidToNameCandidate(request, candidates, "catalog.map_uuid_to_name.public");
}

NameRegistryNameResult MapNameRegistryUuidToName(const EngineApiRequest& request,
                                                 const std::string& object_uuid,
                                                 const std::string& requested_object_class) {
  return MapNameRegistryUuidToNamePublic(request, object_uuid, requested_object_class);
}

bool NameRegistryWouldConflict(const EngineRequestContext& context,
                               const std::string& object_uuid,
                               const std::string& object_class,
                               const std::string& scope_uuid,
                               const std::vector<EngineLocalizedName>& names,
                               std::uint64_t observer_tx,
                               std::string* conflict_name) {
  const auto loaded = LoadNameRegistryState(context, observer_tx);
  if (!loaded.ok) { return false; }
  const std::string default_language = NameRegistryDefaultLanguage(context);
  for (const auto& name : names) {
    const auto wanted = MakeNameRegistryEntry(context, object_uuid, object_class, scope_uuid, name, name.name);
    for (const auto& entry : loaded.state.entries) {
      if (entry.object_uuid == object_uuid) { continue; }
      if (entry.scope_uuid != scope_uuid) { continue; }
      if (ProfileName(entry.identifier_profile_uuid) != ProfileName(wanted.identifier_profile_uuid)) { continue; }
      if (!NameRegistryCollisionScopeMatches(entry, wanted, default_language)) { continue; }
      if (NameRegistryLookupKeysCollide(wanted, entry)) {
        if (conflict_name) { *conflict_name = wanted.display_name; }
        return true;
      }
    }
  }
  return false;
}

}  // namespace scratchbird::engine::internal_api
