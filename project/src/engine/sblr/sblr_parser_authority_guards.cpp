// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_parser_authority_guards.hpp"

#include <utility>

namespace scratchbird::engine::sblr {

SblrResult RefuseParserAuthorityAttempt(const SblrExecutionContext& context,
                                        std::string operation_id,
                                        std::string detail) {
  auto diagnostic = MakeSblrRefusalDiagnostic("SB_DIAG_PARSER_AUTHORITY_REFUSED", context, std::move(detail));
  diagnostic.fields.push_back({"operation_id", operation_id});
  return MakeSblrFailure(SblrStatusCode::security_refused, std::move(operation_id), std::move(diagnostic));
}

}  // namespace scratchbird::engine::sblr
