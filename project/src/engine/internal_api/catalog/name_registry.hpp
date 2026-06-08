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

inline constexpr const char* kNameRegistryEventMagic = "SBNAME1";

struct NameRegistryEntry {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string name_entry_uuid;
  std::string object_uuid;
  std::string object_class;
  std::string scope_uuid;
  std::string parent_object_uuid;
  std::string parent_schema_uuid;
  std::string language_tag = "en";
  std::string name_class = "primary";
  std::string donor_id;
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

std::string NameRegistryDefaultLanguage(const EngineRequestContext& context);
std::string NameRegistrySessionLanguage(const EngineRequestContext& context);
std::string NameRegistryDefaultIdentifierProfile(const EngineRequestContext& context);
std::string NameRegistryLookupKey(std::string text,
                                  const std::string& identifier_profile_uuid,
                                  bool requires_exact_match);

NameRegistryEntry MakeNameRegistryEntry(const EngineRequestContext& context,
                                        const std::string& object_uuid,
                                        const std::string& object_class,
                                        const std::string& scope_uuid,
                                        const EngineLocalizedName& name,
                                        const std::string& fallback_name);

EngineApiDiagnostic AppendNameRegistryEntry(const EngineRequestContext& context,
                                            const NameRegistryEntry& entry,
                                            const std::string& operation_id);

EngineApiDiagnostic PersistNameRegistryEntriesForObject(const EngineRequestContext& context,
                                                        const std::string& operation_id,
                                                        const std::string& object_uuid,
                                                        const std::string& object_class,
                                                        const std::string& scope_uuid,
                                                        const std::vector<EngineLocalizedName>& names,
                                                        const std::string& fallback_name);

EngineApiDiagnostic RetireNameRegistryEntriesForObject(const EngineRequestContext& context,
                                                       const std::string& operation_id,
                                                       const std::string& object_uuid);

NameRegistryResolveResult ResolveNameRegistryPrivate(const EngineApiRequest& request,
                                                     const std::string& requested_object_class = {});

NameRegistryResolveResult ResolveNameRegistryPublic(const EngineApiRequest& request,
                                                    const std::string& requested_object_class = {});

NameRegistryResolveResult ResolveNameRegistry(const EngineApiRequest& request,
                                              const std::string& requested_object_class = {});

NameRegistryNameResult MapNameRegistryUuidToNamePrivate(const EngineApiRequest& request,
                                                        const std::string& object_uuid = {},
                                                        const std::string& requested_object_class = {});

NameRegistryNameResult MapNameRegistryUuidToNamePublic(const EngineApiRequest& request,
                                                       const std::string& object_uuid = {},
                                                       const std::string& requested_object_class = {});

NameRegistryNameResult MapNameRegistryUuidToName(const EngineApiRequest& request,
                                                 const std::string& object_uuid = {},
                                                 const std::string& requested_object_class = {});

bool NameRegistryWouldConflict(const EngineRequestContext& context,
                               const std::string& object_uuid,
                               const std::string& object_class,
                               const std::string& scope_uuid,
                               const std::vector<EngineLocalizedName>& names,
                               std::uint64_t observer_tx,
                               std::string* conflict_name);

}  // namespace scratchbird::engine::internal_api
