// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CATALOG_SCHEMA_TREE_API
struct EngineSchemaTreeRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string schema_uuid;
  std::string parent_schema_uuid;
  // Migration/display cache only. SQL object name authority is SBNAME1 name registry.
  std::string default_name;
  // Parser/request compatibility cache. Durable authority is SBNAME1 name registry.
  std::vector<EngineLocalizedName> localized_names;
  std::vector<std::pair<std::string, std::string>> localized_comments;
  std::string payload;
  std::string state = "active";
};

struct EngineListCatalogChildrenRequest : EngineApiRequest {};
struct EngineListCatalogChildrenResult : EngineApiResult {};
EngineListCatalogChildrenResult EngineListCatalogChildren(const EngineListCatalogChildrenRequest& request);

std::vector<EngineSchemaTreeRecord> VisibleSchemaTreeRecords(const EngineRequestContext& context,
                                                             std::uint64_t observer_tx);
std::optional<EngineSchemaTreeRecord> FindVisibleSchemaTreeRecord(const EngineRequestContext& context,
                                                                  const std::string& schema_uuid,
                                                                  std::uint64_t observer_tx);
std::string SchemaTreeDefaultName(const std::vector<EngineLocalizedName>& names, const std::string& fallback);
std::string SchemaTreePayload(const std::string& parent_schema_uuid,
                              const std::vector<EngineLocalizedName>& names,
                              const std::vector<std::pair<std::string, std::string>>& comments);
std::optional<std::string> SchemaTreePathConflict(const EngineRequestContext& context,
                                                  const std::string& schema_uuid,
                                                  const std::string& parent_schema_uuid,
                                                  const std::vector<EngineLocalizedName>& names,
                                                  std::uint64_t observer_tx);
bool SchemaTreeWouldCreateCycle(const EngineRequestContext& context,
                                const std::string& schema_uuid,
                                const std::string& proposed_parent_schema_uuid,
                                std::uint64_t observer_tx);
EngineApiDiagnostic PersistSchemaTreeRecord(const EngineRequestContext& context,
                                            const EngineSchemaTreeRecord& record,
                                            const std::string& operation_id);

}  // namespace scratchbird::engine::internal_api
