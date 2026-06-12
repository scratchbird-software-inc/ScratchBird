// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-012 SBLR/parser handoff buffer backed by an explicit reserved resource.
#include "reservation_backed_memory_resource.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrParserHandoffBufferResult {
  scratchbird::core::platform::Status status;
  bool fail_closed = false;
  scratchbird::core::platform::u64 payload_bytes = 0;
  std::string digest;
  scratchbird::core::platform::DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

SblrParserHandoffBufferResult BuildSblrParserHandoffBuffer(
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    std::string operation_id,
    std::string_view payload,
    bool engine_mga_authoritative,
    bool parser_or_reference_finality_authority,
    bool debug_or_relaxed_path);

}  // namespace scratchbird::engine::sblr
