// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace scratchbird::cli::conformance {

// Applies optional manifest expectation fields to a normalized conformance result.
// Returns true when all expectations pass. On failure, this mutates `result` to:
//   - status = "error"
//   - append human-readable expectation failures to `errors`
bool applyManifestExpectations(const nlohmann::json& test,
                               nlohmann::json& result,
                               std::string* summary = nullptr);

}  // namespace scratchbird::cli::conformance
