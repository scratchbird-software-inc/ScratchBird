// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_diagnostics.hpp"

namespace scratchbird::listener {

proto::Diagnostic MakeDiagnostic(std::string code,
                                 std::string severity,
                                 std::string message,
                                 std::string component,
                                 std::vector<proto::Field> fields) {
  fields.push_back({"component", std::move(component)});
  return proto::MakeDiagnostic(std::move(code), std::move(message), std::move(fields), std::move(severity));
}

proto::MessageVectorSet MakeMessageVectorSet(std::vector<proto::Diagnostic> diagnostics,
                                             std::string language,
                                             std::string dialect) {
  (void)language;
  (void)dialect;
  proto::MessageVectorSet set;
  set.request_uuid = proto::MakePseudoUuidV7();
  set.diagnostics = std::move(diagnostics);
  return set;
}

std::string MessageVectorSetJson(const proto::MessageVectorSet& set) {
  return proto::ToMessageVectorSetJson(set);
}

std::string QuoteJson(std::string_view value) {
  return proto::JsonEscape(value);
}

} // namespace scratchbird::listener
