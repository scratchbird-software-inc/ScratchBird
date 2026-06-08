// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "observability/performance_optimization_surface.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_OBSERVABILITY_SHOW_API
struct EngineShowVersionRequest : EngineApiRequest {};
struct EngineShowVersionResult : EngineApiResult {};
EngineShowVersionResult EngineShowVersion(const EngineShowVersionRequest& request);

struct EngineShowDatabaseRequest : EngineApiRequest {};
struct EngineShowDatabaseResult : EngineApiResult {};
EngineShowDatabaseResult EngineShowDatabase(const EngineShowDatabaseRequest& request);

struct EngineShowSystemRequest : EngineApiRequest {};
struct EngineShowSystemResult : EngineApiResult {};
EngineShowSystemResult EngineShowSystem(const EngineShowSystemRequest& request);

struct EngineShowCatalogRequest : EngineApiRequest {};
struct EngineShowCatalogResult : EngineApiResult {};
EngineShowCatalogResult EngineShowCatalog(const EngineShowCatalogRequest& request);

struct EngineShowSessionsRequest : EngineApiRequest {};
struct EngineShowSessionsResult : EngineApiResult {};
EngineShowSessionsResult EngineShowSessions(const EngineShowSessionsRequest& request);

struct EngineShowTransactionsRequest : EngineApiRequest {};
struct EngineShowTransactionsResult : EngineApiResult {};
EngineShowTransactionsResult EngineShowTransactions(const EngineShowTransactionsRequest& request);

struct EngineShowLocksRequest : EngineApiRequest {};
struct EngineShowLocksResult : EngineApiResult {};
EngineShowLocksResult EngineShowLocks(const EngineShowLocksRequest& request);

struct EngineShowStatementsRequest : EngineApiRequest {};
struct EngineShowStatementsResult : EngineApiResult {};
EngineShowStatementsResult EngineShowStatements(const EngineShowStatementsRequest& request);

struct EngineShowJobsRequest : EngineApiRequest {};
struct EngineShowJobsResult : EngineApiResult {};
EngineShowJobsResult EngineShowJobs(const EngineShowJobsRequest& request);

struct EngineShowManagementRequest : EngineApiRequest {
  PerformanceOptimizationSurfaceSnapshot performance_optimization_snapshot;
  bool performance_optimization_snapshot_present = false;
};
struct EngineShowManagementResult : EngineApiResult {};
EngineShowManagementResult EngineShowManagement(const EngineShowManagementRequest& request);

struct EngineShowDiagnosticsRequest : EngineApiRequest {};
struct EngineShowDiagnosticsResult : EngineApiResult {};
EngineShowDiagnosticsResult EngineShowDiagnostics(const EngineShowDiagnosticsRequest& request);

struct EngineShowDiagnosticsExtendedRequest : EngineApiRequest {};
struct EngineShowDiagnosticsExtendedResult : EngineApiResult {};
EngineShowDiagnosticsExtendedResult EngineShowDiagnosticsExtended(
    const EngineShowDiagnosticsExtendedRequest& request);

struct EngineShowArchiveReplicationRequest : EngineApiRequest {};
struct EngineShowArchiveReplicationResult : EngineApiResult {};
EngineShowArchiveReplicationResult EngineShowArchiveReplication(
    const EngineShowArchiveReplicationRequest& request);

struct EngineShowAgentsExtendedRequest : EngineApiRequest {};
struct EngineShowAgentsExtendedResult : EngineApiResult {};
EngineShowAgentsExtendedResult EngineShowAgentsExtended(
    const EngineShowAgentsExtendedRequest& request);

struct EngineShowFilespaceExtendedRequest : EngineApiRequest {};
struct EngineShowFilespaceExtendedResult : EngineApiResult {};
EngineShowFilespaceExtendedResult EngineShowFilespaceExtended(
    const EngineShowFilespaceExtendedRequest& request);

struct EngineShowDecisionServiceRequest : EngineApiRequest {};
struct EngineShowDecisionServiceResult : EngineApiResult {};
EngineShowDecisionServiceResult EngineShowDecisionService(
    const EngineShowDecisionServiceRequest& request);

struct EngineShowAccelerationRequest : EngineApiRequest {};
struct EngineShowAccelerationResult : EngineApiResult {};
EngineShowAccelerationResult EngineShowAcceleration(
    const EngineShowAccelerationRequest& request);

struct EngineShowAccelerationExtendedRequest : EngineApiRequest {};
struct EngineShowAccelerationExtendedResult : EngineApiResult {};
EngineShowAccelerationExtendedResult EngineShowAccelerationExtended(
    const EngineShowAccelerationExtendedRequest& request);

struct EngineInspectShowOperationRequest : EngineApiRequest {};
struct EngineInspectShowOperationResult : EngineApiResult {};
EngineInspectShowOperationResult EngineInspectShowOperation(
    const EngineInspectShowOperationRequest& request);

}  // namespace scratchbird::engine::internal_api
