// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "sblr_engine_envelope.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::sblr {

struct SblrToSbsqlDiagnostic {
  std::string code;
  std::string message;
  bool error = true;
};

struct SblrToSbsqlOptions {
  bool source_preserving = false;
};

struct SblrToSbsqlResult {
  bool ok = false;
  std::string sbsql_text;
  std::vector<SblrToSbsqlDiagnostic> diagnostics;
};

SblrToSbsqlResult RenderSblrEnvelopeToSbsql(const SblrOperationEnvelope& envelope,
                                            const SblrToSbsqlOptions& options);

}  // namespace scratchbird::engine::sblr
