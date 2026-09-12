// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <array>
#include <cstdint>

namespace scratchbird::core::uuid {

// A diagnostic occurrence is an identity of a source record, not the identity
// of its registered code. Allocate once at the source; copies and bridges
// retain these exact binary bytes. This is not a parser-facing MessageVector.
// Entropy/clock/generation failure throws; no nil or invented-time fallback.
std::array<std::uint8_t, 16> NewDiagnosticOccurrenceUuid();

}  // namespace scratchbird::core::uuid
