// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "descriptor_value_runtime.hpp"

namespace scratchbird::engine::executor {
// Structural refusals retain their detail; a failed datatype catalog lookup
// additionally reports its original status, including allocation exhaustion.
bool BuildBoundExecutionTypeDescriptor(
    const internal_api::EngineDescriptor& binding,
    core::datatypes::CanonicalTypeId type, ExecutionTypeDescriptor* descriptor,
    std::string* detail, core::platform::Status* provider_status);
}
