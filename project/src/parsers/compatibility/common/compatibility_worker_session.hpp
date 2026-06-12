// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <string_view>

#include "compatibility_dialect.hpp"

namespace scratchbird::parser::compatibility {

std::string HandleWorkerCommand(std::string_view line,
                                const DialectProfile& profile,
                                bool* close);
int ServeTextWorkerSession(int fd, const DialectProfile& profile);

} // namespace scratchbird::parser::compatibility
