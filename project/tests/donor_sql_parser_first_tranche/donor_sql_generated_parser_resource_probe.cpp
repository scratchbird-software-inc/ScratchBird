// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/parser/firebird/firebird_parser.h"
#include "scratchbird/parser/mysql/mysql_parser.h"
#include "scratchbird/parser/postgresql/pg_parser.h"

#include <cstdlib>
#include <type_traits>

int main() {
  static_assert(std::is_class_v<scratchbird::parser::firebird::Parser>);
  static_assert(std::is_class_v<scratchbird::parser::firebird::ParseResult>);
  static_assert(std::is_class_v<scratchbird::parser::mysql::Parser>);
  static_assert(std::is_class_v<scratchbird::parser::mysql::ParseResult>);
  static_assert(std::is_class_v<scratchbird::parser::postgresql::Parser>);
  static_assert(std::is_class_v<scratchbird::parser::postgresql::ParseResult>);
  static_assert(std::is_enum_v<scratchbird::parser::mysql::MySQLDataType::Kind>);
  static_assert(std::is_enum_v<scratchbird::parser::postgresql::PgDataType::Kind>);
  return EXIT_SUCCESS;
}
