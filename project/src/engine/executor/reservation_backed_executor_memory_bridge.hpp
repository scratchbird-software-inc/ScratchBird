// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-012 executor bridge for reservation-backed memory resources.
#include "operator_memory_grant.hpp"
#include "reservation_backed_memory_resource.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::executor {

struct ExecutorReservationBackedMemoryResult {
  scratchbird::core::platform::Status status;
  bool fail_closed = false;
  scratchbird::core::platform::u64 allocated_bytes = 0;
  scratchbird::core::platform::DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

ExecutorReservationBackedMemoryResult AllocateExecutorOperatorFromReservedResource(
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    ExecutorMemoryOperatorKind operator_kind,
    scratchbird::core::platform::u64 bytes,
    std::string purpose,
    ExecutorOperatorMemoryAuthority authority);

}  // namespace scratchbird::engine::executor
