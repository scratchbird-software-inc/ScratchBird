// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_object_lifecycle.hpp"

#include "catalog/name_registry.hpp"
#include "crud_support/crud_store.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

std::string EventPath(const EngineRequestContext& context) {
  return context.database_path + ".sb.catalog_object_events";
}

std::vector<std::string> Split(const std::string& value, char delimiter) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream in(value);
  while (std::getline(in, current, delimiter)) { parts.push_back(current); }
  return parts;
}

std::string HexEncode(const std::string& value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(value.size() * 2);
  for (unsigned char c : value) {
    out.push_back(kHex[(c >> 4) & 0x0f]);
    out.push_back(kHex[c & 0x0f]);
  }
  return out;
}

int HexValue(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
  if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
  return -1;
}

std::string HexDecode(const std::string& value) {
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

std::uint64_t ParseU64(const std::string& value) {
  try {
    return static_cast<std::uint64_t>(std::stoull(value));
  } catch (...) {
    return 0;
  }
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "TRUE";
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string LowerAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  return value;
}

std::string UpperAscii(std::string value) {
  for (char& c : value) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
  return value;
}

std::string ProfileName(const std::string& profile) {
  return profile.empty() ? std::string("sbsql_v3") : LowerAscii(profile);
}

bool ProfileFoldsLower(const std::string& profile) {
  const std::string p = ProfileName(profile);
  return p.find("postgres") != std::string::npos || p.find("cockroach") != std::string::npos ||
         p.find("yugabyte") != std::string::npos || p == "postgresql_family";
}

bool ProfileFoldsUpper(const std::string& profile) {
  const std::string p = ProfileName(profile);
  return p.empty() || p == "sbsql_v3" || p == "native" || p == "sbsql" ||
         p.find("firebird") != std::string::npos;
}

std::string LookupKey(std::string text, const std::string& profile, bool exact) {
  if (exact) { return text; }
  if (ProfileFoldsLower(profile)) { return LowerAscii(std::move(text)); }
  if (ProfileFoldsUpper(profile)) { return UpperAscii(std::move(text)); }
  return text;
}

std::string DefaultLanguageTag(const EngineRequestContext& context) {
  return context.language_context.default_language_tag.empty()
             ? std::string("en")
             : context.language_context.default_language_tag;
}

bool NameClassIsCanonical(const std::string& name_class) {
  return name_class.empty() || name_class == "primary" || name_class == "default" ||
         name_class == "canonical";
}

bool DefaultLanguageCanonicalName(const EngineCatalogNameRecord& name,
                                  const std::string& default_language) {
  return name.language_tag == default_language && NameClassIsCanonical(name.name_class);
}

bool AliasScopedName(const EngineCatalogNameRecord& name,
                     const std::string& default_language) {
  return name.language_tag != default_language || !NameClassIsCanonical(name.name_class);
}

bool CollisionScopeMatches(const EngineCatalogNameRecord& existing,
                           const EngineCatalogNameRecord& wanted,
                           const std::string& default_language) {
  if (existing.language_tag == wanted.language_tag) { return true; }
  if (DefaultLanguageCanonicalName(existing, default_language) ||
      DefaultLanguageCanonicalName(wanted, default_language)) {
    return true;
  }
  return AliasScopedName(existing, default_language) ||
         AliasScopedName(wanted, default_language);
}

bool LookupKeysCollide(const EngineCatalogNameRecord& wanted,
                       const EngineCatalogNameRecord& existing) {
  if (!wanted.requires_exact_match && existing.requires_exact_match) { return false; }
  const std::string left_key = wanted.requires_exact_match ? wanted.exact_lookup_key
                                                           : wanted.normalized_lookup_key;
  const std::string right_key = wanted.requires_exact_match ? existing.exact_lookup_key
                                                            : existing.normalized_lookup_key;
  return left_key == right_key;
}

EngineApiDiagnostic CatalogDiagnostic(const char* code, std::string detail = {}) {
  std::string message_key = "catalog.object.lifecycle";
  if (code == std::string(kCatalogObjectDiagnosticUuidRequired)) { message_key = "catalog.object.uuid_required"; }
  else if (code == std::string(kCatalogObjectDiagnosticKindRequired)) { message_key = "catalog.object.kind_required"; }
  else if (code == std::string(kCatalogObjectDiagnosticSchemaUuidRequired)) { message_key = "catalog.object.schema_uuid_required"; }
  else if (code == std::string(kCatalogObjectDiagnosticNameRequired)) { message_key = "catalog.object.name_required"; }
  else if (code == std::string(kCatalogObjectDiagnosticDuplicateName)) { message_key = "catalog.object.duplicate_name"; }
  else if (code == std::string(kCatalogObjectDiagnosticNotFound)) { message_key = "catalog.object.not_found"; }
  else if (code == std::string(kCatalogObjectDiagnosticNameNotFound)) { message_key = "catalog.object.name_not_found"; }
  else if (code == std::string(kCatalogObjectDiagnosticDependencyTargetNotVisible)) { message_key = "catalog.object.dependency_target_not_visible"; }
  else if (code == std::string(kCatalogObjectDiagnosticDependencyBlockedDrop)) { message_key = "catalog.object.dependency_blocked_drop"; }
  else if (code == std::string(kCatalogObjectDiagnosticSchemaOwnerDenied)) { message_key = "catalog.object.schema_owner_denied"; }
  else if (code == std::string(kCatalogObjectDiagnosticMgaTransactionRequired)) { message_key = "catalog.object.mga_transaction_required"; }
  else if (code == std::string(kCatalogObjectDiagnosticMgaVisibilityRefused)) { message_key = "catalog.object.mga_visibility_refused"; }
  else if (code == std::string(kCatalogObjectDiagnosticCacheEpochStale)) { message_key = "catalog.object.cache_epoch_stale"; }
  else if (code == std::string(kCatalogObjectDiagnosticDatabasePathRequired)) { message_key = "catalog.object.database_path_required"; }
  else if (code == std::string(kCatalogObjectDiagnosticDatabaseWriteFailed)) { message_key = "catalog.object.database_write_failed"; }
  else if (code == std::string(kCatalogSynonymDiagnosticTargetMissing)) { message_key = "catalog.synonym.target_missing"; }
  else if (code == std::string(kCatalogSynonymDiagnosticTargetClassMismatch)) { message_key = "catalog.synonym.target_class_mismatch"; }
  else if (code == std::string(kCatalogSynonymDiagnosticCycle)) { message_key = "catalog.synonym.cycle"; }
  else if (code == std::string(kCatalogSynonymDiagnosticDepthExceeded)) { message_key = "catalog.synonym.depth_exceeded"; }
  else if (code == std::string(kCatalogSynonymDiagnosticParentNotAllowed)) { message_key = "catalog.synonym.parent_not_allowed"; }
  else if (code == std::string(kCatalogSynonymDiagnosticNameConflict)) { message_key = "catalog.synonym.name_conflict"; }
  else if (code == std::string(kCatalogSynonymDiagnosticDependencyInvalid)) { message_key = "catalog.synonym.dependency_invalid"; }
  else if (code == std::string(kCatalogSynonymDiagnosticPermissionDenied)) { message_key = "catalog.synonym.permission_denied"; }
  else if (code == std::string(kCatalogConstraintDiagnosticUuidRequired)) { message_key = "catalog.constraint.uuid_required"; }
  else if (code == std::string(kCatalogConstraintDiagnosticKindRequired)) { message_key = "catalog.constraint.kind_required"; }
  else if (code == std::string(kCatalogConstraintDiagnosticDescriptorInvalid)) { message_key = "catalog.constraint.descriptor_invalid"; }
  else if (code == std::string(kCatalogConstraintDiagnosticDuplicateName)) { message_key = "catalog.constraint.duplicate_name"; }
  else if (code == std::string(kCatalogConstraintDiagnosticSupportRequired)) { message_key = "catalog.constraint.support_required"; }
  else if (code == std::string(kCatalogConstraintDiagnosticSupportFamilyUnsupported)) { message_key = "catalog.constraint.support_family_unsupported"; }
  else if (code == std::string(kCatalogConstraintDiagnosticDependencyInvalid)) { message_key = "catalog.constraint.dependency_invalid"; }
  return MakeEngineApiDiagnostic(code, std::move(message_key), std::move(detail), true);
}

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

template <typename TResult>
TResult SuccessResult(const EngineRequestContext& context, std::string operation_id) {
  TResult result;
  result.ok = true;
  result.operation_id = std::move(operation_id);
  result.transaction_uuid = context.transaction_uuid;
  result.local_transaction_id = context.local_transaction_id;
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  return result;
}

template <typename TResult>
TResult DiagnosticResult(const EngineRequestContext& context,
                         std::string operation_id,
                         EngineApiDiagnostic diagnostic) {
  TResult result;
  result.ok = false;
  result.operation_id = std::move(operation_id);
  result.transaction_uuid = context.transaction_uuid;
  result.local_transaction_id = context.local_transaction_id;
  result.diagnostics.push_back(std::move(diagnostic));
  result.embedded_trust_mode_observed = context.trust_mode == EngineTrustMode::embedded_in_process;
  return result;
}

EngineTypedValue Value(std::string value) {
  EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  return typed;
}

void AddRow(EngineApiResult* result, std::vector<std::pair<std::string, std::string>> fields) {
  EngineRowValue row;
  row.requested_row_uuid.canonical = GenerateCrudEngineUuid("row");
  for (auto& field : fields) { row.fields.push_back({std::move(field.first), Value(std::move(field.second))}); }
  result->result_shape.result_kind = "catalog_object_lifecycle_rows";
  result->result_shape.rows.push_back(std::move(row));
}

void AddEvidence(EngineApiResult* result, std::string kind, std::string id) {
  result->evidence.push_back({std::move(kind), std::move(id)});
}

bool TransactionStatusVisible(const EngineRequestContext& context,
                              const std::map<std::uint64_t, std::string>& statuses,
                              std::uint64_t creator_tx) {
  const auto it = statuses.find(creator_tx);
  if (it == statuses.end()) { return false; }
  if (it->second == "rolled_back" || it->second == "rolling_back" || it->second == "failed_terminal") {
    return false;
  }
  if (creator_tx == context.local_transaction_id &&
      (it->second == "active" || it->second == "preparing" || it->second == "prepared")) {
    return true;
  }
  if (it->second != "committed" && it->second != "archived") { return false; }
  if (context.snapshot_visible_through_local_transaction_id != 0) {
    return creator_tx <= context.snapshot_visible_through_local_transaction_id;
  }
  if (context.local_transaction_id != 0) { return creator_tx <= context.local_transaction_id; }
  return true;
}

bool EventVisible(const EngineRequestContext& context,
                  const CrudState* crud_state,
                  std::uint64_t creator_tx) {
  if (creator_tx == 0) { return true; }
  if (crud_state != nullptr && !crud_state->transactions.empty()) {
    return TransactionStatusVisible(context, crud_state->transactions, creator_tx);
  }
  if (context.local_transaction_id != 0 && creator_tx == context.local_transaction_id) { return true; }
  if (context.snapshot_visible_through_local_transaction_id != 0) {
    return creator_tx <= context.snapshot_visible_through_local_transaction_id;
  }
  if (context.local_transaction_id != 0) { return creator_tx <= context.local_transaction_id; }
  return false;
}

std::string PrimaryName(const EngineApiRequest& request) {
  for (const auto& name : request.localized_names) {
    if (name.default_name && !name.name.empty()) { return name.name; }
  }
  for (const auto& name : request.localized_names) {
    if (!name.name.empty()) { return name.name; }
  }
  if (!request.sql_object_reference.object_name.raw_text.empty()) {
    return request.sql_object_reference.object_name.raw_text;
  }
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, "name:")) { return option.substr(5); }
  }
  return {};
}

