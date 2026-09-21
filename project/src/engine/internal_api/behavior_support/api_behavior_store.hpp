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
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
// Explicit target identity or newly issued binary UUID. Related dependencies
// must never be adopted implicitly as the primary object's identity.
EngineUuid ApiBehaviorObjectUuid(const EngineApiRequest& request, const std::string& kind);
EngineTypedValue ApiBehaviorValue(std::string value);
EngineTypedValue ApiBehaviorValue(const EngineUuid& value);
// Legacy result carriers preserve the source value category. UUID values are
// raw16, never converted to strings or inferred from a field name/text spelling.
// The string/UUID convenience values have no bound datatype descriptor and do
// not authorize canonical result publication. Already-bound typed values retain
// their exact descriptor; the publication owner must independently validate it.
using ApiBehaviorValueInput = std::variant<std::string, EngineUuid, EngineTypedValue>;
using ApiBehaviorFields = std::vector<std::pair<std::string, ApiBehaviorValueInput>>;
EngineRowValue ApiBehaviorRow(ApiBehaviorFields fields);
void AddApiBehaviorRow(EngineApiResult* result, ApiBehaviorFields fields);
// Explicit text vectors remain text. This template cannot steal a mixed braced
// field list from the typed overload, nor reinterpret UUID-looking user TEXT.
template<class Allocator>
EngineRowValue ApiBehaviorRow(std::vector<std::pair<std::string, std::string>, Allocator> fields) {
  ApiBehaviorFields values;
  values.reserve(fields.size());
  for (auto& [name, value] : fields) values.emplace_back(std::move(name), std::move(value));
  return ApiBehaviorRow(std::move(values));
}
template<class Allocator>
void AddApiBehaviorRow(EngineApiResult* result,
    std::vector<std::pair<std::string, std::string>, Allocator> fields) {
  ApiBehaviorFields values;
  values.reserve(fields.size());
  for (auto& [name, value] : fields) values.emplace_back(std::move(name), std::move(value));
  AddApiBehaviorRow(result, std::move(values));
}
void AddApiBehaviorEvidence(EngineApiResult* result, std::string kind, EngineEvidenceValue id);
// Append one complete private evidence group. A namespace qualifies kinds only;
// values retain their exact TEXT/UUID alternatives and bytes. Source may alias
// destination. Allocation failure leaves destination's contents unchanged.
// This transports observations, not identity admission or execution authority.
void AppendApiEvidenceGroup(std::vector<EngineEvidenceReference>& destination,
                            std::span<const EngineEvidenceReference> source,
                            std::string_view evidence_namespace = {});
// Legacy helper name: binds response identity only. It cannot certify executed
// DDL stages or create a catalog row. Invalid bindings refuse the result while
// preserving existing diagnostics; allocation failure leaves it unchanged.
void AddDdlPublicationResult(EngineApiResult* result,
                             const std::string& operation_id,
                             const std::string& object_kind,
                             const EngineUuid& object_uuid,
                             const EngineUuid& catalog_row_uuid = {},
                             const std::string& invalidation_scope = {});
// The diagnostic is mandatory: an empty vector/optional is ordinary absence
// only when it is non-error. Failure never certifies a cache miss or nonexistence.
std::vector<ApiBehaviorRecord> VisibleApiBehaviorRecords(const EngineRequestContext& context,
                                                         const std::string& object_kind,
                                                         std::uint64_t observer_tx,
                                                         EngineApiDiagnostic& diagnostic);
std::optional<ApiBehaviorRecord> FindVisibleApiBehaviorRecord(const EngineRequestContext& context,
                                                              const std::string& object_uuid,
                                                              std::uint64_t observer_tx,
                                                              EngineApiDiagnostic& diagnostic);
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
  result.primary_object.uuid = persisted.record.object_uuid;
  result.primary_object.object_kind = persisted.record.object_kind;
  // An event append has no catalog-row receipt. Do not mint one after the fact.
  // The actual catalog mutation owner must supply its persisted row identity.
  result.catalog_row_uuid = {};
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
                          result.catalog_row_uuid);
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
  result.primary_object.uuid = persisted.record.object_uuid;
  result.primary_object.object_kind = persisted.record.object_kind;
  // An event append has no catalog-row receipt. Do not mint one after the fact.
  // The actual catalog mutation owner must supply its persisted row identity.
  result.catalog_row_uuid = {};
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
                          result.catalog_row_uuid);
  return result;
}

}  // namespace scratchbird::engine::internal_api
