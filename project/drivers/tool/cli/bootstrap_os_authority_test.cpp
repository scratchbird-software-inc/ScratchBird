// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "bootstrap_os_authority.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

std::string Platform() {
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "posix";
#endif
}

std::string ServiceIdentity() {
#ifdef _WIN32
  return "NT SERVICE\\scratchbird";
#else
  return "scratchbird";
#endif
}

std::string ServiceGroup() {
#ifdef _WIN32
  return "ScratchBird";
#else
  return "scratchbird";
#endif
}

bool Expect(bool condition, const char* message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

void Write(const std::filesystem::path& path, const std::string& body) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << body;
}

int VerifyInstalledServiceIdentity(const std::string& profile_path) {
#ifdef _WIN32
  (void)profile_path;
  std::cerr << "installed service identity probe is POSIX-only\n";
  return EXIT_FAILURE;
#else
  if (geteuid() != 0) {
    std::cerr << "installed service identity probe requires root\n";
    return EXIT_FAILURE;
  }
  const auto loaded =
      scratchbird::cli::LoadBootstrapPlatformProfile(profile_path);
  if (!loaded.ok) {
    std::cerr << "installed service identity profile was rejected\n";
    return EXIT_FAILURE;
  }
  const auto assumed =
      scratchbird::cli::VerifyAndAssumeBootstrapServiceIdentity(loaded.profile);
  if (!assumed.ok || !assumed.service_identity_assumed ||
      !assumed.ownership_handoff_ready || geteuid() == 0 || getegid() == 0 ||
      assumed.service_identity != "scratchbird" ||
      assumed.service_group != "scratchbird") {
    std::cerr << scratchbird::cli::kBootstrapDeniedDiagnostic << '\n';
    return EXIT_FAILURE;
  }
  if (setgid(0) == 0 || setuid(0) == 0 || geteuid() == 0 || getegid() == 0) {
    std::cerr << "service identity regained OS bootstrap authority\n";
    return EXIT_FAILURE;
  }
  std::cout << "installed_service_identity_assumption=passed\n";
  std::cout << "effective_user=scratchbird\n";
  std::cout << "effective_group=scratchbird\n";
  std::cout << "bootstrap_authority_regain=refused\n";
  return EXIT_SUCCESS;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 &&
      std::string(argv[1]) == "--verify-installed-service-identity") {
    return VerifyInstalledServiceIdentity(argv[2]);
  }
  if (argc != 1) {
    std::cerr << "unsupported bootstrap authority test arguments\n";
    return EXIT_FAILURE;
  }
  const auto root = std::filesystem::temp_directory_path() /
                    "scratchbird-bootstrap-os-authority-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  const auto profile = root / "SBbootstrap.profile";

  Write(profile,
        "schema_id = scratchbird.bootstrap_platform_profile.v1\n"
        "platform = " + Platform() + "\n"
        "service_identity = " + ServiceIdentity() + "\n"
        "service_group = " + ServiceGroup() + "\n");
  const auto loaded = scratchbird::cli::LoadBootstrapPlatformProfile(
      profile.string());
  bool ok = Expect(loaded.ok, "valid platform profile was rejected") &&
            Expect(loaded.profile.service_identity == ServiceIdentity(),
                   "service identity was not loaded") &&
            Expect(loaded.profile.service_group == ServiceGroup(),
                   "service group was not loaded");

  scratchbird::cli::BootstrapPlatformProfile windows_profile;
  windows_profile.schema_id =
      scratchbird::cli::kBootstrapPlatformProfileSchema;
  windows_profile.platform = "windows";
  windows_profile.service_identity = "NT SERVICE\\scratchbird";
  windows_profile.service_group = "ScratchBird";
  ok = Expect(
           scratchbird::cli::BootstrapPlatformProfileContractValidForPlatform(
               windows_profile, "windows"),
           "exact Windows managed service identity contract was rejected") &&
       ok;
  for (const char* rejected_identity : {
           "scratchbird",
           ".\\scratchbird",
           "MACHINE\\scratchbird",
           "DOMAIN\\scratchbird",
           "NT SERVICE\\other",
           "NT AUTHORITY\\SYSTEM",
           "LocalSystem",
           "Administrator",
       }) {
    auto rejected = windows_profile;
    rejected.service_identity = rejected_identity;
    ok = Expect(
             !scratchbird::cli::BootstrapPlatformProfileContractValidForPlatform(
                 rejected, "windows"),
             "non-canonical Windows service identity was accepted") &&
         ok;
  }
  auto wrong_windows_group = windows_profile;
  wrong_windows_group.service_group = "Administrators";
  ok = Expect(
           !scratchbird::cli::BootstrapPlatformProfileContractValidForPlatform(
               wrong_windows_group, "windows"),
           "non-canonical Windows filesystem group was accepted") &&
       ok;

  scratchbird::cli::BootstrapPlatformProfile posix_profile;
  posix_profile.schema_id = scratchbird::cli::kBootstrapPlatformProfileSchema;
  posix_profile.platform = "linux";
  posix_profile.service_identity = "scratchbird";
  posix_profile.service_group = "scratchbird";
  ok = Expect(
           scratchbird::cli::BootstrapPlatformProfileContractValidForPlatform(
               posix_profile, "linux"),
           "exact Linux scratchbird identity contract was rejected") &&
       ok;
  auto root_profile = posix_profile;
  root_profile.service_identity = "root";
  ok = Expect(
           !scratchbird::cli::BootstrapPlatformProfileContractValidForPlatform(
               root_profile, "linux"),
           "POSIX root identity analogue was accepted") &&
       ok;

  constexpr std::uint64_t kScratchBirdGroupId = 997;
  ok = Expect(
           scratchbird::cli::BootstrapServiceIdentityGroupSetIsLeastAuthority(
               {kScratchBirdGroupId}, kScratchBirdGroupId, {}),
           "exact Linux service group set was rejected") &&
       ok;
  ok = Expect(
           !scratchbird::cli::BootstrapServiceIdentityGroupSetIsLeastAuthority(
               {}, kScratchBirdGroupId, {}),
           "empty service group set was accepted") &&
       ok;
  ok = Expect(
           !scratchbird::cli::BootstrapServiceIdentityGroupSetIsLeastAuthority(
               {kScratchBirdGroupId, 27}, kScratchBirdGroupId, {}),
           "Linux supplementary authority group was accepted") &&
       ok;
  const std::vector<std::uint64_t> macos_implicit_groups = {
      scratchbird::cli::kMacOsImplicitEveryoneGroupId,
      scratchbird::cli::kMacOsImplicitLocalAccountsGroupId,
  };
  ok = Expect(
           scratchbird::cli::BootstrapServiceIdentityGroupSetIsLeastAuthority(
               {kScratchBirdGroupId,
                scratchbird::cli::kMacOsImplicitEveryoneGroupId,
                scratchbird::cli::kMacOsImplicitLocalAccountsGroupId},
               kScratchBirdGroupId, macos_implicit_groups),
           "known macOS implicit baseline groups were rejected") &&
       ok;
  ok = Expect(
           !scratchbird::cli::BootstrapServiceIdentityGroupSetIsLeastAuthority(
               {kScratchBirdGroupId,
                scratchbird::cli::kMacOsImplicitEveryoneGroupId, 80},
               kScratchBirdGroupId, macos_implicit_groups),
           "unknown macOS authority group was accepted") &&
       ok;
  ok = Expect(
           !scratchbird::cli::BootstrapServiceIdentityGroupSetIsLeastAuthority(
               {scratchbird::cli::kMacOsImplicitEveryoneGroupId,
                scratchbird::cli::kMacOsImplicitLocalAccountsGroupId},
               kScratchBirdGroupId, macos_implicit_groups),
           "macOS group set without scratchbird was accepted") &&
       ok;
  ok = Expect(
           scratchbird::cli::
               BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
                   true, {false, false}),
           "complete Windows service SID group inventory was rejected") &&
       ok;
  ok = Expect(
           !scratchbird::cli::
               BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
                   true, {false, true}),
           "explicit Windows service SID group membership was accepted") &&
       ok;
  ok = Expect(
           !scratchbird::cli::
               BootstrapWindowsServiceSidGroupInventoryIsLeastAuthority(
                   false, {}),
           "incomplete Windows service SID group inventory was accepted") &&
       ok;

  Write(profile,
        "schema_id = scratchbird.bootstrap_platform_profile.v1\n"
        "platform = " + Platform() + "\n"
        "service_identity = operator_required\n"
        "service_group = " + ServiceGroup() + "\n");
  ok = Expect(!scratchbird::cli::LoadBootstrapPlatformProfile(profile.string()).ok,
              "unconfigured profile placeholder was accepted") && ok;

  Write(profile,
        "schema_id = scratchbird.bootstrap_platform_profile.v1\n"
        "platform = " + Platform() + "\n"
        "service_identity = " + ServiceIdentity() + "\n"
        "service_group = domain\\qa-group\n");
  ok = Expect(!scratchbird::cli::LoadBootstrapPlatformProfile(profile.string()).ok,
              "qualified/domain group name was accepted") && ok;

  Write(profile,
        "schema_id = scratchbird.bootstrap_platform_profile.v1\n"
        "platform = spoofed-platform\n"
        "service_identity = " + ServiceIdentity() + "\n"
        "service_group = " + ServiceGroup() + "\n");
  ok = Expect(!scratchbird::cli::LoadBootstrapPlatformProfile(profile.string()).ok,
              "mismatched platform profile was accepted") && ok;

  Write(profile,
        "schema_id = scratchbird.bootstrap_platform_profile.v1\n"
        "platform = " + Platform() + "\n"
        "service_identity = root\n"
        "service_group = " + ServiceGroup() + "\n");
  ok = Expect(!scratchbird::cli::LoadBootstrapPlatformProfile(profile.string()).ok,
              "root service identity was accepted") && ok;

  scratchbird::cli::BootstrapPlatformProfile malformed;
  malformed.schema_id = scratchbird::cli::kBootstrapPlatformProfileSchema;
  malformed.platform = Platform();
  malformed.service_identity = "operator_required";
  malformed.service_group = "operator_required";
  const auto denied =
      scratchbird::cli::VerifyAndAssumeBootstrapServiceIdentity(malformed);
  ok = Expect(!denied.ok, "malformed authority profile was admitted") && ok;
  ok = Expect(denied.safe_diagnostic_code ==
                  scratchbird::cli::kBootstrapDeniedDiagnostic,
              "bootstrap denial did not use the safe code-only diagnostic") && ok;

  std::filesystem::remove_all(root, ignored);
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