std::vector<EngineLocalizedName> EffectiveNames(const EngineApiRequest& request) {
  std::vector<EngineLocalizedName> names = request.localized_names;
  if (!request.sql_object_reference.object_name.raw_text.empty()) {
    EngineLocalizedName name;
    name.language_tag = request.context.language_context.language_tag.empty()
                            ? "en"
                            : request.context.language_context.language_tag;
    name.name_class = "primary";
    name.name = request.sql_object_reference.object_name.raw_text;
    name.raw_name_text = request.sql_object_reference.object_name.raw_text;
    name.display_name = request.sql_object_reference.object_name.raw_text;
    name.was_quoted = request.sql_object_reference.object_name.was_quoted;
    name.quote_style = request.sql_object_reference.object_name.quote_style;
    name.requires_exact_match = request.sql_object_reference.object_name.requires_exact_match ||
                                request.sql_object_reference.object_name.was_quoted;
    name.identifier_profile_uuid = request.sql_object_reference.object_name.identifier_profile_uuid;
    name.default_name = true;
    names.push_back(std::move(name));
  }
  const std::string option_name = PrimaryName(request);
  if (names.empty() && !option_name.empty()) {
    EngineLocalizedName name;
    name.language_tag = request.context.language_context.language_tag.empty()
                            ? "en"
                            : request.context.language_context.language_tag;
    name.name_class = "primary";
    name.name = option_name;
    name.raw_name_text = option_name;
    name.display_name = option_name;
    name.default_name = true;
    names.push_back(std::move(name));
  }
  return names;
}

std::string PayloadFromRequest(const EngineApiRequest& request) {
  std::vector<std::string> parts;
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, "payload:")) { parts.push_back(option.substr(8)); }
  }
  if (!request.descriptors.empty()) { parts.push_back("descriptor=" + request.descriptors.front().canonical_type_name); }
  if (!request.columns.empty()) { parts.push_back("column_count=" + std::to_string(request.columns.size())); }
  return [&parts] {
    std::string out;
    for (const auto& part : parts) {
      if (!out.empty()) { out.push_back(';'); }
      out += part;
    }
    return out;
  }();
}

std::map<std::string, std::string> PayloadMap(const std::string& payload) {
  std::map<std::string, std::string> fields;
  for (const auto& part : Split(payload, ';')) {
    const auto pos = part.find('=');
    if (pos == std::string::npos) { continue; }
    fields[part.substr(0, pos)] = part.substr(pos + 1);
  }
  return fields;
}

std::string SynonymPayload(const EngineApiRequest& request,
                           const std::string& target_uuid,
                           const std::string& target_class) {
  std::vector<std::string> parts;
  parts.push_back("synonym_target_uuid=" + target_uuid);
  parts.push_back("synonym_target_class=" + target_class);
  parts.push_back("synonym_dependency_strength=hard");
  parts.push_back("synonym_catalog_table=sys.catalog.synonym");
  for (const auto& option : request.option_envelopes) {
    if (StartsWith(option, "payload:policy_uuid=")) {
      parts.push_back(option.substr(8));
    }
  }
  std::string out;
  for (const auto& part : parts) {
    if (!out.empty()) { out.push_back(';'); }
    out += part;
  }
  return out;
}

std::string SynonymTargetUuid(const EngineCatalogObjectRecord& object) {
  if (object.object_kind != "synonym") { return {}; }
  const auto fields = PayloadMap(object.payload);
  const auto it = fields.find("synonym_target_uuid");
  return it == fields.end() ? std::string{} : it->second;
}

std::string SynonymTargetClass(const EngineCatalogObjectRecord& object) {
  if (object.object_kind != "synonym") { return {}; }
  const auto fields = PayloadMap(object.payload);
  const auto it = fields.find("synonym_target_class");
  return it == fields.end() ? std::string{} : it->second;
}

std::string DependencyKindForRelatedObject(const std::string& source_kind,
                                           const EngineObjectReference& related) {
  if (source_kind == "synonym") {
    return std::string("synonym_hard:") + related.object_kind;
  }
  return related.object_kind;
}

bool ParentCapableKind(const std::string& object_kind) {
  return object_kind == "schema" || object_kind == "package" ||
         object_kind == "table" || object_kind == "view";
}

std::string PayloadField(const std::map<std::string, std::string>& fields,
                         const std::string& key,
                         std::string fallback = {}) {
  const auto it = fields.find(key);
  if (it == fields.end() || it->second.empty()) { return fallback; }
  return it->second;
}

bool PayloadBoolField(const std::map<std::string, std::string>& fields,
                      const std::string& key,
                      bool fallback) {
  const auto it = fields.find(key);
  return it == fields.end() ? fallback : ParseBool(it->second);
}

bool ConstraintClassRequiresSupport(const std::string& constraint_class) {
  return constraint_class == "primary_key" || constraint_class == "unique_key" ||
         constraint_class == "foreign_key" || constraint_class == "exclusion_constraint";
}

bool SupportFamilyAllowedForConstraint(const std::string& constraint_class,
                                       const std::string& support_family) {
  if (!ConstraintClassRequiresSupport(constraint_class)) { return true; }
  if (support_family.empty()) { return false; }
  if (constraint_class == "primary_key" || constraint_class == "unique_key") {
    return support_family == "btree" || support_family == "hash" ||
           support_family == "rowstore_scalar_btree_v1";
  }
  if (constraint_class == "foreign_key") {
    return support_family == "btree" || support_family == "hash" ||
           support_family == "rowstore_scalar_btree_v1" ||
           support_family == "foreign_key_helper";
  }
  if (constraint_class == "exclusion_constraint") {
    return support_family == "btree" || support_family == "range" ||
           support_family == "spatial" || support_family == "graph" ||
           support_family == "search" || support_family == "document" ||
           support_family == "vector_exact" || support_family == "vector_hnsw";
  }
  return true;
}

std::string RequestedOrGeneratedUuid(const EngineUuid& uuid, const std::string& kind) {
  if (!uuid.canonical.empty()) { return uuid.canonical; }
  return GenerateCrudEngineUuid(kind);
}

std::string ObjectKind(const EngineApiRequest& request) {
  if (!request.target_object.object_kind.empty()) { return request.target_object.object_kind; }
  if (!request.sql_object_reference.expected_object_type.empty()) {
    return request.sql_object_reference.expected_object_type;
  }
  return {};
}

std::string ObjectUuid(const EngineApiRequest& request) {
  if (!request.target_object.uuid.canonical.empty()) { return request.target_object.uuid.canonical; }
  if (!request.bound_object_identity.object_uuid.canonical.empty()) {
    return request.bound_object_identity.object_uuid.canonical;
  }
  return {};
}

EngineCatalogNameRecord MakeNameRecord(const EngineRequestContext& context,
                                       const std::string& object_uuid,
                                       const std::string& object_kind,
                                       const std::string& schema_uuid,
                                       const EngineLocalizedName& name,
                                       std::uint64_t metadata_epoch) {
  const std::string raw = !name.raw_name_text.empty() ? name.raw_name_text : name.name;
  const std::string profile = name.identifier_profile_uuid.empty()
                                  ? (context.identifier_profile_uuid.empty() ? "sbsql_v3" : context.identifier_profile_uuid)
                                  : name.identifier_profile_uuid;
  const bool exact = name.requires_exact_match || name.was_quoted;
  EngineCatalogNameRecord record;
  record.creator_tx = context.local_transaction_id;
  record.name_entry_uuid = GenerateCrudEngineUuid("object");
  record.object_uuid = object_uuid;
  record.object_kind = object_kind;
  record.schema_uuid = schema_uuid;
  record.language_tag = name.language_tag.empty()
                            ? (context.language_context.language_tag.empty() ? "en" : context.language_context.language_tag)
                            : name.language_tag;
  record.name_class = name.name_class.empty() ? "primary" : name.name_class;
  record.identifier_profile_uuid = profile;
  record.raw_name_text = raw;
  record.display_name = name.display_name.empty() ? raw : name.display_name;
  record.normalized_lookup_key = name.normalized_lookup_key.empty()
                                     ? LookupKey(raw, profile, false)
                                     : name.normalized_lookup_key;
  record.exact_lookup_key = name.exact_lookup_key.empty()
                                ? LookupKey(raw, profile, true)
                                : name.exact_lookup_key;
  record.requires_exact_match = exact;
  record.metadata_epoch = metadata_epoch;
  return record;
}

std::string ObjectEvent(const EngineCatalogObjectRecord& record) {
  return std::string(kCatalogObjectLifecycleEventMagic) + "\tOBJECT\t" +
         std::to_string(record.creator_tx) + "\t" + record.object_uuid + "\t" + record.object_kind + "\t" +
         record.schema_uuid + "\t" + record.owner_principal_uuid + "\t" + record.lifecycle_state + "\t" +
         std::to_string(record.definition_epoch) + "\t" + std::to_string(record.metadata_epoch) + "\t" +
         HexEncode(record.payload) + "\t" + (record.deleted ? "1" : "0");
}

std::string NameEvent(const EngineCatalogNameRecord& record) {
  return std::string(kCatalogObjectLifecycleEventMagic) + "\tNAME\t" +
         std::to_string(record.creator_tx) + "\t" + record.name_entry_uuid + "\t" + record.object_uuid + "\t" +
         record.object_kind + "\t" + record.schema_uuid + "\t" + record.language_tag + "\t" +
         record.name_class + "\t" + record.identifier_profile_uuid + "\t" + HexEncode(record.raw_name_text) + "\t" +
         HexEncode(record.display_name) + "\t" + HexEncode(record.normalized_lookup_key) + "\t" +
         HexEncode(record.exact_lookup_key) + "\t" + (record.requires_exact_match ? "1" : "0") + "\t" +
         std::to_string(record.metadata_epoch) + "\t" + (record.deleted ? "1" : "0");
}

std::string DependencyEvent(const EngineCatalogDependencyRecord& record) {
  return std::string(kCatalogObjectLifecycleEventMagic) + "\tDEPENDENCY\t" +
         std::to_string(record.creator_tx) + "\t" + record.source_uuid + "\t" + record.source_kind + "\t" +
         record.dependency_uuid + "\t" + record.dependency_kind + "\t" +
         std::to_string(record.metadata_epoch) + "\t" + (record.deleted ? "1" : "0");
}

std::string ColumnMetadataEvent(const EngineCatalogColumnMetadataRecord& record) {
  return std::string(kCatalogObjectLifecycleEventMagic) + "\tCOLUMN_METADATA\t" +
         std::to_string(record.creator_tx) + "\t" + record.column_uuid + "\t" +
         record.owner_object_uuid + "\t" + HexEncode(record.descriptor_kind) + "\t" +
         HexEncode(record.canonical_type_name) + "\t" +
         HexEncode(record.default_expression_envelope) + "\t" +
         std::to_string(record.ordinal) + "\t" + (record.nullable ? "1" : "0") + "\t" +
         std::to_string(record.metadata_epoch) + "\t" + (record.deleted ? "1" : "0");
}

std::string ConstraintDescriptorEvent(const EngineCatalogConstraintDescriptorRecord& record) {
  return std::string(kCatalogObjectLifecycleEventMagic) + "\tCONSTRAINT_DESCRIPTOR\t" +
         std::to_string(record.creator_tx) + "\t" + record.constraint_uuid + "\t" +
         record.constraint_class + "\t" + record.owner_object_uuid + "\t" +
         record.name_ref_uuid + "\t" + record.constraint_policy_version_uuid + "\t" +
         record.enforcement_timing + "\t" + record.validation_state + "\t" +
         record.trust_state + "\t" + record.support_requirement + "\t" +
         record.predicate_sblr_uuid + "\t" + record.diagnostic_profile_uuid + "\t" +
         record.metrics_profile_uuid + "\t" + record.conformance_profile_uuid + "\t" +
         HexEncode(record.constraint_hash) + "\t" +
         HexEncode(record.canonical_constraint_envelope) + "\t" +
         std::to_string(record.metadata_epoch) + "\t" + (record.deleted ? "1" : "0");
}

std::string KeyDescriptorEvent(const EngineCatalogKeyDescriptorRecord& record) {
  return std::string(kCatalogObjectLifecycleEventMagic) + "\tKEY_DESCRIPTOR\t" +
         std::to_string(record.creator_tx) + "\t" + record.key_descriptor_uuid + "\t" +
         record.constraint_uuid + "\t" + record.key_class + "\t" + record.owner_object_uuid + "\t" +
         HexEncode(record.component_order_hash) + "\t" +
         HexEncode(record.comparison_profile_hash) + "\t" + record.null_policy + "\t" +
         record.canonical_encoding_uuid + "\t" +
         (record.candidate_reference_allowed ? "1" : "0") + "\t" + record.key_state + "\t" +
         HexEncode(record.key_hash) + "\t" + std::to_string(record.metadata_epoch) + "\t" +
         (record.deleted ? "1" : "0");
}

