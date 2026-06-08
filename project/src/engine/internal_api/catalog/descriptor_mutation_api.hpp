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

// Engine-owned catalog descriptor mutation route for SBSQL catalog surfaces that
// already have canonical SBLR authority but do not yet require specialized
// physical storage APIs. Parser output is descriptor-only; catalog finality
// remains under engine/MGA authority.
struct EngineCatalogDescriptorMutationRequest : EngineApiRequest {};
struct EngineCatalogDescriptorMutationResult : EngineApiResult {};

EngineCatalogDescriptorMutationResult EngineCatalogDescriptorMutation(
    const EngineCatalogDescriptorMutationRequest& request);

}  // namespace scratchbird::engine::internal_api
