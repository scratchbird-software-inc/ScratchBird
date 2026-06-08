// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrParserProjectionRow {
  std::string operation_id;
  std::string availability;
  std::string refusal_diagnostic;
  std::string notes;
};

std::vector<SblrParserProjectionRow> BuildSblrParserProjectionRows();

}  // namespace scratchbird::engine::sblr