std::string ConstraintSubjectEvent(const EngineCatalogConstraintSubjectRecord& record) {
  return std::string(kCatalogObjectLifecycleEventMagic) + "\tCONSTRAINT_SUBJECT\t" +
         std::to_string(record.creator_tx) + "\t" + record.subject_uuid + "\t" +
         record.constraint_uuid + "\t" + record.subject_kind + "\t" +
         record.subject_object_uuid + "\t" + HexEncode(record.subject_descriptor) + "\t" +
         record.expression_sblr_uuid + "\t" + std::to_string(record.ordinal) + "\t" +
         std::to_string(record.metadata_epoch) + "\t" + (record.deleted ? "1" : "0");
}

std::string ConstraintDependencyEvent(const EngineCatalogConstraintDependencyRecord& record) {
  return std::string(kCatalogObjectLifecycleEventMagic) + "\tCONSTRAINT_DEPENDENCY\t" +
         std::to_string(record.creator_tx) + "\t" + record.dependency_uuid + "\t" +
         record.constraint_uuid + "\t" + record.dependency_kind + "\t" +
         record.dependency_object_uuid + "\t" + record.dependency_version_uuid + "\t" +
         record.invalidation_action + "\t" + HexEncode(record.dependency_hash) + "\t" +
         std::to_string(record.metadata_epoch) + "\t" + (record.deleted ? "1" : "0");
}

std::string ConstraintSupportStructureEvent(const EngineCatalogConstraintSupportStructureRecord& record) {
  return std::string(kCatalogObjectLifecycleEventMagic) + "\tCONSTRAINT_SUPPORT\t" +
         std::to_string(record.creator_tx) + "\t" + record.support_binding_uuid + "\t" +
         record.constraint_uuid + "\t" + record.support_uuid + "\t" +
         record.support_class + "\t" + record.support_family + "\t" +
         HexEncode(record.coverage_scope_hash) + "\t" + record.durability_class + "\t" +
         record.residency_class + "\t" + record.validity_state + "\t" +
         record.enforcement_role + "\t" + HexEncode(record.binding_hash) + "\t" +
         std::to_string(record.metadata_epoch) + "\t" + (record.deleted ? "1" : "0");
}

std::string RetireNamesEvent(std::uint64_t tx, const std::string& object_uuid, std::uint64_t metadata_epoch) {
  return std::string(kCatalogObjectLifecycleEventMagic) + "\tRETIRE_NAMES\t" +
         std::to_string(tx) + "\t" + object_uuid + "\t" + std::to_string(metadata_epoch);
}

std::string CacheInvalidateEvent(const EngineRequestContext& context,
                                 const std::string& operation_id,
                                 const std::string& object_uuid,
                                 std::uint64_t metadata_epoch) {
  return std::string(kCatalogObjectLifecycleEventMagic) + "\tCACHE_INVALIDATE\t" +
         std::to_string(context.local_transaction_id) + "\t" + HexEncode(operation_id) + "\t" +
         object_uuid + "\t" + std::to_string(metadata_epoch) + "\t" +
         std::to_string(context.name_resolution_epoch) + "\t" + std::to_string(context.resource_epoch);
}

EngineApiDiagnostic AppendEvent(const EngineRequestContext& context, const std::string& event) {
  if (context.database_path.empty()) {
    return CatalogDiagnostic(kCatalogObjectDiagnosticDatabasePathRequired, "database_path");
  }
  std::ofstream out(EventPath(context), std::ios::binary | std::ios::app);
  if (!out) { return CatalogDiagnostic(kCatalogObjectDiagnosticDatabaseWriteFailed, "open"); }
  out << event << '\n';
  out.flush();
  if (!out) { return CatalogDiagnostic(kCatalogObjectDiagnosticDatabaseWriteFailed, "flush"); }
  return OkDiagnostic();
}

EngineApiDiagnostic AppendCacheInvalidation(const EngineRequestContext& context,
                                            const std::string& operation_id,
                                            const std::string& object_uuid,
                                            std::uint64_t metadata_epoch) {
  return AppendEvent(context, CacheInvalidateEvent(context, operation_id, object_uuid, metadata_epoch));
}

struct LoadOptions {
  bool enforce_visibility = true;
};

