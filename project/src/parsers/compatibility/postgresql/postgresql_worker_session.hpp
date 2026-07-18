// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "compatibility_dialect.hpp"

#include <string_view>

namespace scratchbird::parser::postgresql {

using PostgresqlWireParseFn =
    scratchbird::parser::compatibility::ParseResult (*)(std::string_view sql_text);

int ServePostgresqlWireWorkerSession(int fd, PostgresqlWireParseFn parse_statement);
int ServePostgresqlWorkerSession(int fd);

} // namespace scratchbird::parser::postgresql
