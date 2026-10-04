// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: AEIC_DURABLE_AGENT_CATALOG_ENTERPRISE
// SEARCH_KEY: AEIC_DURABLE_AGENT_MANAGEMENT_SURFACES

#include "agent_durable_catalog.hpp"
#include "api_diagnostics.hpp"
#include "api_types.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::internal_api {

inline constexpr const char* kAgentDurableCatalogStoreTableName =
    "sys.agent_durable_catalog_state";

// A load observation is not initialization authority. In particular, absence
// must be rechecked by the store under its publication guard before mutation.
enum class AgentDurableCatalogLoadDisposition {
  not_attempted,
  loaded,
  absent,
  existing_not_visible,
  source_failure,
  invalid_image,
};

struct AgentDurableCatalogStoreRequest {
  EngineRequestContext context;
  scratchbird::core::agents::DurableAgentCatalogImage image;
  std::string evidence_uuid;
  std::string expected_catalog_root_digest;
  bool production_live_path = true;
  bool fsync_or_checkpoint_evidence = false;
  bool initialize_only = false;
};

struct AgentDurableCatalogStoreResult {
  bool ok = false;
  AgentDurableCatalogLoadDisposition load_disposition =
      AgentDurableCatalogLoadDisposition::not_attempted;
  EngineApiDiagnostic diagnostic;
  scratchbird::core::agents::DurableAgentCatalogImage image;
  std::string table_uuid;
  std::string row_uuid;
  std::string version_uuid;
  std::uint64_t row_event_sequence = 0;
  std::string storage_linkage_digest;
  bool schema_migration_applied = false;
  bool schema_migration_persisted = false;
};

struct AgentDurableCatalogLoadRequest {
  EngineRequestContext context;
  bool production_live_path = true;
  bool persist_schema_migration = false;
  bool fsync_or_checkpoint_evidence = false;
  std::string migration_evidence_uuid;
};

AgentDurableCatalogStoreResult PersistAgentDurableCatalogImage(
    const AgentDurableCatalogStoreRequest& request);

AgentDurableCatalogStoreResult LoadAgentDurableCatalogImage(
    const EngineRequestContext& context,
    bool production_live_path);

AgentDurableCatalogStoreResult LoadAgentDurableCatalogImage(
    const AgentDurableCatalogLoadRequest& request);

}  // namespace scratchbird::engine::internal_api