std::string CatalogLifecycleFileFingerprint(const std::string& path) {
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

std::string CatalogLifecycleLoadCacheKey(const EngineRequestContext& context,
                                         LoadOptions options) {
  std::ostringstream key;
  key << "db=" << context.database_path
      << "|visible=" << (options.enforce_visibility ? "1" : "0")
      << "|local_tx=" << context.local_transaction_id
      << "|catalog=" << context.catalog_generation_id
      << "|security=" << context.security_epoch
      << "|resource=" << context.resource_epoch
      << "|name_resolution=" << context.name_resolution_epoch
      << "|events=" << CatalogLifecycleFileFingerprint(EventPath(context))
      << "|crud=" << CatalogLifecycleFileFingerprint(context.database_path + ".sb.crud_events");
  return key.str();
}

std::mutex& CatalogLifecycleLoadCacheMutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, EngineLoadCatalogObjectLifecycleStateResult>&
CatalogLifecycleLoadCache() {
  static std::map<std::string, EngineLoadCatalogObjectLifecycleStateResult> cache;
  return cache;
}

std::optional<EngineLoadCatalogObjectLifecycleStateResult>
LookupCatalogLifecycleLoadCache(const std::string& cache_key) {
  std::lock_guard<std::mutex> guard(CatalogLifecycleLoadCacheMutex());
  const auto found = CatalogLifecycleLoadCache().find(cache_key);
  if (found == CatalogLifecycleLoadCache().end()) return std::nullopt;
  return found->second;
}

void StoreCatalogLifecycleLoadCache(
    const std::string& cache_key,
    const EngineLoadCatalogObjectLifecycleStateResult& result) {
  if (cache_key.empty() || !result.ok) return;
  std::lock_guard<std::mutex> guard(CatalogLifecycleLoadCacheMutex());
  auto& cache = CatalogLifecycleLoadCache();
  cache[cache_key] = result;
  constexpr std::size_t kMaxCatalogLifecycleLoadCacheEntries = 64;
  while (cache.size() > kMaxCatalogLifecycleLoadCacheEntries) {
    cache.erase(cache.begin());
  }
}

EngineLoadCatalogObjectLifecycleStateResult LoadState(const EngineRequestContext& context,
                                                      LoadOptions options) {
  EngineLoadCatalogObjectLifecycleStateResult result;
  if (context.database_path.empty()) {
    result.diagnostic = CatalogDiagnostic(kCatalogObjectDiagnosticDatabasePathRequired, "database_path");
    return result;
  }
  const std::string load_cache_key =
      CatalogLifecycleLoadCacheKey(context, options);
  if (auto cached = LookupCatalogLifecycleLoadCache(load_cache_key)) {
    return *cached;
  }
  std::ifstream in(EventPath(context), std::ios::binary);
  if (!in) {
    result.ok = true;
    result.diagnostic = OkDiagnostic();
    StoreCatalogLifecycleLoadCache(load_cache_key, result);
    return result;
  }
  const auto crud_state = LoadCrudState(context);
  const CrudState* transaction_state = crud_state.ok ? &crud_state.state : nullptr;

  std::map<std::string, EngineCatalogObjectRecord> objects;
  std::vector<EngineCatalogNameRecord> names;
  std::map<std::string, EngineCatalogDependencyRecord> dependencies;
  std::map<std::string, EngineCatalogColumnMetadataRecord> columns;
  std::map<std::string, EngineCatalogConstraintDescriptorRecord> constraints;
  std::map<std::string, EngineCatalogKeyDescriptorRecord> key_descriptors;
  std::map<std::string, EngineCatalogConstraintSubjectRecord> constraint_subjects;
  std::map<std::string, EngineCatalogConstraintDependencyRecord> constraint_dependencies;
  std::map<std::string, EngineCatalogConstraintSupportStructureRecord> constraint_supports;
  std::set<std::string> retired_objects;
  std::uint64_t event_sequence = 0;
  std::string line;
  while (std::getline(in, line)) {
    ++event_sequence;
    if (!StartsWith(line, kCatalogObjectLifecycleEventMagic)) { continue; }
    const auto parts = Split(line, '\t');
    if (parts.size() < 2) { continue; }
    const std::string& event = parts[1];
    const std::uint64_t creator_tx = parts.size() >= 3 ? ParseU64(parts[2]) : 0;
    if (options.enforce_visibility && !EventVisible(context, transaction_state, creator_tx)) { continue; }
    if (event == "OBJECT" && parts.size() >= 12) {
      EngineCatalogObjectRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.object_uuid = parts[3];
      record.object_kind = parts[4];
      record.schema_uuid = parts[5];
      record.owner_principal_uuid = parts[6];
      record.lifecycle_state = parts[7].empty() ? "active" : parts[7];
      record.definition_epoch = ParseU64(parts[8]);
      record.metadata_epoch = ParseU64(parts[9]);
      record.payload = HexDecode(parts[10]);
      record.deleted = ParseBool(parts[11]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      if (record.deleted || record.lifecycle_state == "dropped") {
        retired_objects.insert(record.object_uuid);
        objects.erase(record.object_uuid);
      } else {
        retired_objects.erase(record.object_uuid);
        objects[record.object_uuid] = std::move(record);
      }
    } else if (event == "NAME" && parts.size() >= 17) {
      EngineCatalogNameRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.name_entry_uuid = parts[3];
      record.object_uuid = parts[4];
      record.object_kind = parts[5];
      record.schema_uuid = parts[6];
      record.language_tag = parts[7].empty() ? "en" : parts[7];
      record.name_class = parts[8].empty() ? "primary" : parts[8];
      record.identifier_profile_uuid = parts[9].empty() ? "sbsql_v3" : parts[9];
      record.raw_name_text = HexDecode(parts[10]);
      record.display_name = HexDecode(parts[11]);
      record.normalized_lookup_key = HexDecode(parts[12]);
      record.exact_lookup_key = HexDecode(parts[13]);
      record.requires_exact_match = ParseBool(parts[14]);
      record.metadata_epoch = ParseU64(parts[15]);
      record.deleted = ParseBool(parts[16]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      result.state.name_resolution_epoch = std::max(result.state.name_resolution_epoch, record.metadata_epoch);
      if (!record.deleted) { names.push_back(std::move(record)); }
    } else if (event == "RETIRE_NAMES" && parts.size() >= 5) {
      const std::string object_uuid = parts[3];
      const std::uint64_t epoch = ParseU64(parts[4]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, epoch);
      result.state.name_resolution_epoch = std::max(result.state.name_resolution_epoch, epoch);
      names.erase(std::remove_if(names.begin(), names.end(), [&object_uuid](const EngineCatalogNameRecord& name) {
                    return name.object_uuid == object_uuid;
                  }),
                  names.end());
    } else if (event == "DEPENDENCY" && parts.size() >= 9) {
      EngineCatalogDependencyRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.source_uuid = parts[3];
      record.source_kind = parts[4];
      record.dependency_uuid = parts[5];
      record.dependency_kind = parts[6];
      record.metadata_epoch = ParseU64(parts[7]);
      record.deleted = ParseBool(parts[8]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      const std::string key = record.source_uuid + "\t" + record.dependency_uuid + "\t" + record.dependency_kind;
      if (record.deleted) {
        dependencies.erase(key);
      } else {
        dependencies[key] = std::move(record);
      }
    } else if (event == "COLUMN_METADATA" && parts.size() >= 12) {
      EngineCatalogColumnMetadataRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.column_uuid = parts[3];
      record.owner_object_uuid = parts[4];
      record.descriptor_kind = HexDecode(parts[5]);
      record.canonical_type_name = HexDecode(parts[6]);
      record.default_expression_envelope = HexDecode(parts[7]);
      record.ordinal = static_cast<std::uint32_t>(ParseU64(parts[8]));
      record.nullable = ParseBool(parts[9]);
      record.metadata_epoch = ParseU64(parts[10]);
      record.deleted = ParseBool(parts[11]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      if (record.deleted) {
        columns.erase(record.column_uuid);
      } else {
        columns[record.column_uuid] = std::move(record);
      }
    } else if (event == "CONSTRAINT_DESCRIPTOR" && parts.size() >= 20) {
      EngineCatalogConstraintDescriptorRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.constraint_uuid = parts[3];
      record.constraint_class = parts[4];
      record.owner_object_uuid = parts[5];
      record.name_ref_uuid = parts[6];
      record.constraint_policy_version_uuid = parts[7];
      record.enforcement_timing = parts[8].empty() ? "immediate" : parts[8];
      record.validation_state = parts[9].empty() ? "unvalidated" : parts[9];
      record.trust_state = parts[10].empty() ? "untrusted" : parts[10];
      record.support_requirement = parts[11].empty() ? "optional" : parts[11];
      record.predicate_sblr_uuid = parts[12];
      record.diagnostic_profile_uuid = parts[13];
      record.metrics_profile_uuid = parts[14];
      record.conformance_profile_uuid = parts[15];
      record.constraint_hash = HexDecode(parts[16]);
      record.canonical_constraint_envelope = HexDecode(parts[17]);
      record.metadata_epoch = ParseU64(parts[18]);
      record.deleted = ParseBool(parts[19]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      if (record.deleted) {
        constraints.erase(record.constraint_uuid);
      } else {
        constraints[record.constraint_uuid] = std::move(record);
      }
    } else if (event == "KEY_DESCRIPTOR" && parts.size() >= 16) {
      EngineCatalogKeyDescriptorRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.key_descriptor_uuid = parts[3];
      record.constraint_uuid = parts[4];
      record.key_class = parts[5];
      record.owner_object_uuid = parts[6];
      record.component_order_hash = HexDecode(parts[7]);
      record.comparison_profile_hash = HexDecode(parts[8]);
      record.null_policy = parts[9].empty() ? "not_applicable" : parts[9];
      record.canonical_encoding_uuid = parts[10];
      record.candidate_reference_allowed = ParseBool(parts[11]);
      record.key_state = parts[12].empty() ? "active" : parts[12];
      record.key_hash = HexDecode(parts[13]);
      record.metadata_epoch = ParseU64(parts[14]);
      record.deleted = ParseBool(parts[15]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      if (record.deleted) {
        key_descriptors.erase(record.key_descriptor_uuid);
      } else {
        key_descriptors[record.key_descriptor_uuid] = std::move(record);
      }
    } else if (event == "CONSTRAINT_SUBJECT" && parts.size() >= 12) {
      EngineCatalogConstraintSubjectRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.subject_uuid = parts[3];
      record.constraint_uuid = parts[4];
      record.subject_kind = parts[5];
      record.subject_object_uuid = parts[6];
      record.subject_descriptor = HexDecode(parts[7]);
      record.expression_sblr_uuid = parts[8];
      record.ordinal = static_cast<std::uint32_t>(ParseU64(parts[9]));
      record.metadata_epoch = ParseU64(parts[10]);
      record.deleted = ParseBool(parts[11]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      if (record.deleted) {
        constraint_subjects.erase(record.subject_uuid);
      } else {
        constraint_subjects[record.subject_uuid] = std::move(record);
      }
    } else if (event == "CONSTRAINT_DEPENDENCY" && parts.size() >= 12) {
      EngineCatalogConstraintDependencyRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.dependency_uuid = parts[3];
      record.constraint_uuid = parts[4];
      record.dependency_kind = parts[5];
      record.dependency_object_uuid = parts[6];
      record.dependency_version_uuid = parts[7];
      record.invalidation_action = parts[8];
      record.dependency_hash = HexDecode(parts[9]);
      record.metadata_epoch = ParseU64(parts[10]);
      record.deleted = ParseBool(parts[11]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      if (record.deleted) {
        constraint_dependencies.erase(record.dependency_uuid);
      } else {
        constraint_dependencies[record.dependency_uuid] = std::move(record);
      }
    } else if (event == "CONSTRAINT_SUPPORT" && parts.size() >= 16) {
      EngineCatalogConstraintSupportStructureRecord record;
      record.event_sequence = event_sequence;
      record.creator_tx = creator_tx;
      record.support_binding_uuid = parts[3];
      record.constraint_uuid = parts[4];
      record.support_uuid = parts[5];
      record.support_class = parts[6];
      record.support_family = parts[7];
      record.coverage_scope_hash = HexDecode(parts[8]);
      record.durability_class = parts[9];
      record.residency_class = parts[10];
      record.validity_state = parts[11];
      record.enforcement_role = parts[12];
      record.binding_hash = HexDecode(parts[13]);
      record.metadata_epoch = ParseU64(parts[14]);
      record.deleted = ParseBool(parts[15]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, record.metadata_epoch);
      if (record.deleted) {
        constraint_supports.erase(record.support_binding_uuid);
      } else {
        constraint_supports[record.support_binding_uuid] = std::move(record);
      }
    } else if (event == "CACHE_INVALIDATE" && parts.size() >= 7) {
      const std::uint64_t epoch = ParseU64(parts[5]);
      result.state.metadata_epoch = std::max(result.state.metadata_epoch, epoch);
      result.state.name_resolution_epoch = std::max(result.state.name_resolution_epoch, ParseU64(parts[6]));
    }
  }
  bool pruned_child = true;
  while (pruned_child) {
    pruned_child = false;
    for (auto it = objects.begin(); it != objects.end();) {
      if (!it->second.schema_uuid.empty() && retired_objects.count(it->second.schema_uuid) != 0) {
        retired_objects.insert(it->second.object_uuid);
        it = objects.erase(it);
        pruned_child = true;
      } else {
        ++it;
      }
    }
  }
  for (auto& [_, object] : objects) { result.state.objects.push_back(std::move(object)); }
  std::set<std::string> active_objects;
  for (const auto& object : result.state.objects) { active_objects.insert(object.object_uuid); }
  for (auto& name : names) {
    if (active_objects.count(name.object_uuid) != 0) { result.state.names.push_back(std::move(name)); }
  }
  for (auto& [_, dependency] : dependencies) {
    if (active_objects.count(dependency.source_uuid) != 0 && active_objects.count(dependency.dependency_uuid) != 0) {
      result.state.dependencies.push_back(std::move(dependency));
    }
  }
  std::set<std::string> active_constraints;
  for (auto& [_, column] : columns) {
    if (active_objects.count(column.owner_object_uuid) != 0) {
      result.state.columns.push_back(std::move(column));
    }
  }
  for (auto& [_, constraint] : constraints) {
    if (active_objects.count(constraint.owner_object_uuid) != 0) {
      active_constraints.insert(constraint.constraint_uuid);
      result.state.constraints.push_back(std::move(constraint));
    }
  }
  for (auto& [_, key] : key_descriptors) {
    const bool constraint_link_ok = key.constraint_uuid.empty() ||
                                    active_constraints.count(key.constraint_uuid) != 0;
    if (constraint_link_ok && active_objects.count(key.owner_object_uuid) != 0) {
      result.state.key_descriptors.push_back(std::move(key));
    }
  }
  for (auto& [_, subject] : constraint_subjects) {
    if (active_constraints.count(subject.constraint_uuid) != 0) {
      result.state.constraint_subjects.push_back(std::move(subject));
    }
  }
  for (auto& [_, dependency] : constraint_dependencies) {
    if (active_constraints.count(dependency.constraint_uuid) != 0) {
      result.state.constraint_dependencies.push_back(std::move(dependency));
    }
  }
  for (auto& [_, support] : constraint_supports) {
    if (active_constraints.count(support.constraint_uuid) != 0) {
      result.state.constraint_support_structures.push_back(std::move(support));
    }
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  StoreCatalogLifecycleLoadCache(load_cache_key, result);
  return result;
}

const EngineCatalogObjectRecord* FindObject(const EngineCatalogObjectLifecycleState& state,
                                            const std::string& object_uuid) {
  for (const auto& object : state.objects) {
    if (object.object_uuid == object_uuid) { return &object; }
  }
  return nullptr;
}

bool PrincipalOwnsSchema(const EngineCatalogObjectLifecycleState& state,
                         const EngineRequestContext& context,
                         const std::string& schema_uuid) {
  if (schema_uuid.empty()) { return true; }
  const auto* schema = FindObject(state, schema_uuid);
  if (schema == nullptr || schema->object_kind != "schema") { return true; }
  if (schema->owner_principal_uuid.empty()) { return true; }
  return schema->owner_principal_uuid == context.principal_uuid.canonical;
}

EngineApiDiagnostic ValidateMutatingContext(const EngineRequestContext& context) {
  if (context.database_path.empty()) {
    return CatalogDiagnostic(kCatalogObjectDiagnosticDatabasePathRequired, "database_path");
  }
  if (context.local_transaction_id == 0) {
    return CatalogDiagnostic(kCatalogObjectDiagnosticMgaTransactionRequired, "local_transaction_id");
  }
  return OkDiagnostic();
}

EngineApiDiagnostic CheckNameConflict(const EngineCatalogObjectLifecycleState& state,
                                      const std::string& object_uuid,
                                      const std::string& object_kind,
                                      const std::string& schema_uuid,
                                      const std::vector<EngineLocalizedName>& names,
                                      const EngineRequestContext& context) {
  const std::string default_language = DefaultLanguageTag(context);
  for (const auto& requested : names) {
    const auto wanted = MakeNameRecord(context, object_uuid, object_kind, schema_uuid, requested, 0);
    for (const auto& existing : state.names) {
      if (existing.object_uuid == object_uuid) { continue; }
      if (existing.schema_uuid != schema_uuid) { continue; }
      if (ProfileName(existing.identifier_profile_uuid) != ProfileName(wanted.identifier_profile_uuid)) { continue; }
      if (!CollisionScopeMatches(existing, wanted, default_language)) { continue; }
      if (LookupKeysCollide(wanted, existing)) {
        return CatalogDiagnostic(object_kind == "synonym" ? kCatalogSynonymDiagnosticNameConflict
                                                          : kCatalogObjectDiagnosticDuplicateName,
                                 existing.object_kind + ":" + schema_uuid + ":" + wanted.display_name);
      }
    }
  }
  return OkDiagnostic();
}

const EngineCatalogObjectRecord* ResolveNamedObjectInScope(
    const EngineCatalogObjectLifecycleState& state,
    const EngineRequestContext& context,
    const std::string& schema_uuid,
    const std::string& requested_object_kind,
    const EngineLocalizedName& requested_name) {
  const auto wanted = MakeNameRecord(context, {}, requested_object_kind, schema_uuid, requested_name, 0);
  for (const auto& entry : state.names) {
    if (!requested_object_kind.empty() && entry.object_kind != requested_object_kind &&
        entry.object_kind != "synonym") {
      continue;
    }
    if (!schema_uuid.empty() && entry.schema_uuid != schema_uuid) { continue; }
    if (entry.language_tag != wanted.language_tag) { continue; }
    if (ProfileName(entry.identifier_profile_uuid) != ProfileName(wanted.identifier_profile_uuid)) { continue; }
    const std::string left_key = wanted.requires_exact_match ? wanted.exact_lookup_key : wanted.normalized_lookup_key;
    const std::string right_key = wanted.requires_exact_match ? entry.exact_lookup_key : entry.normalized_lookup_key;
    if (left_key != right_key) { continue; }
    const auto* object = FindObject(state, entry.object_uuid);
    if (object != nullptr) { return object; }
  }
  return nullptr;
}

std::vector<EngineLocalizedName> PathSegmentNames(const EngineApiRequest& request) {
  std::vector<EngineLocalizedName> names;
  for (const auto& segment : request.sql_object_reference.path_components) {
    EngineLocalizedName name;
    name.language_tag = request.context.language_context.language_tag.empty()
                            ? "en"
                            : request.context.language_context.language_tag;
    name.name_class = "primary";
    name.name = segment.raw_text;
    name.raw_name_text = segment.raw_text;
    name.display_name = segment.raw_text;
    name.was_quoted = segment.was_quoted;
    name.quote_style = segment.quote_style;
    name.requires_exact_match = segment.requires_exact_match || segment.was_quoted;
    name.identifier_profile_uuid = segment.identifier_profile_uuid;
    name.default_name = true;
    names.push_back(std::move(name));
  }
  return names;
}

template <typename TResult>
void FillObjectResult(TResult* result,
                      const EngineRequestContext& context,
                      const EngineCatalogObjectRecord& object,
                      std::uint64_t metadata_epoch) {
  result->primary_object.uuid.canonical = object.object_uuid;
  result->primary_object.object_kind = object.object_kind;
  result->bound_object_identity.object_uuid.canonical = object.object_uuid;
  result->bound_object_identity.resolved_object_type = object.object_kind;
  result->bound_object_identity.resolved_schema_uuid.canonical = object.schema_uuid;
  result->bound_object_identity.parent_object_uuid.canonical = object.schema_uuid;
  result->bound_object_identity.catalog_generation_id = metadata_epoch;
  result->bound_object_identity.security_epoch = context.security_epoch;
  result->bound_object_identity.resource_epoch = context.resource_epoch;
  result->catalog_row_uuid.canonical = GenerateCrudEngineUuid("row");
  result->metadata_cache_epoch = metadata_epoch;
  AddEvidence(result, "catalog_metadata_epoch", std::to_string(metadata_epoch));
  AddEvidence(result, "metadata_cache_invalidation", object.object_uuid + ":" + std::to_string(metadata_epoch));
  AddRow(result, {{"object_uuid", object.object_uuid},
                  {"object_kind", object.object_kind},
                  {"schema_uuid", object.schema_uuid},
                  {"lifecycle_state", object.lifecycle_state},
                  {"definition_epoch", std::to_string(object.definition_epoch)},
                  {"metadata_epoch", std::to_string(metadata_epoch)}});
}

EngineApiDiagnostic PersistResolverNames(const EngineRequestContext& context,
                                         const std::string& operation_id,
                                         const std::string& object_uuid,
                                         const std::string& object_kind,
                                         const std::string& schema_uuid,
                                         const std::vector<EngineLocalizedName>& names,
                                         const std::string& fallback_name,
                                         bool retire_first) {
  if (retire_first) {
    const auto retired = RetireNameRegistryEntriesForObject(context, operation_id, object_uuid);
    if (retired.error) { return retired; }
  }
  return PersistNameRegistryEntriesForObject(context, operation_id, object_uuid, object_kind, schema_uuid, names, fallback_name);
}

}  // namespace

EngineLoadCatalogObjectLifecycleStateResult LoadCatalogObjectLifecycleState(
    const EngineRequestContext& context) {
  return LoadState(context, {.enforce_visibility = true});
}

bool EngineCatalogObjectCanOwnChildren(const std::string& object_kind) {
  return ParentCapableKind(object_kind);
}

EngineCatalogSynonymResolutionResult ResolveCatalogSynonymChain(
    const EngineCatalogObjectLifecycleState& state,
    const EngineCatalogObjectRecord& candidate,
    const EngineRequestContext&,
    const std::string& required_final_object_kind) {
  EngineCatalogSynonymResolutionResult result;
  const EngineCatalogObjectRecord* current = &candidate;
  std::set<std::string> seen_synonyms;
  while (current != nullptr && current->object_kind == "synonym") {
    if (seen_synonyms.count(current->object_uuid) != 0) {
      result.diagnostic = CatalogDiagnostic(kCatalogSynonymDiagnosticCycle, current->object_uuid);
      return result;
    }
    if (result.synonym_chain.size() == kCatalogSynonymMaxDereferenceDepth) {
      result.diagnostic = CatalogDiagnostic(kCatalogSynonymDiagnosticDepthExceeded, current->object_uuid);
      return result;
    }
    seen_synonyms.insert(current->object_uuid);
    result.synonym_chain.push_back(current->object_uuid);

    const std::string target_uuid = SynonymTargetUuid(*current);
    const std::string target_class = SynonymTargetClass(*current);
    if (target_uuid.empty() || target_class.empty()) {
      result.diagnostic = CatalogDiagnostic(kCatalogSynonymDiagnosticTargetMissing, current->object_uuid);
      return result;
    }
    bool dependency_ok = false;
    const std::string required_dependency_kind = "synonym_hard:" + target_class;
    for (const auto& dependency : state.dependencies) {
      if (dependency.source_uuid == current->object_uuid &&
          dependency.dependency_uuid == target_uuid &&
          dependency.dependency_kind == required_dependency_kind) {
        dependency_ok = true;
        break;
      }
    }
    if (!dependency_ok) {
      result.diagnostic = CatalogDiagnostic(kCatalogSynonymDiagnosticDependencyInvalid,
                                            current->object_uuid + "->" + target_uuid);
      return result;
    }
    current = FindObject(state, target_uuid);
    if (current == nullptr) {
      result.diagnostic = CatalogDiagnostic(kCatalogSynonymDiagnosticTargetMissing, target_uuid);
      return result;
    }
    if (current->object_kind != target_class) {
      result.diagnostic = CatalogDiagnostic(kCatalogSynonymDiagnosticTargetClassMismatch,
                                            target_uuid + ":" + current->object_kind + "!=" + target_class);
      return result;
    }
  }

  if (current == nullptr) {
    result.diagnostic = CatalogDiagnostic(kCatalogSynonymDiagnosticTargetMissing, candidate.object_uuid);
    return result;
  }
  if (!required_final_object_kind.empty() && current->object_kind != required_final_object_kind) {
    result.diagnostic = CatalogDiagnostic(kCatalogSynonymDiagnosticTargetClassMismatch,
                                          current->object_uuid + ":" + current->object_kind + "!=" +
                                              required_final_object_kind);
    return result;
  }
  result.ok = true;
  result.diagnostic = OkDiagnostic();
  result.final_object = *current;
  return result;
}

EngineApiDiagnostic PersistCatalogColumnAndConstraintMetadata(
    const EngineApiRequest& request,
    const EngineCatalogObjectLifecycleState& state,
    const std::string& owner_object_uuid,
    std::uint64_t metadata_epoch) {
  for (const auto& column : request.columns) {
    EngineCatalogColumnMetadataRecord record;
    record.creator_tx = request.context.local_transaction_id;
    record.column_uuid = RequestedOrGeneratedUuid(column.requested_column_uuid, "column");
    if (record.column_uuid.empty()) {
      return CatalogDiagnostic(kCatalogObjectDiagnosticUuidRequired, "column_uuid");
    }
    record.owner_object_uuid = owner_object_uuid;
    record.descriptor_kind = column.descriptor.descriptor_kind;
    record.canonical_type_name = column.descriptor.canonical_type_name;
    record.default_expression_envelope = column.default_expression_envelope;
    record.ordinal = column.ordinal;
    record.nullable = column.nullable;
    record.metadata_epoch = metadata_epoch;
    const auto appended = AppendEvent(request.context, ColumnMetadataEvent(record));
    if (appended.error) { return appended; }
  }

  std::set<std::string> pending_constraint_name_keys;
  for (const auto& definition : request.constraints) {
    const std::string constraint_uuid =
        RequestedOrGeneratedUuid(definition.requested_constraint_uuid, "constraint");
    if (constraint_uuid.empty()) {
      return CatalogDiagnostic(kCatalogConstraintDiagnosticUuidRequired, "constraint_uuid");
    }
    if (definition.constraint_kind.empty()) {
      return CatalogDiagnostic(kCatalogConstraintDiagnosticKindRequired, constraint_uuid);
    }
    if (FindObject(state, constraint_uuid) != nullptr) {
      return CatalogDiagnostic(kCatalogObjectDiagnosticDuplicateName,
                               "constraint_uuid:" + constraint_uuid);
    }

    const auto fields = PayloadMap(definition.canonical_constraint_envelope);
    const std::string support_uuid = PayloadField(fields, "support_uuid");
    const std::string support_family = PayloadField(fields, "support_family");
    if (ConstraintClassRequiresSupport(definition.constraint_kind) && support_uuid.empty()) {
      return CatalogDiagnostic(kCatalogConstraintDiagnosticSupportRequired,
                               definition.constraint_kind + ":" + constraint_uuid);
    }
    if (!SupportFamilyAllowedForConstraint(definition.constraint_kind, support_family)) {
      return CatalogDiagnostic(kCatalogConstraintDiagnosticSupportFamilyUnsupported,
                               definition.constraint_kind + ":" + support_family);
    }

    const auto conflict = CheckNameConflict(state,
                                           constraint_uuid,
                                           "constraint",
                                           owner_object_uuid,
                                           definition.names,
                                           request.context);
    if (conflict.error) {
      return CatalogDiagnostic(kCatalogConstraintDiagnosticDuplicateName, conflict.detail);
    }
    for (const auto& name : definition.names) {
      const auto pending = MakeNameRecord(request.context, constraint_uuid, "constraint", owner_object_uuid, name, metadata_epoch);
      const std::string lookup_key = owner_object_uuid + "\t" + pending.language_tag + "\t" +
                                     ProfileName(pending.identifier_profile_uuid) + "\t" +
                                     (pending.requires_exact_match ? pending.exact_lookup_key
                                                                   : pending.normalized_lookup_key);
      if (!pending_constraint_name_keys.insert(lookup_key).second) {
        return CatalogDiagnostic(kCatalogConstraintDiagnosticDuplicateName, pending.display_name);
      }
    }

    const std::string constraint_hash = PayloadField(fields, "constraint_hash");
    if (constraint_hash.empty()) {
      return CatalogDiagnostic(kCatalogConstraintDiagnosticDescriptorInvalid,
                               "constraint_hash:" + constraint_uuid);
    }

    EngineCatalogObjectRecord object;
    object.creator_tx = request.context.local_transaction_id;
    object.object_uuid = constraint_uuid;
    object.object_kind = "constraint";
    object.schema_uuid = owner_object_uuid;
    object.owner_principal_uuid = request.context.principal_uuid.canonical;
    object.lifecycle_state = "active";
    object.definition_epoch = 1;
    object.metadata_epoch = metadata_epoch;
    object.payload = "constraint_catalog_table=sys.constraint_descriptor;constraint_class=" +
                     definition.constraint_kind + ";owner_object_uuid=" + owner_object_uuid;
    auto appended = AppendEvent(request.context, ObjectEvent(object));
    if (appended.error) { return appended; }

    for (const auto& name : definition.names) {
      appended = AppendEvent(request.context,
                             NameEvent(MakeNameRecord(request.context,
                                                      constraint_uuid,
                                                      "constraint",
                                                      owner_object_uuid,
                                                      name,
                                                      metadata_epoch)));
      if (appended.error) { return appended; }
    }

    EngineCatalogConstraintDescriptorRecord descriptor;
    descriptor.creator_tx = request.context.local_transaction_id;
    descriptor.constraint_uuid = constraint_uuid;
    descriptor.constraint_class = definition.constraint_kind;
    descriptor.owner_object_uuid = owner_object_uuid;
    descriptor.name_ref_uuid = PayloadField(fields, "name_ref_uuid");
    descriptor.constraint_policy_version_uuid =
        PayloadField(fields, "constraint_policy_version_uuid", PayloadField(fields, "policy_uuid"));
    descriptor.enforcement_timing = PayloadField(fields, "enforcement_timing", "immediate");
    descriptor.validation_state = PayloadField(fields, "validation_state", "unvalidated");
    descriptor.trust_state = PayloadField(fields, "trust_state", "untrusted");
    descriptor.support_requirement = PayloadField(
        fields,
        "support_requirement",
        ConstraintClassRequiresSupport(definition.constraint_kind) ? "required" : "optional");
    descriptor.predicate_sblr_uuid = PayloadField(fields, "predicate_sblr_uuid");
    descriptor.diagnostic_profile_uuid = PayloadField(fields, "diagnostic_profile_uuid");
    descriptor.metrics_profile_uuid = PayloadField(fields, "metrics_profile_uuid");
    descriptor.conformance_profile_uuid = PayloadField(fields, "conformance_profile_uuid");
    descriptor.constraint_hash = constraint_hash;
    descriptor.canonical_constraint_envelope = definition.canonical_constraint_envelope;
    descriptor.metadata_epoch = metadata_epoch;
    appended = AppendEvent(request.context, ConstraintDescriptorEvent(descriptor));
    if (appended.error) { return appended; }

    const std::string key_descriptor_uuid = PayloadField(fields, "key_descriptor_uuid");
    if (!key_descriptor_uuid.empty() || ConstraintClassRequiresSupport(definition.constraint_kind)) {
      EngineCatalogKeyDescriptorRecord key;
      key.creator_tx = request.context.local_transaction_id;
      key.key_descriptor_uuid = key_descriptor_uuid.empty()
                                    ? GenerateCrudEngineUuid("key_descriptor")
                                    : key_descriptor_uuid;
      if (key.key_descriptor_uuid.empty()) {
        return CatalogDiagnostic(kCatalogConstraintDiagnosticDescriptorInvalid,
                                 "key_descriptor_uuid:" + constraint_uuid);
      }
      key.constraint_uuid = constraint_uuid;
      key.key_class = PayloadField(fields, "key_class", definition.constraint_kind);
      key.owner_object_uuid = owner_object_uuid;
      key.component_order_hash = PayloadField(fields, "component_order_hash");
      key.comparison_profile_hash = PayloadField(fields, "comparison_profile_hash");
      key.null_policy = PayloadField(fields, "null_policy", "not_applicable");
      key.canonical_encoding_uuid = PayloadField(fields, "canonical_encoding_uuid");
      key.candidate_reference_allowed = PayloadBoolField(fields, "candidate_reference_allowed", true);
      key.key_state = PayloadField(fields, "key_state", "active");
      key.key_hash = PayloadField(fields, "key_hash", constraint_hash);
      key.metadata_epoch = metadata_epoch;
      appended = AppendEvent(request.context, KeyDescriptorEvent(key));
      if (appended.error) { return appended; }
    }

    EngineCatalogConstraintSubjectRecord subject;
    subject.creator_tx = request.context.local_transaction_id;
    subject.subject_uuid = PayloadField(fields, "subject_uuid", GenerateCrudEngineUuid("constraint_subject"));
    subject.constraint_uuid = constraint_uuid;
    subject.subject_kind = PayloadField(fields, "subject_kind", "owner_object");
    subject.subject_object_uuid = PayloadField(fields, "subject_object_uuid", owner_object_uuid);
    subject.subject_descriptor = PayloadField(fields, "subject_descriptor");
    subject.expression_sblr_uuid = PayloadField(fields, "expression_sblr_uuid");
    subject.ordinal = static_cast<std::uint32_t>(ParseU64(PayloadField(fields, "subject_ordinal", "0")));
    subject.metadata_epoch = metadata_epoch;
    if (subject.subject_uuid.empty() || subject.subject_object_uuid.empty()) {
      return CatalogDiagnostic(kCatalogConstraintDiagnosticDescriptorInvalid,
                               "constraint_subject:" + constraint_uuid);
    }
    appended = AppendEvent(request.context, ConstraintSubjectEvent(subject));
    if (appended.error) { return appended; }

    const std::string dependency_object_uuid = PayloadField(fields, "dependency_object_uuid");
    if (!dependency_object_uuid.empty()) {
      EngineCatalogConstraintDependencyRecord dependency;
      dependency.creator_tx = request.context.local_transaction_id;
      dependency.dependency_uuid = PayloadField(fields, "dependency_uuid", GenerateCrudEngineUuid("constraint_dependency"));
      dependency.constraint_uuid = constraint_uuid;
      dependency.dependency_kind = PayloadField(fields, "dependency_kind", "object");
      dependency.dependency_object_uuid = dependency_object_uuid;
      dependency.dependency_version_uuid = PayloadField(fields, "dependency_version_uuid");
      dependency.invalidation_action = PayloadField(fields, "invalidation_action", "revalidate_required");
      dependency.dependency_hash = PayloadField(fields, "dependency_hash", constraint_hash);
      dependency.metadata_epoch = metadata_epoch;
      if (dependency.dependency_uuid.empty() || dependency.dependency_kind.empty()) {
        return CatalogDiagnostic(kCatalogConstraintDiagnosticDependencyInvalid,
                                 "dependency:" + constraint_uuid);
      }
      appended = AppendEvent(request.context, ConstraintDependencyEvent(dependency));
      if (appended.error) { return appended; }

      EngineCatalogDependencyRecord object_dependency;
      object_dependency.creator_tx = request.context.local_transaction_id;
      object_dependency.source_uuid = constraint_uuid;
      object_dependency.source_kind = "constraint";
      object_dependency.dependency_uuid = dependency_object_uuid;
      object_dependency.dependency_kind = "constraint:" + dependency.dependency_kind;
      object_dependency.metadata_epoch = metadata_epoch;
      appended = AppendEvent(request.context, DependencyEvent(object_dependency));
      if (appended.error) { return appended; }
    }

    if (!support_uuid.empty()) {
      EngineCatalogConstraintSupportStructureRecord support;
      support.creator_tx = request.context.local_transaction_id;
      support.support_binding_uuid = PayloadField(fields, "support_binding_uuid", GenerateCrudEngineUuid("constraint_support"));
      support.constraint_uuid = constraint_uuid;
      support.support_uuid = support_uuid;
      support.support_class = PayloadField(fields, "support_class", "index");
      support.support_family = support_family;
      support.coverage_scope_hash = PayloadField(fields, "coverage_scope_hash", constraint_hash);
      support.durability_class = PayloadField(fields, "durability_class", "durable");
      support.residency_class = PayloadField(fields, "residency_class", "warmable");
      support.validity_state = PayloadField(fields, "validity_state", "valid");
      support.enforcement_role = PayloadField(fields, "enforcement_role", "primary_enforcement");
      support.binding_hash = PayloadField(fields, "binding_hash", constraint_hash);
      support.metadata_epoch = metadata_epoch;
      if (support.support_binding_uuid.empty() || support.support_family.empty()) {
        return CatalogDiagnostic(kCatalogConstraintDiagnosticSupportRequired,
                                 "support:" + constraint_uuid);
      }
      appended = AppendEvent(request.context, ConstraintSupportStructureEvent(support));
      if (appended.error) { return appended; }

      EngineCatalogDependencyRecord support_dependency;
      support_dependency.creator_tx = request.context.local_transaction_id;
      support_dependency.source_uuid = constraint_uuid;
      support_dependency.source_kind = "constraint";
      support_dependency.dependency_uuid = support_uuid;
      support_dependency.dependency_kind = "constraint:support_structure";
      support_dependency.metadata_epoch = metadata_epoch;
      appended = AppendEvent(request.context, DependencyEvent(support_dependency));
      if (appended.error) { return appended; }
    }
  }
  return OkDiagnostic();
}

EngineCatalogCreateObjectResult EngineCatalogCreateObject(const EngineCatalogCreateObjectRequest& request) {
  constexpr const char* kOperation = "catalog.object.create";
  const auto valid = ValidateMutatingContext(request.context);
  if (valid.error) { return DiagnosticResult<EngineCatalogCreateObjectResult>(request.context, kOperation, valid); }
  const std::string object_uuid = ObjectUuid(request);
  if (object_uuid.empty()) {
    return DiagnosticResult<EngineCatalogCreateObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  const std::string object_kind = ObjectKind(request);
  if (object_kind.empty()) {
    return DiagnosticResult<EngineCatalogCreateObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticKindRequired, "target_object.object_kind"));
  }
  const auto names = EffectiveNames(request);
  if (names.empty()) {
    return DiagnosticResult<EngineCatalogCreateObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticNameRequired, "localized_names"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineCatalogCreateObjectResult>(request.context, kOperation, loaded.diagnostic);
  }
  if (FindObject(loaded.state, object_uuid) != nullptr) {
    return DiagnosticResult<EngineCatalogCreateObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticDuplicateName, "object_uuid:" + object_uuid));
  }
  std::string schema_uuid = request.target_schema.uuid.canonical;
  for (const auto& segment_name : PathSegmentNames(request)) {
    const auto* parent = ResolveNamedObjectInScope(loaded.state, request.context, schema_uuid, {}, segment_name);
    if (parent == nullptr) {
      return DiagnosticResult<EngineCatalogCreateObjectResult>(
          request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticNameNotFound, segment_name.display_name));
    }
    const auto remapped = ResolveCatalogSynonymChain(loaded.state, *parent, request.context, {});
    if (!remapped.ok) {
      return DiagnosticResult<EngineCatalogCreateObjectResult>(request.context, kOperation, remapped.diagnostic);
    }
    if (!EngineCatalogObjectCanOwnChildren(remapped.final_object.object_kind)) {
      return DiagnosticResult<EngineCatalogCreateObjectResult>(
          request.context,
          kOperation,
          CatalogDiagnostic(kCatalogSynonymDiagnosticParentNotAllowed,
                            remapped.final_object.object_uuid + ":" + remapped.final_object.object_kind));
    }
    schema_uuid = remapped.final_object.object_uuid;
  }
  if (!schema_uuid.empty()) {
    if (const auto* parent_candidate = FindObject(loaded.state, schema_uuid)) {
      const auto remapped = ResolveCatalogSynonymChain(loaded.state, *parent_candidate, request.context, {});
      if (!remapped.ok) {
        return DiagnosticResult<EngineCatalogCreateObjectResult>(request.context, kOperation, remapped.diagnostic);
      }
      if (!EngineCatalogObjectCanOwnChildren(remapped.final_object.object_kind)) {
        return DiagnosticResult<EngineCatalogCreateObjectResult>(
            request.context,
            kOperation,
            CatalogDiagnostic(kCatalogSynonymDiagnosticParentNotAllowed,
                              remapped.final_object.object_uuid + ":" + remapped.final_object.object_kind));
      }
      schema_uuid = remapped.final_object.object_uuid;
    }
  }
  if (object_kind != "schema" && schema_uuid.empty()) {
    return DiagnosticResult<EngineCatalogCreateObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticSchemaUuidRequired, "target_schema.uuid"));
  }
  if (!PrincipalOwnsSchema(loaded.state, request.context, schema_uuid)) {
    return DiagnosticResult<EngineCatalogCreateObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticSchemaOwnerDenied, schema_uuid));
  }
  const auto conflict = CheckNameConflict(loaded.state, object_uuid, object_kind, schema_uuid, names, request.context);
  if (conflict.error) { return DiagnosticResult<EngineCatalogCreateObjectResult>(request.context, kOperation, conflict); }
  std::string synonym_target_uuid;
  std::string synonym_target_class;
  if (object_kind == "synonym") {
    if (request.related_objects.empty() || request.related_objects.front().uuid.canonical.empty() ||
        request.related_objects.front().object_kind.empty()) {
      return DiagnosticResult<EngineCatalogCreateObjectResult>(
          request.context,
          kOperation,
          CatalogDiagnostic(kCatalogSynonymDiagnosticTargetMissing, "related_objects[0]"));
    }
    synonym_target_uuid = request.related_objects.front().uuid.canonical;
    synonym_target_class = request.related_objects.front().object_kind;
  }
  for (const auto& related : request.related_objects) {
    if (related.uuid.canonical.empty()) { continue; }
    const auto* target = FindObject(loaded.state, related.uuid.canonical);
    if (target == nullptr) {
      return DiagnosticResult<EngineCatalogCreateObjectResult>(
          request.context,
          kOperation,
          CatalogDiagnostic(object_kind == "synonym" ? kCatalogSynonymDiagnosticTargetMissing
                                                     : kCatalogObjectDiagnosticDependencyTargetNotVisible,
                            related.uuid.canonical));
    }
    if (object_kind == "synonym" && target->object_kind != related.object_kind) {
      return DiagnosticResult<EngineCatalogCreateObjectResult>(
          request.context,
          kOperation,
          CatalogDiagnostic(kCatalogSynonymDiagnosticTargetClassMismatch,
                            target->object_uuid + ":" + target->object_kind + "!=" + related.object_kind));
    }
  }

  const std::uint64_t epoch = loaded.state.metadata_epoch + 1;
  EngineCatalogObjectRecord record;
  record.creator_tx = request.context.local_transaction_id;
  record.object_uuid = object_uuid;
  record.object_kind = object_kind;
  record.schema_uuid = schema_uuid;
  record.owner_principal_uuid = request.context.principal_uuid.canonical;
  record.lifecycle_state = "active";
  record.definition_epoch = 1;
  record.metadata_epoch = epoch;
  record.payload = object_kind == "synonym"
                       ? SynonymPayload(request, synonym_target_uuid, synonym_target_class)
                       : PayloadFromRequest(request);
  auto appended = AppendEvent(request.context, ObjectEvent(record));
  if (appended.error) {
    return DiagnosticResult<EngineCatalogCreateObjectResult>(request.context, kOperation, appended);
  }
  for (const auto& name : names) {
    appended = AppendEvent(request.context, NameEvent(MakeNameRecord(request.context, object_uuid, object_kind, schema_uuid, name, epoch)));
    if (appended.error) {
      return DiagnosticResult<EngineCatalogCreateObjectResult>(request.context, kOperation, appended);
    }
  }
  for (const auto& related : request.related_objects) {
    if (related.uuid.canonical.empty()) { continue; }
    EngineCatalogDependencyRecord dependency;
    dependency.creator_tx = request.context.local_transaction_id;
    dependency.source_uuid = object_uuid;
    dependency.source_kind = object_kind;
    dependency.dependency_uuid = related.uuid.canonical;
    dependency.dependency_kind = DependencyKindForRelatedObject(object_kind, related);
    dependency.metadata_epoch = epoch;
    appended = AppendEvent(request.context, DependencyEvent(dependency));
    if (appended.error) {
      return DiagnosticResult<EngineCatalogCreateObjectResult>(request.context, kOperation, appended);
    }
  }
  appended = PersistCatalogColumnAndConstraintMetadata(request, loaded.state, object_uuid, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineCatalogCreateObjectResult>(request.context, kOperation, appended);
  }
  appended = AppendCacheInvalidation(request.context, kOperation, object_uuid, epoch);
  if (appended.error) { return DiagnosticResult<EngineCatalogCreateObjectResult>(request.context, kOperation, appended); }
  const auto resolver = PersistResolverNames(request.context, kOperation, object_uuid, object_kind, schema_uuid, names, PrimaryName(request), false);
  if (resolver.error) { return DiagnosticResult<EngineCatalogCreateObjectResult>(request.context, kOperation, resolver); }

  auto result = SuccessResult<EngineCatalogCreateObjectResult>(request.context, kOperation);
  FillObjectResult(&result, request.context, record, epoch);
  AddEvidence(&result, "resolver_boundary", "SBNAME1:" + object_uuid);
  return result;
}

EngineCatalogApplyConstraintsResult EngineCatalogApplyConstraintsToObject(
    const EngineCatalogApplyConstraintsRequest& request) {
  const std::string operation_id =
      request.operation_id.empty() ? "catalog.constraint.apply" : request.operation_id;
  const auto valid = ValidateMutatingContext(request.context);
  if (valid.error) {
    return DiagnosticResult<EngineCatalogApplyConstraintsResult>(request.context, operation_id, valid);
  }
  if (request.constraints.empty()) {
    return DiagnosticResult<EngineCatalogApplyConstraintsResult>(
        request.context,
        operation_id,
        CatalogDiagnostic(kCatalogConstraintDiagnosticKindRequired, "constraints"));
  }
  const std::string owner_object_uuid = ObjectUuid(request);
  if (owner_object_uuid.empty()) {
    return DiagnosticResult<EngineCatalogApplyConstraintsResult>(
        request.context,
        operation_id,
        CatalogDiagnostic(kCatalogObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineCatalogApplyConstraintsResult>(request.context, operation_id, loaded.diagnostic);
  }
  const auto* existing = FindObject(loaded.state, owner_object_uuid);
  if (existing == nullptr) {
    return DiagnosticResult<EngineCatalogApplyConstraintsResult>(
        request.context,
        operation_id,
        CatalogDiagnostic(kCatalogObjectDiagnosticNotFound, owner_object_uuid));
  }
  if (!PrincipalOwnsSchema(loaded.state, request.context, existing->schema_uuid)) {
    return DiagnosticResult<EngineCatalogApplyConstraintsResult>(
        request.context,
        operation_id,
        CatalogDiagnostic(kCatalogObjectDiagnosticSchemaOwnerDenied, existing->schema_uuid));
  }

  const std::uint64_t epoch = loaded.state.metadata_epoch + 1;
  const auto appended = PersistCatalogColumnAndConstraintMetadata(
      request, loaded.state, owner_object_uuid, epoch);
  if (appended.error) {
    return DiagnosticResult<EngineCatalogApplyConstraintsResult>(request.context, operation_id, appended);
  }
  const auto invalidated = AppendCacheInvalidation(request.context, operation_id, owner_object_uuid, epoch);
  if (invalidated.error) {
    return DiagnosticResult<EngineCatalogApplyConstraintsResult>(request.context, operation_id, invalidated);
  }

  auto result = SuccessResult<EngineCatalogApplyConstraintsResult>(request.context, operation_id);
  FillObjectResult(&result, request.context, *existing, epoch);
  AddEvidence(&result, "constraint_catalog_route", "sys.constraint_descriptor");
  AddEvidence(&result, "constraint_apply_count", std::to_string(request.constraints.size()));
  return result;
}

EngineCatalogAlterObjectResult EngineCatalogAlterObject(const EngineCatalogAlterObjectRequest& request) {
  constexpr const char* kOperation = "catalog.object.alter";
  const auto valid = ValidateMutatingContext(request.context);
  if (valid.error) { return DiagnosticResult<EngineCatalogAlterObjectResult>(request.context, kOperation, valid); }
  const std::string object_uuid = ObjectUuid(request);
  if (object_uuid.empty()) {
    return DiagnosticResult<EngineCatalogAlterObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineCatalogAlterObjectResult>(request.context, kOperation, loaded.diagnostic);
  }
  const auto* existing = FindObject(loaded.state, object_uuid);
  if (existing == nullptr) {
    return DiagnosticResult<EngineCatalogAlterObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticNotFound, object_uuid));
  }
  if (!PrincipalOwnsSchema(loaded.state, request.context, existing->schema_uuid)) {
    return DiagnosticResult<EngineCatalogAlterObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticSchemaOwnerDenied, existing->schema_uuid));
  }
  std::string synonym_target_uuid;
  std::string synonym_target_class;
  if (existing->object_kind == "synonym") {
    if (request.related_objects.empty() || request.related_objects.front().uuid.canonical.empty() ||
        request.related_objects.front().object_kind.empty()) {
      return DiagnosticResult<EngineCatalogAlterObjectResult>(
          request.context,
          kOperation,
          CatalogDiagnostic(kCatalogSynonymDiagnosticTargetMissing, "related_objects[0]"));
    }
    const auto* target = FindObject(loaded.state, request.related_objects.front().uuid.canonical);
    if (target == nullptr) {
      return DiagnosticResult<EngineCatalogAlterObjectResult>(
          request.context,
          kOperation,
          CatalogDiagnostic(kCatalogSynonymDiagnosticTargetMissing,
                            request.related_objects.front().uuid.canonical));
    }
    if (target->object_kind != request.related_objects.front().object_kind) {
      return DiagnosticResult<EngineCatalogAlterObjectResult>(
          request.context,
          kOperation,
          CatalogDiagnostic(kCatalogSynonymDiagnosticTargetClassMismatch,
                            target->object_uuid + ":" + target->object_kind + "!=" +
                                request.related_objects.front().object_kind));
    }
    synonym_target_uuid = target->object_uuid;
    synonym_target_class = target->object_kind;
  }
  const std::uint64_t epoch = loaded.state.metadata_epoch + 1;
  EngineCatalogObjectRecord replacement = *existing;
  replacement.creator_tx = request.context.local_transaction_id;
  replacement.definition_epoch = existing->definition_epoch + 1;
  replacement.metadata_epoch = epoch;
  replacement.payload = existing->object_kind == "synonym"
                            ? SynonymPayload(request, synonym_target_uuid, synonym_target_class)
                            : PayloadFromRequest(request);
  auto appended = AppendEvent(request.context, ObjectEvent(replacement));
  if (appended.error) { return DiagnosticResult<EngineCatalogAlterObjectResult>(request.context, kOperation, appended); }
  if (existing->object_kind == "synonym") {
    const std::string old_target_uuid = SynonymTargetUuid(*existing);
    const std::string old_target_class = SynonymTargetClass(*existing);
    if (!old_target_uuid.empty() && !old_target_class.empty() &&
        (old_target_uuid != synonym_target_uuid || old_target_class != synonym_target_class)) {
      EngineCatalogDependencyRecord retired_dependency;
      retired_dependency.creator_tx = request.context.local_transaction_id;
      retired_dependency.source_uuid = object_uuid;
      retired_dependency.source_kind = "synonym";
      retired_dependency.dependency_uuid = old_target_uuid;
      retired_dependency.dependency_kind = "synonym_hard:" + old_target_class;
      retired_dependency.metadata_epoch = epoch;
      retired_dependency.deleted = true;
      appended = AppendEvent(request.context, DependencyEvent(retired_dependency));
      if (appended.error) { return DiagnosticResult<EngineCatalogAlterObjectResult>(request.context, kOperation, appended); }
    }
    EngineCatalogDependencyRecord dependency;
    dependency.creator_tx = request.context.local_transaction_id;
    dependency.source_uuid = object_uuid;
    dependency.source_kind = "synonym";
    dependency.dependency_uuid = synonym_target_uuid;
    dependency.dependency_kind = "synonym_hard:" + synonym_target_class;
    dependency.metadata_epoch = epoch;
    appended = AppendEvent(request.context, DependencyEvent(dependency));
    if (appended.error) { return DiagnosticResult<EngineCatalogAlterObjectResult>(request.context, kOperation, appended); }
  }
  appended = AppendCacheInvalidation(request.context, kOperation, object_uuid, epoch);
  if (appended.error) { return DiagnosticResult<EngineCatalogAlterObjectResult>(request.context, kOperation, appended); }

  auto result = SuccessResult<EngineCatalogAlterObjectResult>(request.context, kOperation);
  FillObjectResult(&result, request.context, replacement, epoch);
  return result;
}

