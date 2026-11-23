// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "api_diagnostics.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CATALOG_NAME_REGISTRY
// Durable SQL-object name registry. This is the canonical name-to-UUID lookup
// surface for the current event-store implementation.

inline constexpr const char* kNameRegistryEventMagic = "SBNAME02";

struct NameRegistryEntry {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  EngineUuid name_entry_uuid;
  EngineUuid object_uuid;
  std::string object_class;
  EngineUuid scope_uuid;
  EngineUuid parent_object_uuid;
  EngineUuid parent_schema_uuid;
  std::string language_tag = "en";
  std::string name_class = "primary";
  std::string reference_id;
  std::string dialect_profile_uuid;
  std::string identifier_profile_uuid = "sbsql_v3";
  std::string case_fold_profile_uuid;
  std::string quoted_identifier_profile_uuid;
  std::string raw_name_text;
  std::string display_name;
  bool was_quoted = false;
  std::string quote_style = "none";
  bool requires_exact_match = false;
  std::string normalized_lookup_key;
  std::string exact_lookup_key;
  std::string full_path_lookup_key;
  std::uint64_t catalog_generation_id = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  std::string lifecycle_state = "active";
  bool deleted = false;
  bool derived_from_legacy_name = false;
  // SEARCH_KEY: SB_NATIVE_EXACT_NAME (Part B S2, 2026-11-29). Set ONLY on an entry the NATIVE exact matcher produced
  // (ResolveNativeExactName in name_registry.cpp): the row's own values, as stored, never derived. language_uuid is the
  // entry's language; object_kind the u16 CatalogRecordKind; name_class_code CatalogNameClass; quote_style_code
  // CatalogNameQuoteStyle. was_quoted / quote_style_code / requires_exact_match are the parser's OPAQUE datum: returned,
  // never interpreted. On such an entry the three lookup keys above and the profile labels are EMPTY (nothing folded).
  bool native_exact = false;
  EngineUuid language_uuid;
  std::uint16_t object_kind = 0;
  std::uint8_t name_class_code = 0;
  std::uint8_t quote_style_code = 0;
  bool default_for_language = false;
  bool default_for_object = false;
};

struct NameRegistryState {
  std::vector<NameRegistryEntry> entries;
};

struct NameRegistryLoadResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  NameRegistryState state;
};

struct NameRegistryResolveResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<NameRegistryEntry> matches;
};

struct NameRegistryNameResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  NameRegistryEntry entry;
};

NameRegistryLoadResult LoadNameRegistryState(const EngineRequestContext& context,
                                             std::uint64_t observer_tx);

// SB_NATIVE_EXACT_NAME: the language tag text of a language UUID: a known language constant's tag, else the UUID's own 32
// lower-case hex digits (the token the native projection has always given an unknown language). The inverse mapping the
// native exact matcher applies to a request's language tag.
std::string NameRegistryLanguageTagOfUuid(const EngineUuid& language);
// Part B S3: the language UUID a tag names, by the SAME exact rule the native matcher applies (a known tag spelled exactly, or
// 32 hex digits); false for anything else (the caller refuses, no default, no fallback).
bool NameRegistryLanguageUuidOfTag(const std::string& tag, EngineUuid* language);

std::string NameRegistryDefaultLanguage(const EngineRequestContext& context);
std::string NameRegistrySessionLanguage(const EngineRequestContext& context);
std::string NameRegistryDefaultIdentifierProfile(const EngineRequestContext& context);
std::string NameRegistryLookupKey(std::string text,
                                  const std::string& identifier_profile_uuid,
                                  bool requires_exact_match);
// SEARCH_KEY: SB_CATALOG_IDENTIFIER_CASE_FOLD (owner Q177, Q180-Q183,
// 2026-11-14). The one matching and collision rule of this registry (and of
// the parser server's registry leaf lookup): an entry's own-lane key (exact
// when quoted, folded when unquoted); a lookup key matches an entry when it
// equals that key; two entries collide when their own-lane keys are equal.
std::string NameRegistryEntryOwnLaneKey(const NameRegistryEntry& entry);
bool NameRegistryEntryMatchesLookupKey(const NameRegistryEntry& entry, const std::string& lookup_key);
bool NameRegistryLookupKeysCollide(const NameRegistryEntry& wanted, const NameRegistryEntry& existing);

NameRegistryEntry MakeNameRegistryEntry(const EngineRequestContext& context,
                                        const EngineUuid& object_uuid,
                                        const std::string& object_class,
                                        const EngineUuid& scope_uuid,
                                        const EngineLocalizedName& name,
                                        const std::string& fallback_name);

EngineApiDiagnostic AppendNameRegistryEntry(const EngineRequestContext& context,
                                            const NameRegistryEntry& entry,
                                            const std::string& operation_id);

EngineApiDiagnostic PersistNameRegistryEntriesForObject(const EngineRequestContext& context,
                                                        const std::string& operation_id,
                                                        const EngineUuid& object_uuid,
                                                        const std::string& object_class,
                                                        const EngineUuid& scope_uuid,
                                                        const std::vector<EngineLocalizedName>& names,
                                                        const std::string& fallback_name);

EngineApiDiagnostic RetireNameRegistryEntriesForObject(const EngineRequestContext& context,
                                                       const std::string& operation_id,
                                                       const EngineUuid& object_uuid);

NameRegistryResolveResult ResolveNameRegistryPrivate(const EngineApiRequest& request,
                                                     const std::string& requested_object_class = {});

NameRegistryResolveResult ResolveNameRegistryPublic(const EngineApiRequest& request,
                                                    const std::string& requested_object_class = {});

NameRegistryResolveResult ResolveNameRegistry(const EngineApiRequest& request,
                                              const std::string& requested_object_class = {});

NameRegistryNameResult MapNameRegistryUuidToNamePrivate(const EngineApiRequest& request,
                                                        const EngineUuid& object_uuid = {},
                                                        const std::string& requested_object_class = {});

NameRegistryNameResult MapNameRegistryUuidToNamePublic(const EngineApiRequest& request,
                                                       const EngineUuid& object_uuid = {},
                                                       const std::string& requested_object_class = {});

NameRegistryNameResult MapNameRegistryUuidToName(const EngineApiRequest& request,
                                                 const EngineUuid& object_uuid = {},
                                                 const std::string& requested_object_class = {});

bool NameRegistryWouldConflict(const EngineRequestContext& context,
                               const EngineUuid& object_uuid,
                               const std::string& object_class,
                               const EngineUuid& scope_uuid,
                               const std::vector<EngineLocalizedName>& names,
                               std::uint64_t observer_tx,
                               std::string* conflict_name,
                               EngineApiDiagnostic& diagnostic);

}  // namespace scratchbird::engine::internal_api
