// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_query_object_free_profile.hpp"

namespace scratchbird::engine::sblr {

// Internal synchronous-dispatch support. Wrappers borrow memory_receipts;
// the caller must keep them alive until all wrapped callbacks finish.
// Only exact producer receipts qualify. Grants do not become observations.
void PublishOrdinaryRuntimeObservations(
    std::vector<scratchbird::engine::executor::CanonicalPhysicalExecutorRegistration>*
        registrations,
    const OrdinaryRuntimeMemoryReceipts& memory_receipts);

bool HasOrdinaryRuntimeObservationWrapperTarget(
    const std::vector<scratchbird::engine::executor::CanonicalPhysicalExecutorRegistration>&
        registrations,
    const OrdinaryRuntimeMemoryReceipts& memory_receipts);

}  // namespace scratchbird::engine::sblr