EngineCatalogRenameObjectResult EngineCatalogRenameObject(const EngineCatalogRenameObjectRequest& request) {
  constexpr const char* kOperation = "catalog.object.rename";
  const auto valid = ValidateMutatingContext(request.context);
  if (valid.error) { return DiagnosticResult<EngineCatalogRenameObjectResult>(request.context, kOperation, valid); }
  const std::string object_uuid = ObjectUuid(request);
  if (object_uuid.empty()) {
    return DiagnosticResult<EngineCatalogRenameObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  const auto names = EffectiveNames(request);
  if (names.empty()) {
    return DiagnosticResult<EngineCatalogRenameObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticNameRequired, "localized_names"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineCatalogRenameObjectResult>(request.context, kOperation, loaded.diagnostic);
  }
  const auto* existing = FindObject(loaded.state, object_uuid);
  if (existing == nullptr) {
    return DiagnosticResult<EngineCatalogRenameObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticNotFound, object_uuid));
  }
  if (!PrincipalOwnsSchema(loaded.state, request.context, existing->schema_uuid)) {
    return DiagnosticResult<EngineCatalogRenameObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticSchemaOwnerDenied, existing->schema_uuid));
  }
  const auto conflict = CheckNameConflict(loaded.state, object_uuid, existing->object_kind, existing->schema_uuid, names, request.context);
  if (conflict.error) { return DiagnosticResult<EngineCatalogRenameObjectResult>(request.context, kOperation, conflict); }

  const std::uint64_t epoch = loaded.state.metadata_epoch + 1;
  auto appended = AppendEvent(request.context, RetireNamesEvent(request.context.local_transaction_id, object_uuid, epoch));
  if (appended.error) { return DiagnosticResult<EngineCatalogRenameObjectResult>(request.context, kOperation, appended); }
  for (const auto& name : names) {
    appended = AppendEvent(request.context, NameEvent(MakeNameRecord(request.context, object_uuid, existing->object_kind, existing->schema_uuid, name, epoch)));
    if (appended.error) {
      return DiagnosticResult<EngineCatalogRenameObjectResult>(request.context, kOperation, appended);
    }
  }
  EngineCatalogObjectRecord replacement = *existing;
  replacement.creator_tx = request.context.local_transaction_id;
  replacement.definition_epoch = existing->definition_epoch + 1;
  replacement.metadata_epoch = epoch;
  appended = AppendEvent(request.context, ObjectEvent(replacement));
  if (appended.error) { return DiagnosticResult<EngineCatalogRenameObjectResult>(request.context, kOperation, appended); }
  appended = AppendCacheInvalidation(request.context, kOperation, object_uuid, epoch);
  if (appended.error) { return DiagnosticResult<EngineCatalogRenameObjectResult>(request.context, kOperation, appended); }
  const auto resolver = PersistResolverNames(request.context, kOperation, object_uuid, existing->object_kind, existing->schema_uuid, names, PrimaryName(request), true);
  if (resolver.error) { return DiagnosticResult<EngineCatalogRenameObjectResult>(request.context, kOperation, resolver); }

  auto result = SuccessResult<EngineCatalogRenameObjectResult>(request.context, kOperation);
  FillObjectResult(&result, request.context, replacement, epoch);
  AddEvidence(&result, "resolver_boundary", "SBNAME1:" + object_uuid);
  return result;
}

