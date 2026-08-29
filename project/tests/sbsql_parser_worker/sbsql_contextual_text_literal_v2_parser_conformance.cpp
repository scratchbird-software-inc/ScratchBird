// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace scratchbird::parser::sbsql {
std::uint64_t ContextualTextLiteralV2ParserProofMaskForTest();
}

int main() {
  constexpr std::uint64_t kExpectedMask = 0x7f;
  const auto actual = scratchbird::parser::sbsql::
      ContextualTextLiteralV2ParserProofMaskForTest();
  if (actual != kExpectedMask) {
    std::cerr << "contextual TEXT literal V2 parser proof mask=" << actual
              << " expected=" << kExpectedMask << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
