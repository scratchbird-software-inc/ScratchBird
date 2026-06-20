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
#include "crud_support/crud_store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_API_BEHAVIOR_STORE
// Shared local-node behavior/event store for non-cluster engine internal APIs.

inline constexpr const char* kApiBehaviorEventMagic = "SBAPI1";

struct ApiBehaviorRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string operation_id;
  std::string object_uuid;
  std::string object_kind;
  // Migration/display cache only. SQL object name authority is SBNAME1 name registry.
  std::string default_name;
  std::string payload;
  std::string state;
  bool deleted = false;
};

struct ApiBehaviorState {
  std::vector<ApiBehaviorRecord> records;
};

struct ApiBehaviorStoreResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  ApiBehaviorState state;
};

ApiBehaviorStoreResult LoadApiBehaviorState(const EngineRequestContext& context);
EngineApiDiagnostic AppendApiBehaviorEvent(const EngineRequestContext& context, const std::string& event);
std::string MakeApiBehaviorRecordEvent(const ApiBehaviorRecord& record);
std::string ApiBehaviorPrimaryName(const EngineApiRequest& request, const std::string& fallback);
std::string ApiBehaviorPayloadFromRequest(const EngineApiRequest& request);
std::string ApiBehaviorObjectUuid(const EngineApiRequest& request, const std::string& kind);
EngineTypedValue ApiBehaviorValue(std::string value);
EngineRowValue ApiBehaviorRow(std::vector<std::pair<std::string, std::string>> fields);
void AddApiBehaviorRow(EngineApiResult* result, std::vector<std::pair<std::string, std::string>> fields);
void AddApiBehaviorEvidence(EngineApiResult* result, std::string kind, std::string id);
void AddDdlPublicationResult(EngineApiResult* result,
                             const std::string& operation_id,
                             const std::string& object_kind,
                             const std::string& object_uuid,
                             const std::string& catalog_row_uuid = {},
                             const std::string& invalidation_scope = {});
std::vector<ApiBehaviorRecord> VisibleApiBehaviorRecords(const EngineRequestContext& context,
                                                         const std::string& object_kind,
                                                         std::uint64_t observer_tx);
std::optional<ApiBehaviorRecord> FindVisibleApiBehaviorRecord(const EngineRequestContext& context,
                                                              const std::string& object_uuid,
                                                              std::uint64_t observer_tx);
EngineDescriptor ApiBehaviorDescriptor(const ApiBehaviorRecord& record);
EngineApiDiagnostic ValidateApiBehaviorContext(const EngineRequestContext& context,
                                               const std::string& operation_id,
                                               bool require_transaction,
                                               bool require_database_path = true);

struct ApiBehaviorPersistedRecord {
  EngineApiDiagnostic diagnostic;
  ApiBehaviorRecord record;
  bool ok = false;
};

ApiBehaviorPersistedRecord PersistApiBehaviorRecord(const EngineApiRequest& request,
                                                    const std::string& operation_id,
                                                    const std::string& object_kind,
                                                    bool require_transaction,
                                                    std::string explicit_state = "active",
                                                    bool deleted = false);
ApiBehaviorPersistedRecord PersistApiBehaviorRecordWithPayload(const EngineApiRequest& request,
                                                               const std::string& operation_id,
                                                               const std::string& object_kind,
                                                               bool require_transaction,
                                                               std::string explicit_state,
                                                               bool deleted,
                                                               std::string payload_override);

template <typename TResult>
TResult MakeApiBehaviorSuccess(const EngineRequestContext& context, std::string operation_id) {
  return MakeCrudSuccessResult<TResult>(context, std::move(operation_id));
}

template <typename TResult>
TResult MakeApiBehaviorDiagnostic(const EngineRequestContext& context,
                                  std::string operation_id,
                                  EngineApiDiagnostic diagnostic) {
  return MakeCrudDiagnosticResult<TResult>(context, std::move(operation_id), std::move(diagnostic));
}

template <typename TResult, typename TRequest>
TResult PersistedRecordResult(const TRequest& request,
                              const std::string& operation_id,
                              const std::string& object_kind,
                              bool require_transaction = true,
                              std::string state = "active",
                              bool deleted = false) {
  const auto persisted = PersistApiBehaviorRecord(request, operation_id, object_kind, require_transaction, std::move(state), deleted);
  if (!persisted.ok) {
    return MakeApiBehaviorDiagnostic<TResult>(request.context, operation_id, persisted.diagnostic);
  }
  auto result = MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
  result.primary_object.uuid.canonical = persisted.record.object_uuid;
  result.primary_object.object_kind = persisted.record.object_kind;
  result.catalog_row_uuid.canonical = GenerateCrudEngineUuid("row");
  AddApiBehaviorEvidence(&result, "api_behavior_event", operation_id);
  AddApiBehaviorEvidence(&result, persisted.record.object_kind, persisted.record.object_uuid);
  AddApiBehaviorRow(&result, {{"object_uuid", persisted.record.object_uuid},
                              {"object_kind", persisted.record.object_kind},
                              {"name", persisted.record.default_name},
                              {"state", persisted.record.state},
                              {"payload", persisted.record.payload}});
  AddDdlPublicationResult(&result,
                          operation_id,
                          persisted.record.object_kind,
                          persisted.record.object_uuid,
                          result.catalog_row_uuid.canonical);
  return result;
}

template <typename TResult, typename TRequest>
TResult PersistedRecordResultWithPayload(const TRequest& request,
                                         const std::string& operation_id,
                                         const std::string& object_kind,
                                         bool require_transaction,
                                         std::string state,
                                         bool deleted,
                                         std::string payload_override) {
  const auto persisted = PersistApiBehaviorRecordWithPayload(request,
                                                            operation_id,
                                                            object_kind,
                                                            require_transaction,
                                                            std::move(state),
                                                            deleted,
                                                            std::move(payload_override));
  if (!persisted.ok) {
    return MakeApiBehaviorDiagnostic<TResult>(request.context, operation_id, persisted.diagnostic);
  }
  auto result = MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
  result.primary_object.uuid.canonical = persisted.record.object_uuid;
  result.primary_object.object_kind = persisted.record.object_kind;
  result.catalog_row_uuid.canonical = GenerateCrudEngineUuid("row");
  AddApiBehaviorEvidence(&result, "api_behavior_event", operation_id);
  AddApiBehaviorEvidence(&result, persisted.record.object_kind, persisted.record.object_uuid);
  AddApiBehaviorRow(&result, {{"object_uuid", persisted.record.object_uuid},
                              {"object_kind", persisted.record.object_kind},
                              {"name", persisted.record.default_name},
                              {"state", persisted.record.state},
                              {"payload", persisted.record.payload}});
  AddDdlPublicationResult(&result,
                          operation_id,
                          persisted.record.object_kind,
                          persisted.record.object_uuid,
                          result.catalog_row_uuid.canonical);
  return result;
}

}  // namespace scratchbird::engine::internal_api
