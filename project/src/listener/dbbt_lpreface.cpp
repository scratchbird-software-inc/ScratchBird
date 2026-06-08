// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dbbt_lpreface.hpp"

#include <cstdlib>
#include <string_view>

namespace scratchbird::listener {
namespace {
std::vector<std::uint8_t> BytesFromString(std::string_view value) {
  return std::vector<std::uint8_t>(value.begin(), value.end());
}
}

DbbtGateResult LoadDbbtKeyMaterial(const ListenerConfig& config, DbbtKeyMaterial* out) {
  std::vector<proto::Diagnostic> diagnostics;
  if (config.dbbt_key_source == DbbtKeySource::kKeyring) {
    const char* key = std::getenv("SCRATCHBIRD_LISTENER_DBBT_KEY_HEX");
    if (key == nullptr || *key == '\0') {
      diagnostics.push_back(MakeDiagnostic("LISTENER.DBBT.KEYRING_KEY_MISSING", "ERROR",
                                           "keyring DBBT source requires SCRATCHBIRD_LISTENER_DBBT_KEY_HEX until the platform keyring bridge is attached",
                                           "sb_listener.dbbt"));
      return {false, MakeMessageVectorSet(std::move(diagnostics), config.language, config.dialect)};
    }
    out->bytes = proto::FromHex(key);
    out->source_name = "keyring";
    return {!out->bytes.empty(), MakeMessageVectorSet({}, config.language, config.dialect)};
  }
  if (config.dbbt_key_source == DbbtKeySource::kDevEnvironment) {
    if (!config.allow_dev_dbbt_env) {
      diagnostics.push_back(MakeDiagnostic("LISTENER.DBBT.DEV_KEY_DISABLED", "ERROR", "dev DBBT key source is disabled by policy", "sb_listener.dbbt"));
      return {false, MakeMessageVectorSet(std::move(diagnostics), config.language, config.dialect)};
    }
    const char* key = std::getenv("SCRATCHBIRD_DEV_DBBT_KEY");
    if (key == nullptr || *key == '\0') {
      diagnostics.push_back(MakeDiagnostic("LISTENER.DBBT.DEV_KEY_MISSING", "ERROR", "SCRATCHBIRD_DEV_DBBT_KEY is required for dev_environment DBBT source", "sb_listener.dbbt"));
      return {false, MakeMessageVectorSet(std::move(diagnostics), config.language, config.dialect)};
    }
    out->bytes = BytesFromString(key);
    out->source_name = "dev_environment";
    return {true, MakeMessageVectorSet({}, config.language, config.dialect)};
  }
  if (!config.allow_test_dbbt_builtin) {
    diagnostics.push_back(MakeDiagnostic("LISTENER.DBBT.TEST_KEY_DISABLED", "ERROR", "test builtin DBBT key source is disabled by policy", "sb_listener.dbbt"));
    return {false, MakeMessageVectorSet(std::move(diagnostics), config.language, config.dialect)};
  }
  out->bytes = BytesFromString("scratchbird-listener-test-dbbt-key-v1");
  out->source_name = "test_builtin";
  return {true, MakeMessageVectorSet({}, config.language, config.dialect)};
}

DbbtGateResult ValidateDbbtHexToken(const ListenerConfig& config,
                                    const std::string& token_hex,
                                    proto::DbbtReplayCache* replay_cache) {
  DbbtKeyMaterial key;
  auto key_result = LoadDbbtKeyMaterial(config, &key);
  if (!key_result.ok) {
    return key_result;
  }
  proto::DbbtValidationOptions options;
  options.expected_listener_id = 1;
  options.now_ms = proto::CurrentEpochMilliseconds();
  proto::DbbtToken token;
  auto validation = proto::ValidateDbbt(proto::FromHex(token_hex), key.bytes, options, &token);
  if (validation.ok && replay_cache != nullptr) {
    const auto token_id = proto::DbbtTokenId(token, key.bytes);
    if (!replay_cache->CheckAndInsert(token_id, token.expires_at_ms, options.now_ms)) {
      return {false, MakeMessageVectorSet({MakeDiagnostic("MCP.DBBT_REPLAY_DETECTED",
                                                           "ERROR",
                                                           "DBBT replay was detected",
                                                           "sb_listener.dbbt")},
                                          config.language,
                                          config.dialect)};
    }
  }
  return {validation.ok, MakeMessageVectorSet(std::move(validation.diagnostics), config.language, config.dialect)};
}

std::string MakeListenerDebugTagForParser(const ListenerConfig& config, const std::string& parser_worker_id) {
  return "listener=" + config.listener_uuid + ";profile=" + config.listener_profile + ";parser=" + parser_worker_id +
         ";database_selector=" + config.database_selector + ";server_endpoint=" + config.server_endpoint;
}

} // namespace scratchbird::listener