EngineCatalogDropObjectResult EngineCatalogDropObject(const EngineCatalogDropObjectRequest& request) {
  constexpr const char* kOperation = "catalog.object.drop";
  const auto valid = ValidateMutatingContext(request.context);
  if (valid.error) { return DiagnosticResult<EngineCatalogDropObjectResult>(request.context, kOperation, valid); }
  const std::string object_uuid = ObjectUuid(request);
  if (object_uuid.empty()) {
    return DiagnosticResult<EngineCatalogDropObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineCatalogDropObjectResult>(request.context, kOperation, loaded.diagnostic);
  }
  const auto* existing = FindObject(loaded.state, object_uuid);
  if (existing == nullptr) {
    return DiagnosticResult<EngineCatalogDropObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticNotFound, object_uuid));
  }
  if (!PrincipalOwnsSchema(loaded.state, request.context, existing->schema_uuid)) {
    return DiagnosticResult<EngineCatalogDropObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticSchemaOwnerDenied, existing->schema_uuid));
  }
  for (const auto& dependency : loaded.state.dependencies) {
    if (dependency.dependency_uuid == object_uuid) {
      return DiagnosticResult<EngineCatalogDropObjectResult>(
          request.context,
          kOperation,
          CatalogDiagnostic(kCatalogObjectDiagnosticDependencyBlockedDrop, dependency.source_uuid + "->" + object_uuid));
    }
  }
  const std::uint64_t epoch = loaded.state.metadata_epoch + 1;
  EngineCatalogObjectRecord dropped = *existing;
  dropped.creator_tx = request.context.local_transaction_id;
  dropped.lifecycle_state = "dropped";
  dropped.deleted = true;
  dropped.metadata_epoch = epoch;
  auto appended = AppendEvent(request.context, ObjectEvent(dropped));
  if (appended.error) { return DiagnosticResult<EngineCatalogDropObjectResult>(request.context, kOperation, appended); }
  appended = AppendEvent(request.context, RetireNamesEvent(request.context.local_transaction_id, object_uuid, epoch));
  if (appended.error) { return DiagnosticResult<EngineCatalogDropObjectResult>(request.context, kOperation, appended); }
  appended = AppendCacheInvalidation(request.context, kOperation, object_uuid, epoch);
  if (appended.error) { return DiagnosticResult<EngineCatalogDropObjectResult>(request.context, kOperation, appended); }
  const auto resolver = RetireNameRegistryEntriesForObject(request.context, kOperation, object_uuid);
  if (resolver.error) { return DiagnosticResult<EngineCatalogDropObjectResult>(request.context, kOperation, resolver); }

  auto result = SuccessResult<EngineCatalogDropObjectResult>(request.context, kOperation);
  result.primary_object.uuid.canonical = object_uuid;
  result.primary_object.object_kind = existing->object_kind;
  result.metadata_cache_epoch = epoch;
  AddEvidence(&result, "catalog_metadata_epoch", std::to_string(epoch));
  AddEvidence(&result, "metadata_cache_invalidation", object_uuid + ":" + std::to_string(epoch));
  AddRow(&result, {{"object_uuid", object_uuid}, {"object_kind", existing->object_kind}, {"lifecycle_state", "dropped"}});
  return result;
}

