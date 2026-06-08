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
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_EXECUTABLE_OBJECT_LIFECYCLE
// Engine-owned executable database object lifecycle. Parsers supply resolved
// UUID/SBLR operations; this API owns catalog-visible executable state.

inline constexpr const char* kExecutableObjectLifecycleEventMagic = "SBEXECOBJ1";

inline constexpr const char* kExecutableObjectDiagnosticDatabasePathRequired =
    "EXECUTABLE.OBJECT.DATABASE_PATH_REQUIRED";
inline constexpr const char* kExecutableObjectDiagnosticDatabaseWriteFailed =
    "EXECUTABLE.OBJECT.DATABASE_WRITE_FAILED";
inline constexpr const char* kExecutableObjectDiagnosticMgaTransactionRequired =
    "EXECUTABLE.OBJECT.MGA_TRANSACTION_REQUIRED";
inline constexpr const char* kExecutableObjectDiagnosticSecurityContextRequired =
    "EXECUTABLE.OBJECT.SECURITY_CONTEXT_REQUIRED";
inline constexpr const char* kExecutableObjectDiagnosticPermissionDenied =
    "EXECUTABLE.OBJECT.PERMISSION_DENIED";
inline constexpr const char* kExecutableObjectDiagnosticUuidRequired =
    "EXECUTABLE.OBJECT.UUID_REQUIRED";
inline constexpr const char* kExecutableObjectDiagnosticKindRequired =
    "EXECUTABLE.OBJECT.KIND_REQUIRED";
inline constexpr const char* kExecutableObjectDiagnosticUnsupportedKind =
    "EXECUTABLE.OBJECT.UNSUPPORTED_KIND";
inline constexpr const char* kExecutableObjectDiagnosticSchemaUuidRequired =
    "EXECUTABLE.OBJECT.SCHEMA_UUID_REQUIRED";
inline constexpr const char* kExecutableObjectDiagnosticDuplicate =
    "EXECUTABLE.OBJECT.DUPLICATE";
inline constexpr const char* kExecutableObjectDiagnosticNotFound =
    "EXECUTABLE.OBJECT.NOT_FOUND";
inline constexpr const char* kExecutableObjectDiagnosticMgaVisibilityRefused =
    "EXECUTABLE.OBJECT.MGA_VISIBILITY_REFUSED";
inline constexpr const char* kExecutableObjectDiagnosticStoredSblrRequired =
    "EXECUTABLE.OBJECT.STORED_SBLR_REQUIRED";
inline constexpr const char* kExecutableObjectDiagnosticStoredSblrProvenanceRequired =
    "EXECUTABLE.OBJECT.STORED_SBLR_PROVENANCE_REQUIRED";
inline constexpr const char* kExecutableObjectDiagnosticInternalProcedureRequired =
    "EXECUTABLE.OBJECT.INTERNAL_PROCEDURE_REQUIRED";
inline constexpr const char* kExecutableObjectDiagnosticExecutionBoundaryRefused =
    "EXECUTABLE.OBJECT.EXECUTION_BOUNDARY_REFUSED";
inline constexpr const char* kExecutableObjectDiagnosticDependencyNotVisible =
    "EXECUTABLE.OBJECT.DEPENDENCY_NOT_VISIBLE";
inline constexpr const char* kExecutableObjectDiagnosticDependencyInvalidated =
    "EXECUTABLE.OBJECT.DEPENDENCY_INVALIDATED";
inline constexpr const char* kExecutableObjectDiagnosticSideEffectPolicyDenied =
    "EXECUTABLE.OBJECT.SIDE_EFFECT_POLICY_DENIED";
inline constexpr const char* kExecutableObjectDiagnosticUnloadBlockedActiveInvocation =
    "EXECUTABLE.OBJECT.UNLOAD_BLOCKED_ACTIVE_INVOCATION";
inline constexpr const char* kExecutableObjectDiagnosticQuiescing =
    "EXECUTABLE.OBJECT.QUIESCING";
inline constexpr const char* kExecutableObjectDiagnosticUnloaded =
    "EXECUTABLE.OBJECT.UNLOADED";
inline constexpr const char* kExecutableObjectDiagnosticInvocationNotFound =
    "EXECUTABLE.OBJECT.INVOCATION_NOT_FOUND";
inline constexpr const char* kExecutableObjectDiagnosticEventTriggerAuthorityUnavailable =
    "SBSQL.EVENT_TRIGGER_AUTHORITY_UNAVAILABLE";
inline constexpr const char* kExecutableObjectDiagnosticEventTriggerEventUnsupported =
    "EXECUTABLE.OBJECT.EVENT_TRIGGER_EVENT_UNSUPPORTED";

