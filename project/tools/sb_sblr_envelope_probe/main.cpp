// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_engine_envelope.hpp"

#include <iostream>

int main() {
  namespace sblr = scratchbird::engine::sblr;
  auto envelope = sblr::MakeSblrEnvelope("observability.show_version", "show.version", "TRACE-SBLR-ENVELOPE");
  auto validation = sblr::ValidateSblrEnvelope(envelope);
  auto encoded = sblr::EncodeSblrEnvelope(envelope);
  auto decoded = sblr::DecodeSblrEnvelope(encoded);
  envelope.contains_sql_text = true;
  auto rejected = sblr::ValidateSblrEnvelope(envelope);
  const bool ok = validation.ok && decoded.ok && !rejected.ok;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"valid_diagnostics\":" << validation.diagnostics.size()
            << ",\"reject_diagnostics\":" << rejected.diagnostics.size() << "}\n";
  return ok ? 0 : 1;
}