EngineCatalogLookupObjectResult EngineCatalogLookupObjectByUuid(const EngineCatalogLookupObjectRequest& request) {
  constexpr const char* kOperation = "catalog.object.lookup_uuid";
  const std::string object_uuid = ObjectUuid(request);
  if (object_uuid.empty()) {
    return DiagnosticResult<EngineCatalogLookupObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticUuidRequired, "target_object.uuid"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineCatalogLookupObjectResult>(request.context, kOperation, loaded.diagnostic);
  }
  if (const auto* object = FindObject(loaded.state, object_uuid)) {
    auto result = SuccessResult<EngineCatalogLookupObjectResult>(request.context, kOperation);
    FillObjectResult(&result, request.context, *object, loaded.state.metadata_epoch);
    return result;
  }
  const auto all = LoadState(request.context, {.enforce_visibility = false});
  if (all.ok && FindObject(all.state, object_uuid) != nullptr) {
    return DiagnosticResult<EngineCatalogLookupObjectResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticMgaVisibilityRefused, object_uuid));
  }
  return DiagnosticResult<EngineCatalogLookupObjectResult>(
      request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticNotFound, object_uuid));
}

EngineCatalogResolveObjectNameResult EngineCatalogResolveObjectName(const EngineCatalogResolveObjectNameRequest& request) {
  constexpr const char* kOperation = "catalog.object.resolve_name";
  const std::string object_kind = ObjectKind(request);
  const auto names = EffectiveNames(request);
  if (names.empty()) {
    return DiagnosticResult<EngineCatalogResolveObjectNameResult>(
        request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticNameRequired, "localized_names"));
  }
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineCatalogResolveObjectNameResult>(request.context, kOperation, loaded.diagnostic);
  }
  std::string schema_uuid = request.target_schema.uuid.canonical;
  if (!schema_uuid.empty()) {
    if (const auto* parent_candidate = FindObject(loaded.state, schema_uuid)) {
      const auto remapped = ResolveCatalogSynonymChain(loaded.state, *parent_candidate, request.context, {});
      if (!remapped.ok) {
        return DiagnosticResult<EngineCatalogResolveObjectNameResult>(request.context, kOperation, remapped.diagnostic);
      }
      if (!EngineCatalogObjectCanOwnChildren(remapped.final_object.object_kind)) {
        return DiagnosticResult<EngineCatalogResolveObjectNameResult>(
            request.context,
            kOperation,
            CatalogDiagnostic(kCatalogSynonymDiagnosticParentNotAllowed,
                              remapped.final_object.object_uuid + ":" + remapped.final_object.object_kind));
      }
      schema_uuid = remapped.final_object.object_uuid;
    }
  }

  for (const auto& segment_name : PathSegmentNames(request)) {
    const auto* parent = ResolveNamedObjectInScope(loaded.state, request.context, schema_uuid, {}, segment_name);
    if (parent == nullptr) {
      return DiagnosticResult<EngineCatalogResolveObjectNameResult>(
          request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticNameNotFound, segment_name.display_name));
    }
    const auto remapped = ResolveCatalogSynonymChain(loaded.state, *parent, request.context, {});
    if (!remapped.ok) {
      return DiagnosticResult<EngineCatalogResolveObjectNameResult>(request.context, kOperation, remapped.diagnostic);
    }
    if (!EngineCatalogObjectCanOwnChildren(remapped.final_object.object_kind)) {
      return DiagnosticResult<EngineCatalogResolveObjectNameResult>(
          request.context,
          kOperation,
          CatalogDiagnostic(kCatalogSynonymDiagnosticParentNotAllowed,
                            remapped.final_object.object_uuid + ":" + remapped.final_object.object_kind));
    }
    schema_uuid = remapped.final_object.object_uuid;
  }

  for (const auto& requested : names) {
    const auto* object = ResolveNamedObjectInScope(loaded.state, request.context, schema_uuid, object_kind, requested);
    if (object == nullptr) { continue; }
    const auto resolved = ResolveCatalogSynonymChain(loaded.state, *object, request.context, object_kind);
    if (!resolved.ok) {
      return DiagnosticResult<EngineCatalogResolveObjectNameResult>(request.context, kOperation, resolved.diagnostic);
    }
    auto result = SuccessResult<EngineCatalogResolveObjectNameResult>(request.context, kOperation);
    FillObjectResult(&result, request.context, resolved.final_object, loaded.state.metadata_epoch);
    AddEvidence(&result, "resolver_boundary", "SBCATOBJ1");
    for (const auto& synonym_uuid : resolved.synonym_chain) {
      AddEvidence(&result, "synonym_chain", synonym_uuid);
    }
    if (!schema_uuid.empty()) {
      result.bound_object_identity.parent_object_uuid.canonical = schema_uuid;
    }
    return result;
  }
  const auto all = LoadState(request.context, {.enforce_visibility = false});
  if (all.ok) {
    for (const auto& requested : names) {
      const auto wanted = MakeNameRecord(request.context, {}, object_kind, schema_uuid, requested, 0);
      for (const auto& entry : all.state.names) {
        if (!object_kind.empty() && entry.object_kind != object_kind) { continue; }
        if (!schema_uuid.empty() && entry.schema_uuid != schema_uuid) { continue; }
        const std::string left_key = wanted.requires_exact_match ? wanted.exact_lookup_key : wanted.normalized_lookup_key;
        const std::string right_key = wanted.requires_exact_match ? entry.exact_lookup_key : entry.normalized_lookup_key;
        if (left_key == right_key) {
          return DiagnosticResult<EngineCatalogResolveObjectNameResult>(
              request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticMgaVisibilityRefused, entry.object_uuid));
        }
      }
    }
  }
  return DiagnosticResult<EngineCatalogResolveObjectNameResult>(
      request.context, kOperation, CatalogDiagnostic(kCatalogObjectDiagnosticNameNotFound, PrimaryName(request)));
}

