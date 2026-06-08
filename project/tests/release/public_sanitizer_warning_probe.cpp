// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <cstdlib>
#include <iostream>
#include <string_view>

#ifndef SB_PUBLIC_RELEASE_SANITIZER_PROFILE_TEXT
#define SB_PUBLIC_RELEASE_SANITIZER_PROFILE_TEXT "none"
#endif

namespace {

bool HasArg(int argc, char** argv, std::string_view name) {
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == name) {
      return true;
    }
  }
  return false;
}

std::string_view ValueAfter(int argc, char** argv, std::string_view name) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string_view(argv[i]) == name) {
      return argv[i + 1];
    }
  }
  return {};
}

int Fail(std::string_view message) {
  std::cerr << "public_sanitizer_warning_probe=fail:" << message << '\n';
  return EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string_view expected_profile =
      ValueAfter(argc, argv, "--expect-sanitizer-profile");
  if (expected_profile.empty()) {
    return Fail("missing_expected_sanitizer_profile");
  }

  const std::string_view configured_profile =
      SB_PUBLIC_RELEASE_SANITIZER_PROFILE_TEXT;
  if (configured_profile != expected_profile) {
    std::cerr << "expected=" << expected_profile
              << " configured=" << configured_profile << '\n';
    return Fail("sanitizer_profile_mismatch");
  }

  const bool expect_warnings_as_errors =
      HasArg(argc, argv, "--expect-warnings-as-errors");
#if defined(SB_PUBLIC_RELEASE_WARNINGS_AS_ERRORS_ACTIVE)
  const bool warnings_as_errors_active = true;
#else
  const bool warnings_as_errors_active = false;
#endif
  if (warnings_as_errors_active != expect_warnings_as_errors) {
    return Fail("warnings_as_errors_flag_mismatch");
  }

  if (configured_profile == "asan-ubsan") {
#if !defined(SB_PUBLIC_RELEASE_ASAN_UBSAN_PROFILE)
    return Fail("asan_ubsan_profile_definition_missing");
#endif
  } else if (configured_profile == "tsan") {
#if !defined(SB_PUBLIC_RELEASE_TSAN_PROFILE)
    return Fail("tsan_profile_definition_missing");
#endif
  } else if (configured_profile != "none") {
    return Fail("unknown_configured_profile");
  }

  std::cout << "public_sanitizer_warning_probe=passed"
            << " sanitizer_profile=" << configured_profile
            << " warnings_as_errors="
            << (warnings_as_errors_active ? "on" : "off") << '\n';
  return EXIT_SUCCESS;
}
