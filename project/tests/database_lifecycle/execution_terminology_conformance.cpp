// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "product_identity.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace server = scratchbird::server;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::string LowerAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool ContainsCaseInsensitive(std::string_view haystack,
                             std::string_view needle) {
  const auto lower_haystack = LowerAscii(std::string(haystack));
  const auto lower_needle = LowerAscii(std::string(needle));
  return lower_haystack.find(lower_needle) != std::string::npos;
}

void RequireNoLegacyStageTerms(std::string_view value,
                               std::string_view context) {
  const std::vector<std::string_view> forbidden = {
      "vnext",
      "beta 2",
      "beta2",
      "product-skeleton",
      "implementation_stage",
      "implementation-stage",
      "stage-label",
      "work-stage",
      "future-stage",
  };
  for (const auto term : forbidden) {
    if (ContainsCaseInsensitive(value, term)) {
      Fail(std::string("legacy execution terminology leaked from ") +
           std::string(context) + ": " + std::string(term));
    }
  }
}

void TestServerProductIdentityTerminology() {
  const auto& identity = server::GetServerProductIdentity();
  Require(identity.product_name == "SBsrv",
          "server product identity name changed unexpectedly");
  Require(!identity.product_version.empty(),
          "server product identity version is empty");
  Require(identity.protocol_family == "SBPS",
          "server product identity lost protocol family");
  Require(!identity.release_channel.empty(),
          "server product identity release channel is empty");

  RequireNoLegacyStageTerms(identity.product_name, "product_name");
  RequireNoLegacyStageTerms(identity.product_kind, "product_kind");
  RequireNoLegacyStageTerms(identity.product_version, "product_version");
  RequireNoLegacyStageTerms(identity.protocol_family, "protocol_family");
  RequireNoLegacyStageTerms(identity.release_channel, "release_channel");

  const auto version_line = server::ProductVersionLine();
  Require(version_line.find("release_channel=") != std::string::npos,
          "server version line does not publish neutral release channel");
  Require(version_line.find(identity.protocol_family) != std::string::npos,
          "server version line omits protocol family");
  RequireNoLegacyStageTerms(version_line, "ProductVersionLine");
}

}  // namespace

int main() {
  TestServerProductIdentityTerminology();
  std::cout << "execution_terminology_conformance=passed\n";
  return EXIT_SUCCESS;
}