EngineCatalogValidateMetadataCacheResult EngineCatalogValidateMetadataCache(
    const EngineCatalogValidateMetadataCacheRequest& request) {
  constexpr const char* kOperation = "catalog.object.validate_metadata_cache";
  const auto loaded = LoadState(request.context, {.enforce_visibility = true});
  if (!loaded.ok) {
    return DiagnosticResult<EngineCatalogValidateMetadataCacheResult>(request.context, kOperation, loaded.diagnostic);
  }
  const std::uint64_t observed_epoch = request.bound_object_identity.catalog_generation_id != 0
                                           ? request.bound_object_identity.catalog_generation_id
                                           : request.context.catalog_generation_id;
  if (observed_epoch != 0 && observed_epoch < loaded.state.metadata_epoch) {
    return DiagnosticResult<EngineCatalogValidateMetadataCacheResult>(
        request.context,
        kOperation,
        CatalogDiagnostic(kCatalogObjectDiagnosticCacheEpochStale,
                          std::to_string(observed_epoch) + "<" + std::to_string(loaded.state.metadata_epoch)));
  }
  auto result = SuccessResult<EngineCatalogValidateMetadataCacheResult>(request.context, kOperation);
  result.metadata_cache_epoch = loaded.state.metadata_epoch;
  AddEvidence(&result, "catalog_metadata_epoch", std::to_string(loaded.state.metadata_epoch));
  AddEvidence(&result, "metadata_cache_current", std::to_string(observed_epoch));
  return result;
}

}  // namespace scratchbird::engine::internal_api
