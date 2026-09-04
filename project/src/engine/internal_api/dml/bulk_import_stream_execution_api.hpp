// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "sblr_bulk_import_stream_registry.hpp"

#include <cstdint>
#include <vector>

namespace scratchbird::engine::internal_api {

// Engine-owned terminal boundary for one exact, already sealed opcode-775
// stream.  The caller supplies no row bytes and no target or policy override;
// every executable fact is recovered from the durable descriptor registry and
// the exact live engine statement context.
struct EngineExecuteBulkImportStreamRequestV1 {
  EngineRequestContext context;
  SblrBulkImportStreamRegistry* registry = nullptr;
  std::vector<std::uint8_t> canonical_biro;
};

struct EngineExecuteBulkImportStreamResultV1 {
  bool ok = false;
  bool replayed = false;
  std::uint64_t affected_rows = 0;
  std::uint64_t rejected_rows = 0;
  std::vector<std::uint8_t> canonical_birs;
  EngineApiDiagnostic diagnostic;
};

EngineExecuteBulkImportStreamResultV1 ExecuteBulkImportStreamV1(
    const EngineExecuteBulkImportStreamRequestV1& request);

}  // namespace scratchbird::engine::internal_api
