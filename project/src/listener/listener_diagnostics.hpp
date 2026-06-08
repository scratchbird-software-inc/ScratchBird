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
#include <vector>

#include "manager_protocol.hpp"

namespace scratchbird::listener {

namespace proto = scratchbird::manager::protocol;

proto::Diagnostic MakeDiagnostic(std::string code,
                                 std::string severity,
                                 std::string message,
                                 std::string component = "sb_listener",
                                 std::vector<proto::Field> fields = {});

proto::MessageVectorSet MakeMessageVectorSet(std::vector<proto::Diagnostic> diagnostics,
                                             std::string language = "en",
                                             std::string dialect = "sbsql.v3");

std::string MessageVectorSetJson(const proto::MessageVectorSet& set);

std::string QuoteJson(std::string_view value);

} // namespace scratchbird::listener
