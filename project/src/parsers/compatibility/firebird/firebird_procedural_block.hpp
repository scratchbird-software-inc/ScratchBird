// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace scratchbird::parser::firebird {

// Firebird resolves an unqualified COLLATE on an anonymous-block input or
// output descriptor against the attachment character set.  The database
// default remains explicit in this context so focused tests can prove that it
// is not accidentally used as fallback authority.
struct FirebirdExecuteBlockBindingContext {
  std::string database_default_charset;
  std::string attachment_charset;
};

enum class FirebirdBoundedExecuteBlockKind {
  kUnsupported,
  kEmptyResult,
  kTimestampSubstringAssignment,
};

enum class FirebirdProceduralSlotType {
  kCharacter,
  kInt32,
};

// This is a same-family bound semantic route, not a stored SQL or AST
// representation.  V1 deliberately covers the first three zero-yield
// Firebird QA shapes only.
struct FirebirdBoundedExecuteBlockRoute {
  FirebirdBoundedExecuteBlockKind kind{
      FirebirdBoundedExecuteBlockKind::kUnsupported};
  std::string slot_name;
  FirebirdProceduralSlotType slot_type{FirebirdProceduralSlotType::kCharacter};
  std::uint32_t character_length{0};
  bool nullable{true};
  std::string bound_character_set;
  std::string bound_collation;
  bool collation_bound_from_attachment{false};

  [[nodiscard]] bool recognized() const {
    return kind != FirebirdBoundedExecuteBlockKind::kUnsupported;
  }
  [[nodiscard]] bool has_output() const {
    return kind == FirebirdBoundedExecuteBlockKind::kEmptyResult;
  }
  [[nodiscard]] bool has_local() const {
    return kind ==
           FirebirdBoundedExecuteBlockKind::kTimestampSubstringAssignment;
  }
};

FirebirdBoundedExecuteBlockRoute ParseFirebirdBoundedExecuteBlockRoute(
    std::string_view firebird_sql,
    const FirebirdExecuteBlockBindingContext& binding_context);

// Encodes canonical neutral SBLR fields only.  Firebird names, charset and
// collation presentation, and source SQL are intentionally absent.
std::string EncodeFirebirdBoundedExecuteBlockEnvelope(
    const FirebirdBoundedExecuteBlockRoute& route);

std::string_view FirebirdBoundedExecuteBlockRouteName(
    FirebirdBoundedExecuteBlockKind kind);

}  // namespace scratchbird::parser::firebird
