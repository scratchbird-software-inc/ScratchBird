// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/executor/descriptor_value_runtime.hpp"
#include "engine/optimizer/model_family_coordinator.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

// Projects already retained binary descriptor authority. It neither allocates
// identities nor consults names to reconstruct missing authority.
std::optional<internal_api::EngineUuid> Rcp079DescriptorIdentity(
    const internal_api::EngineDescriptor& descriptor, std::string_view key);

// Wraps an admitted executor with exact output descriptor rebinding, preserving
// its cancellation evidence and accounting all retained input/output payloads.
executor::CanonicalPhysicalExecutorRegistration
WithMultilegResultDescriptorRebindingV1(
    executor::CanonicalPhysicalExecutorRegistration registration,
    std::vector<optimizer::MultilegDescriptorAllocationV1> allocations,
    std::string operation_name,
    std::function<bool()> cancellation_requested);

}  // namespace scratchbird::engine::sblr
