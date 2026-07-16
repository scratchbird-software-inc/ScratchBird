// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "bootstrap_password_verifier.hpp"

#include <cstdlib>
#include <iostream>
#include <regex>
#include <string>

int main() {
  const std::string candidate = "A-different-QA-secret-42";
  std::string first;
  std::string second;
  if (!scratchbird::cli::DeriveBootstrapPasswordVerifier(candidate, &first) ||
      !scratchbird::cli::DeriveBootstrapPasswordVerifier(candidate, &second)) {
    std::cerr << "PBKDF2 verifier derivation failed\n";
    return EXIT_FAILURE;
  }
  const std::regex canonical(
      R"(^local-password-pbkdf2-sha256:v1:iterations=600000:salt=[0-9a-f]{32}:verifier=[0-9a-f]{64}$)");
  if (!std::regex_match(first, canonical) ||
      !std::regex_match(second, canonical)) {
    std::cerr << "PBKDF2 verifier envelope is not canonical\n";
    return EXIT_FAILURE;
  }
  if (first == second) {
    std::cerr << "same password reused a salt/verifier envelope\n";
    return EXIT_FAILURE;
  }
  std::string with_nul("valid-prefix-password", 12);
  with_nul.push_back('\0');
  with_nul += "suffix";
  std::string refused;
  if (scratchbird::cli::DeriveBootstrapPasswordVerifier(with_nul, &refused)) {
    std::cerr << "password containing NUL was accepted\n";
    return EXIT_FAILURE;
  }
  if (scratchbird::cli::BootstrapPasswordSecretValid(
          std::string(1025, 'x'))) {
    std::cerr << "oversized bootstrap password was accepted\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
