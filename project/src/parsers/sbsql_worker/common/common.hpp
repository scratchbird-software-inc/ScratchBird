// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ipc/sbsql_ipc_common.hpp"
#include "parser_client_types.hpp"

namespace scratchbird::parser::sbsql {

inline constexpr std::uint32_t kSbsqlWorkerParserApiCurrentMajor = 1;
inline constexpr std::uint32_t kSbsqlWorkerProtocolCurrentVersion = 1;
inline constexpr std::uint32_t kSbsqlWorkerProtocolMinSupported = 1;
inline constexpr std::uint32_t kSbsqlWorkerProtocolMaxSupported = 1;
inline constexpr std::uint32_t kSbsqlWorkerRegistryCurrentVersion = 1;

enum class ParserState {
  kSpawned,
  kPackageAdmitted,
  kInitializing,
  kWireReady,
  kIdlePreAuth,
  kClientConnected,
  kAuthenticating,
  kAuthenticated,
  kActive,
  kDraining,
  kRecycled,
  kDisconnected,
  kTerminating,
  kFailed,
  kQuarantined,
};

struct ParserResourceBudget {
  std::uint64_t max_statement_bytes{1024 * 1024};
  std::uint64_t max_identifier_bytes{256};
  std::uint64_t max_token_count{131072};
  std::uint64_t max_literal_bytes{1024 * 1024};
  std::uint64_t max_ast_depth{256};
  std::uint64_t max_parameter_count{65535};
  std::uint64_t max_sblr_envelope_bytes{16 * 1024 * 1024};
  std::uint64_t max_diagnostic_payload_bytes{64 * 1024};
  std::uint64_t max_message_vector_count{1024};
  std::uint64_t max_result_metadata_columns{4096};
  std::uint64_t max_render_output_bytes{1024 * 1024};
  std::uint64_t max_parser_cache_entries{10000};
};

struct ParserConfig : ipc::ParserClientConfig {
  bool listener_worker{false};
  bool probe_mode{false};
  bool allow_probe_auth{false};
  std::intptr_t listener_control_fd{-1};
  std::string listener_control_socket;
  std::uint64_t worker_numeric_id{1};
  std::string parser_uuid;
  std::string listener_uuid;
  bool embedded_engine_direct{false};
  // Programmatic fixture-only escape hatch. Production clients never set it.
  // It permits opening an already-created minimal test database without a
  // page-backed bootstrap principal; it never permits database creation.
  bool allow_uncredentialed_fixture_database{false};
  // Fixture-only no-credential attach, additionally gated by
  // allow_uncredentialed_fixture_database.
  bool embedded_auth_bypass_sysarch{false};
  bool embedded_database_ownership_prelocked{false};
  std::string embedded_database_path;
  std::string bundle_contract_id{"sbp_sbsql@1"};
  std::string build_id{"dev"};
  bool tls_required{false};
  std::string tls_cert_file;
  std::string tls_key_file;
  std::string tls_ca_file;
  std::uint32_t parser_api_major{kSbsqlWorkerParserApiCurrentMajor};
  std::uint32_t protocol_version{kSbsqlWorkerProtocolCurrentVersion};
  std::uint32_t metrics_schema_version{1};
  ParserResourceBudget resource_budget;

  ParserConfig() {
    dialect = "sbsql";
    profile_id = "default";
    default_language_profile = "sbsql.builtin.recovery.en";
    input_syntax_profile = "sbsql.syntax.standard";
    common_resource_hash = "builtin.common.sbsql.v1";
    resource_compatibility_identity = "sbsql.resource.compat.v1";
    resource_version_identity = "sbsql.resource-pack.v1";
    dialect_profile_uuid = "sbsql_v3";
    registry_version = kSbsqlWorkerRegistryCurrentVersion;
  }
};

struct SessionContext : ipc::ParserSessionContext {
  SessionContext() {
    language_profile = "sbsql.builtin.recovery.en";
    input_syntax_profile = "sbsql.syntax.standard";
    common_resource_hash = "builtin.common.sbsql.v1";
    resource_compatibility_identity = "sbsql.resource.compat.v1";
    resource_version_identity = "sbsql.resource-pack.v1";
  }
};

struct PipelineResult {
  bool accepted{false};
  bool frontdoor_cache_hit{false};
  bool parser_executes_sql{false};
  bool cached_storage_authority{false};
  bool cached_authorization_authority{false};
  bool cached_finality_authority{false};
  std::string statement_family;
  std::string operation_family;
  std::uint64_t statement_hash{0};
  std::string sblr_payload;
  std::string server_operation_id;
  std::string server_cursor_uuid;
  std::uint64_t server_row_count{0};
  std::uint64_t server_affected_rows{0};
  bool server_affected_rows_present{false};
  std::string server_result_payload;
  MessageVectorSet messages;
};

std::string StateName(ParserState state);
std::string TrimAscii(std::string_view text);
std::uint64_t Fnv1a64(std::string_view text);

} // namespace scratchbird::parser::sbsql
