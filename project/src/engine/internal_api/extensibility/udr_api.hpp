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

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_EXTENSIBILITY_UDR_API
struct EngineRegisterUdrPackageRequest : EngineApiRequest {};
struct EngineRegisterUdrPackageResult : EngineApiResult {};
EngineRegisterUdrPackageResult EngineRegisterUdrPackage(const EngineRegisterUdrPackageRequest& request);

struct EngineLoadUdrPackageRequest : EngineApiRequest {};
struct EngineLoadUdrPackageResult : EngineApiResult {};
EngineLoadUdrPackageResult EngineLoadUdrPackage(const EngineLoadUdrPackageRequest& request);

struct EngineUnloadUdrPackageRequest : EngineApiRequest {};
struct EngineUnloadUdrPackageResult : EngineApiResult {};
EngineUnloadUdrPackageResult EngineUnloadUdrPackage(const EngineUnloadUdrPackageRequest& request);

struct EngineInspectUdrPackageRequest : EngineApiRequest {};
struct EngineInspectUdrPackageResult : EngineApiResult {};
EngineInspectUdrPackageResult EngineInspectUdrPackages(const EngineInspectUdrPackageRequest& request);

struct EngineInvokeUdrPackageRequest : EngineApiRequest {};
struct EngineInvokeUdrPackageResult : EngineApiResult {};
EngineInvokeUdrPackageResult EngineInvokeUdrPackage(const EngineInvokeUdrPackageRequest& request);

}  // namespace scratchbird::engine::internal_api
