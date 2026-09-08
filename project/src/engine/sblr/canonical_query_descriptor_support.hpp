// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/executor/descriptor_value_runtime.hpp"
#include "query/plan_api.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace scratchbird::engine::sblr {

// Owns exact descriptor/result-shape comparison and nullability projection.
// It does not create catalog descriptors or validate persisted authority.
scratchbird::engine::executor::CanonicalResultNullability ResultNullability(
    scratchbird::engine::internal_api::RelationalNullability nullability);

bool SameExactEngineDescriptorV1(
    const scratchbird::engine::internal_api::EngineDescriptor& left,
    const scratchbird::engine::internal_api::EngineDescriptor& right);

bool SameExactRelationalTypeDescriptorV2(
    const scratchbird::engine::internal_api::RelationalTypeDescriptor& left,
    const scratchbird::engine::internal_api::RelationalTypeDescriptor& right);

bool CanonicalQueryEngineDescriptorExactlyEqual(
    const scratchbird::engine::internal_api::EngineDescriptor& left,
    const scratchbird::engine::internal_api::EngineDescriptor& right);

bool CanonicalQueryTypedValuePayloadExactlyEqual(
    const scratchbird::engine::internal_api::EngineTypedValue& left,
    const scratchbird::engine::internal_api::EngineTypedValue& right);

bool CanonicalQueryDescriptorTuplePayloadExactlyEqual(
    const scratchbird::engine::executor::DescriptorTuple& left,
    const scratchbird::engine::executor::DescriptorTuple& right);

bool CanonicalQueryDescriptorBatchesExactlyEqual(
    const scratchbird::engine::executor::DescriptorBatch& left,
    const scratchbird::engine::executor::DescriptorBatch& right);

std::optional<std::string> ExactEncodedDescriptorField(
    std::string_view descriptor,
    std::string_view key);

}  // namespace scratchbird::engine::sblr
