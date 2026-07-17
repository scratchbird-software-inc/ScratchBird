// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "bootstrap_os_authority.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <grp.h>
#include <pwd.h>
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

int VerifyRunningServiceIdentity(const std::string& profile_path,
                                 const std::string& canary_root) {
#ifdef _WIN32
  (void)profile_path;
  (void)canary_root;
  std::cerr << "running service identity probe is POSIX-only\n";
  return EXIT_FAILURE;
#else
  if (geteuid() == 0) {
    std::cerr << "running service identity probe must not run as root\n";
    return EXIT_FAILURE;
  }
  const auto loaded =
      scratchbird::cli::LoadBootstrapPlatformProfile(profile_path);
  if (!loaded.ok || loaded.profile.service_identity != "scratchbird" ||
      loaded.profile.service_group != "scratchbird") {
    std::cerr << "running service identity profile was rejected\n";
    return EXIT_FAILURE;
  }
  const passwd* service_user_record = getpwnam("scratchbird");
  if (service_user_record == nullptr) {
    std::cerr << scratchbird::cli::kBootstrapDeniedDiagnostic << '\n';
    return EXIT_FAILURE;
  }
  const uid_t service_user_id = service_user_record->pw_uid;
  const group* service_group = getgrnam("scratchbird");
  if (service_group == nullptr || service_user_id == 0 ||
      service_group->gr_gid == 0 || getuid() != service_user_id ||
      geteuid() != service_user_id ||
      getgid() != service_group->gr_gid || getegid() != service_group->gr_gid ||
      !scratchbird::cli::BootstrapCurrentProcessHasOnlyConfiguredGroup(
          static_cast<std::uint64_t>(service_group->gr_gid))) {
    std::cerr << scratchbird::cli::kBootstrapDeniedDiagnostic << '\n';
    return EXIT_FAILURE;
  }
  if (setgid(0) == 0 || setuid(0) == 0 || geteuid() == 0 || getegid() == 0) {
    std::cerr << "running service identity regained OS bootstrap authority\n";
    return EXIT_FAILURE;
  }
  std::error_code canary_error;
  const std::filesystem::path canary_directory(canary_root);
  if (canary_root.empty() ||
      !std::filesystem::is_directory(canary_directory, canary_error) ||
      canary_error) {
    std::cerr << "host-computed authority canary directory was rejected\n";
    return EXIT_FAILURE;
  }
  std::size_t canary_count = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(canary_directory, canary_error)) {
    if (canary_error || !entry.is_regular_file(canary_error) || canary_error) {
      std::cerr << "host-computed authority canary inventory failed\n";
      return EXIT_FAILURE;
    }
    ++canary_count;
    std::ifstream canary(entry.path(), std::ios::binary);
    if (canary.is_open()) {
      std::cerr << "host-computed authority group remained effective\n";
      return EXIT_FAILURE;
    }
    std::ofstream canary_write(entry.path(), std::ios::binary | std::ios::app);
    if (canary_write.is_open()) {
      std::cerr << "host-computed authority group retained write access\n";
      return EXIT_FAILURE;
    }
  }
  if (canary_error) {
    std::cerr << "host-computed authority canary inventory failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "effective_user=scratchbird\n";
  std::cout << "effective_group=scratchbird\n";
  std::cout << "supplementary_group_policy=exact_scratchbird_only\n";
  std::cout << "host_computed_authority_canaries=refused\n";
  std::cout << "host_computed_authority_canary_count=" << canary_count << '\n';
  std::cout << "bootstrap_authority_regain=refused\n";
  std::cout << "launchd_service_process_credential=passed\n" << std::flush;
  return EXIT_SUCCESS;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 &&
      std::string(argv[1]) == "--verify-installed-service-identity") {
    return VerifyInstalledServiceIdentity(argv[2]);
  }
  if (argc == 4 &&
      std::string(argv[1]) == "--verify-running-service-identity") {
    return VerifyRunningServiceIdentity(argv[2], argv[3]);
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
               {kScratchBirdGroupId}, kScratchBirdGroupId),
           "exact Linux service group set was rejected") &&
       ok;
  ok = Expect(
           scratchbird::cli::BootstrapServiceIdentityGroupSetIsLeastAuthority(
               {kScratchBirdGroupId, kScratchBirdGroupId,
                kScratchBirdGroupId},
               kScratchBirdGroupId),
           "duplicate observations of the exact service group were rejected") &&
       ok;
  ok = Expect(
           !scratchbird::cli::BootstrapServiceIdentityGroupSetIsLeastAuthority(
               {}, kScratchBirdGroupId),
           "empty service group set was accepted") &&
       ok;
  ok = Expect(
           !scratchbird::cli::BootstrapServiceIdentityGroupSetIsLeastAuthority(
               {kScratchBirdGroupId, 27}, kScratchBirdGroupId),
           "Linux supplementary authority group was accepted") &&
       ok;
  ok = Expect(
           !scratchbird::cli::
               BootstrapServiceIdentityGroupSetIsLeastAuthority(
                   {kScratchBirdGroupId, 12, 61, 701, 100},
                   kScratchBirdGroupId),
           "host-computed directory groups were copied into process policy") &&
       ok;
  ok = Expect(
           !scratchbird::cli::BootstrapServiceIdentityGroupSetIsLeastAuthority(
               {12, 61}, kScratchBirdGroupId),
           "group set without scratchbird was accepted") &&
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
