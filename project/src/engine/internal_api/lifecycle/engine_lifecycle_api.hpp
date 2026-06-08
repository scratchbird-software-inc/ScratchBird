// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_LIFECYCLE_API
struct EngineOpenLifecycleRequest : EngineApiRequest {};
struct EngineOpenLifecycleResult : EngineApiResult {};
EngineOpenLifecycleResult EngineOpenLifecycle(const EngineOpenLifecycleRequest& request);

struct EngineCreateLifecycleRequest : EngineApiRequest {};
struct EngineCreateLifecycleResult : EngineApiResult {};
EngineCreateLifecycleResult EngineCreateLifecycle(const EngineCreateLifecycleRequest& request);

struct EngineAttachLifecycleRequest : EngineApiRequest {};
struct EngineAttachLifecycleResult : EngineApiResult {};
EngineAttachLifecycleResult EngineAttachLifecycle(const EngineAttachLifecycleRequest& request);

struct EngineDetachLifecycleRequest : EngineApiRequest {};
struct EngineDetachLifecycleResult : EngineApiResult {};
EngineDetachLifecycleResult EngineDetachLifecycle(const EngineDetachLifecycleRequest& request);

struct EngineShutdownLifecycleRequest : EngineApiRequest {};
struct EngineShutdownLifecycleResult : EngineApiResult {};
EngineShutdownLifecycleResult EngineShutdownLifecycle(const EngineShutdownLifecycleRequest& request);

struct EngineEnterMaintenanceLifecycleRequest : EngineApiRequest {};
struct EngineEnterMaintenanceLifecycleResult : EngineApiResult {};
EngineEnterMaintenanceLifecycleResult EngineEnterMaintenanceLifecycle(const EngineEnterMaintenanceLifecycleRequest& request);

struct EngineExitMaintenanceLifecycleRequest : EngineApiRequest {};
struct EngineExitMaintenanceLifecycleResult : EngineApiResult {};
EngineExitMaintenanceLifecycleResult EngineExitMaintenanceLifecycle(const EngineExitMaintenanceLifecycleRequest& request);

struct EngineEnterRestrictedOpenLifecycleRequest : EngineApiRequest {};
struct EngineEnterRestrictedOpenLifecycleResult : EngineApiResult {};
EngineEnterRestrictedOpenLifecycleResult EngineEnterRestrictedOpenLifecycle(const EngineEnterRestrictedOpenLifecycleRequest& request);

struct EngineExitRestrictedOpenLifecycleRequest : EngineApiRequest {};
struct EngineExitRestrictedOpenLifecycleResult : EngineApiResult {};
EngineExitRestrictedOpenLifecycleResult EngineExitRestrictedOpenLifecycle(const EngineExitRestrictedOpenLifecycleRequest& request);

struct EngineInspectLifecycleRequest : EngineApiRequest {};
struct EngineInspectLifecycleResult : EngineApiResult {};
EngineInspectLifecycleResult EngineInspectLifecycle(const EngineInspectLifecycleRequest& request);

struct EngineVerifyLifecycleRequest : EngineApiRequest {};
struct EngineVerifyLifecycleResult : EngineApiResult {};
EngineVerifyLifecycleResult EngineVerifyLifecycle(const EngineVerifyLifecycleRequest& request);

struct EngineRepairLifecycleRequest : EngineApiRequest {};
struct EngineRepairLifecycleResult : EngineApiResult {};
EngineRepairLifecycleResult EngineRepairLifecycle(const EngineRepairLifecycleRequest& request);

struct EngineForceShutdownLifecycleRequest : EngineApiRequest {};
struct EngineForceShutdownLifecycleResult : EngineApiResult {};
EngineForceShutdownLifecycleResult EngineForceShutdownLifecycle(const EngineForceShutdownLifecycleRequest& request);

struct EngineAcknowledgeShutdownLifecycleRequest : EngineApiRequest {};
struct EngineAcknowledgeShutdownLifecycleResult : EngineApiResult {};
EngineAcknowledgeShutdownLifecycleResult EngineAcknowledgeShutdownLifecycle(
    const EngineAcknowledgeShutdownLifecycleRequest& request);

struct EngineDropLifecycleRequest : EngineApiRequest {};
struct EngineDropLifecycleResult : EngineApiResult {};
EngineDropLifecycleResult EngineDropLifecycle(const EngineDropLifecycleRequest& request);

}  // namespace scratchbird::engine::internal_api
