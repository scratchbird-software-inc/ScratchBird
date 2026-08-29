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
#include <string>

namespace scratchbird::parser::sbsql {
std::uint64_t AuthoritativeMultiSourceProjectionProofMaskForTest();
std::string AuthoritativeMultiSourceProjectionProofDetailForTest();
}

int main() {
  constexpr std::uint64_t kExpectedProofMask = 0x7fff;
  const auto actual = scratchbird::parser::sbsql::
      AuthoritativeMultiSourceProjectionProofMaskForTest();
  if (actual != kExpectedProofMask) {
    std::cerr << "sbsql_authoritative_multi_source_projection_conformance="
              << "failed:actual_mask=" << actual
              << ":expected_mask=" << kExpectedProofMask << ":detail="
              << scratchbird::parser::sbsql::
                     AuthoritativeMultiSourceProjectionProofDetailForTest()
              << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "sbsql_authoritative_multi_source_projection_conformance="
            << "passed\n";
  return EXIT_SUCCESS;
}
