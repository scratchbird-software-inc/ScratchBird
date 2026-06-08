// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_PARSER_PACKAGE_LIFECYCLE

#pragma once

#include "config.hpp"
#include "diagnostics.hpp"
#include "sbps.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::server {

struct ParserPackageRegistryEntry {
  std::string parser_package_uuid = "builtin_test_package";
  std::string parser_family_uuid = "builtin_test_family";
  std::string dialect_profile_uuid = "builtin_test_dialect";
  std::string required_capability_id = "parser.sbsql.builtin";
  std::string capability_provider_id = "sb_server";
  std::string capability_edition_scope = "community";
  std::string resource_bundle_hash = "builtin";
  std::string state = "enabled";
  std::uint64_t capability_epoch = 1;
  std::uint64_t capability_policy_generation = 1;
  std::uint32_t parser_api_major = 3;
  std::uint32_t parser_api_minor = 0;
  std::uint16_t sbps_min_major = sbps::kProtocolMajor;
  std::uint16_t sbps_min_minor = 0;
  std::uint16_t sbps_max_major = sbps::kProtocolMajor;
  std::uint16_t sbps_max_minor = sbps::kProtocolMinor;
  bool match_builtin_test_profile = true;
  bool hash_required = true;
  bool signature_required = false;
  bool signature_present = false;
  bool dev_hash_bypass = false;
  bool parser_support_udr_required = false;
  bool parser_support_udr_available = true;
  std::string parser_support_udr_uuid;
  std::string parser_support_udr_abi = "sb_udr_v1";
  std::string parser_support_udr_source_revision;
  std::string parser_support_udr_binary_hash;
  std::string parser_support_udr_signature_policy;
  std::string parser_support_udr_capability_role;
  bool capability_installed = true;
  bool capability_enabled = true;
  bool cluster_capability_required = false;
  std::uint64_t failure_count_10m = 0;
  std::uint64_t failure_count_1h = 0;
};

struct ParserPackageRegistry {
  std::uint64_t generation = 1;
  std::uint64_t capability_policy_generation = 1;
  std::string source = "default_builtin";
  std::vector<ParserPackageRegistryEntry> entries;
  std::vector<ServerDiagnostic> diagnostics;
};

struct ParserPackageAdmissionResult {
  bool admitted = false;
  bool dev_warning = false;
  std::uint64_t registry_generation = 1;
  ParserPackageRegistryEntry entry;
  std::vector<ServerDiagnostic> diagnostics;
};

struct ParserChildLaunchPolicy {
  std::vector<std::string> env_whitelist;
  std::vector<std::string> inherited_handles;
  std::uint64_t memory_bytes = 256ull * 1024ull * 1024ull;
  std::uint32_t cpu_percent = 100;
  std::uint32_t open_handle_limit = 128;
  std::uint32_t process_count = 1;
  std::uint64_t ipc_queued_bytes = 16ull * 1024ull * 1024ull;
  std::uint64_t restart_window_ms = 600000;
  std::uint64_t quarantine_failures_10m = 5;
  std::uint64_t quarantine_failures_1h = 10;
};

ParserPackageRegistry LoadParserPackageRegistry(const ServerBootstrapConfig& config);
ParserPackageAdmissionResult AdmitParserPackage(const ParserPackageRegistry& registry,
                                                const sbps::HelloRequest& hello,
                                                std::uint16_t protocol_major,
                                                std::uint16_t protocol_minor);
ParserChildLaunchPolicy DefaultParserChildLaunchPolicy();
std::uint64_t ParserChildRestartDelayMs(const std::string& package_uuid,
                                        std::uint64_t attempt_index);
bool ParserPackageShouldQuarantine(const ParserPackageRegistryEntry& entry);
std::string ParserPackageRegistryStatusJson(const ParserPackageRegistry& registry);

}  // namespace scratchbird::server