struct EngineExecutableObjectRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string object_uuid;
  std::string object_kind;
  std::string schema_uuid;
  std::string owner_principal_uuid;
  std::string package_uuid;
  std::string lifecycle_state = "active";
  std::uint64_t executable_generation = 0;
  std::uint64_t metadata_epoch = 0;
  std::string executor_kind = "sblr";
  std::string stored_sblr_hash;
  std::string stored_sblr_provenance;
  std::string internal_procedure_id;
  std::string side_effect_class = "none";
  std::string event_trigger_event;
  std::string payload;
  bool invalidated = false;
  std::uint64_t invalidated_generation = 0;
  std::string invalidation_reason_uuid;
  bool deleted = false;
};

struct EngineExecutableDependencyRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string source_uuid;
  std::string source_kind;
  std::string dependency_uuid;
  std::string dependency_kind;
  std::uint64_t dependency_generation = 0;
  std::uint64_t metadata_epoch = 0;
  bool deleted = false;
};

struct EngineExecutableInvocationRecord {
  std::uint64_t creator_tx = 0;
  std::uint64_t event_sequence = 0;
  std::string invocation_lease_uuid;
  std::string object_uuid;
  std::uint64_t executable_generation = 0;
  std::string lifecycle_state = "active";
  std::uint64_t metadata_epoch = 0;
};

struct EngineExecutableObjectLifecycleState {
  std::vector<EngineExecutableObjectRecord> objects;
  std::vector<EngineExecutableDependencyRecord> dependencies;
  std::vector<EngineExecutableInvocationRecord> active_invocations;
  std::uint64_t metadata_epoch = 0;
  std::uint64_t dependency_generation = 0;
};

struct EngineLoadExecutableObjectLifecycleStateResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineExecutableObjectLifecycleState state;
};

struct EngineExecutableObjectLifecycleResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  std::uint64_t executable_generation = 0;
  std::uint64_t metadata_cache_epoch = 0;
  std::uint64_t active_invocation_count = 0;
  std::string invocation_lease_uuid;
};

struct EngineCreateExecutableObjectRequest : EngineApiRequest {};
struct EngineAlterExecutableObjectRequest : EngineApiRequest {};
struct EngineDropExecutableObjectRequest : EngineApiRequest {};
struct EngineQuiesceExecutableObjectRequest : EngineApiRequest {};
struct EngineUnloadExecutableObjectRequest : EngineApiRequest {};
struct EngineBeginExecutableObjectInvocationRequest : EngineApiRequest {};
struct EngineFinishExecutableObjectInvocationRequest : EngineApiRequest {};
struct EngineInvokeExecutableObjectRequest : EngineApiRequest {};
struct EngineFireExecutableEventTriggerRequest : EngineApiRequest {};
struct EngineInspectExecutableObjectRequest : EngineApiRequest {};

using EngineCreateExecutableObjectResult = EngineExecutableObjectLifecycleResult;
using EngineAlterExecutableObjectResult = EngineExecutableObjectLifecycleResult;
using EngineDropExecutableObjectResult = EngineExecutableObjectLifecycleResult;
using EngineQuiesceExecutableObjectResult = EngineExecutableObjectLifecycleResult;
using EngineUnloadExecutableObjectResult = EngineExecutableObjectLifecycleResult;
using EngineBeginExecutableObjectInvocationResult = EngineExecutableObjectLifecycleResult;
using EngineFinishExecutableObjectInvocationResult = EngineExecutableObjectLifecycleResult;
using EngineInvokeExecutableObjectResult = EngineExecutableObjectLifecycleResult;
using EngineFireExecutableEventTriggerResult = EngineExecutableObjectLifecycleResult;
using EngineInspectExecutableObjectResult = EngineExecutableObjectLifecycleResult;

EngineCreateExecutableObjectResult EngineCreateExecutableObject(
    const EngineCreateExecutableObjectRequest& request);
EngineAlterExecutableObjectResult EngineAlterExecutableObject(
    const EngineAlterExecutableObjectRequest& request);
EngineDropExecutableObjectResult EngineDropExecutableObject(
    const EngineDropExecutableObjectRequest& request);
EngineQuiesceExecutableObjectResult EngineQuiesceExecutableObject(
    const EngineQuiesceExecutableObjectRequest& request);
EngineUnloadExecutableObjectResult EngineUnloadExecutableObject(
    const EngineUnloadExecutableObjectRequest& request);
EngineBeginExecutableObjectInvocationResult EngineBeginExecutableObjectInvocation(
    const EngineBeginExecutableObjectInvocationRequest& request);
EngineFinishExecutableObjectInvocationResult EngineFinishExecutableObjectInvocation(
    const EngineFinishExecutableObjectInvocationRequest& request);
EngineInvokeExecutableObjectResult EngineInvokeExecutableObject(
    const EngineInvokeExecutableObjectRequest& request);
EngineFireExecutableEventTriggerResult EngineFireExecutableEventTrigger(
    const EngineFireExecutableEventTriggerRequest& request);
EngineInspectExecutableObjectResult EngineInspectExecutableObjects(
    const EngineInspectExecutableObjectRequest& request);

EngineLoadExecutableObjectLifecycleStateResult LoadExecutableObjectLifecycleState(
    const EngineRequestContext& context);

}  // namespace scratchbird::engine::internal_api
