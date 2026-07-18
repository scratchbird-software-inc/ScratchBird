// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "scratchbird/engine/engine.h"

#include <cstddef>
#include <string>
#include <vector>

namespace scratchbird::server_engine_bridge {

// Private, family-neutral server-to-engine diagnostic transport. Structured
// fields deliberately do not extend or reinterpret the frozen public C ABI.
struct EngineDiagnosticField {
  std::string key;
  std::string value;
};

bool CopyEngineDiagnosticFields(
    sb_engine_result_t result,
    std::size_t diagnostic_index,
    std::vector<EngineDiagnosticField>* fields);

}  // namespace scratchbird::server_engine_bridge
