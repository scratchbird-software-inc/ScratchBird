// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_NATIVE_CREATE_TEST_FIXTURE (owner Q256 = 1, 2026-11-16 legacy
// cut). The one native create shared by the test fixtures and seeders. It goes
// through the production native create (EngineCreateLifecycle, native format),
// so the fixture can never build a file the engine would not build. The test
// stands in for the hosting server: it supplies the server's policy pack root
// on the context (Q104 = 2), and the caller's security context. The engine
// mints every identity (database, filespace, principals); a caller-chosen
// database identity is not honoured (native create has no such input).
//
// Inputs native create needs and the legacy fixtures never gave: the database
// name (data), its language constant, total_pages (no default, Q105 = 1), and
// an internal service principal (its password is derived inside the engine
// from the database UUID and a seed the create stores, owner Q267 = 2 /
// Q274, 2026-11-19; was: a credential verifier supplied here, then the
// receipt's password). Legacy inputs with no native counterpart are not
// passed: resource seed pack, metric policy files.

#include "lifecycle/engine_lifecycle_api.hpp"
#include "catalog_language.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace scratchbird::tests {

inline std::string DefaultNativeFixturePolicyPackRoot() {
#ifdef SB_DEFAULT_POLICY_PACK_ROOT
  return SB_DEFAULT_POLICY_PACK_ROOT;
#else
  // tests/support/<this file> -> project root, as ConfigureCredentialedFixtureBootstrap.
  return (std::filesystem::path(__FILE__).lexically_normal().parent_path().parent_path().parent_path() /
          "resources/policy-packs/default-local-password")
      .string();
#endif
}

// A verifier the fixtures already use (engine_statement_fixture.hpp), for the
// bootstrap principal. (Was: also the service principal's default; Q267 = 2.)
inline constexpr std::string_view kNativeFixtureCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";

struct NativeFixtureCreateRequest {
  std::filesystem::path path;
  std::string database_name = "fixture_database";
  core::platform::Uuid language = core::catalog::kCatalogLanguage_en;
  std::uint32_t page_size = 16384;
  std::uint64_t total_pages = 0;  // 0: 2048 at 8K, otherwise 512 (the native tests' sizes)
  std::string bootstrap_principal_name = "fixture_owner";
  std::string bootstrap_credential_fingerprint = std::string(kNativeFixtureCredentialFingerprint);
  std::string service_principal_name = "fixture_service";
  std::string policy_seed_pack_root = DefaultNativeFixturePolicyPackRoot();
};

struct NativeFixtureCreateResult {
  bool ok = false;
  engine::internal_api::EngineCreateLifecycleResult api;
  core::platform::Uuid database_uuid{};
  core::platform::Uuid filespace_uuid{};
  core::platform::Uuid bootstrap_principal_uuid{};
  core::platform::Uuid service_principal_uuid{};
  core::platform::Uuid construction_transaction_uuid{};
  // (Q274, 2026-11-19: the receipt carries no service password; was a
  // 16-byte field here.)
  std::string diagnostic_code;
  std::string diagnostic_detail;
};

inline bool NativeFixtureReceiptUuid(const engine::internal_api::EngineApiResult& result,
                                     std::string_view name, core::platform::Uuid* out) {
  for (const auto& row : result.result_shape.rows)
    for (const auto& field : row.fields)
      if (field.first == name) {
        if (field.second.isSqlNull() || field.second.binary_value.size() != out->bytes.size()) return false;
        std::copy(field.second.binary_value.begin(), field.second.binary_value.end(), out->bytes.begin());
        return true;
      }
  return false;
}

inline NativeFixtureCreateResult CreateNativeFixtureDatabase(const NativeFixtureCreateRequest& in) {
  namespace api = engine::internal_api;
  api::EngineCreateLifecycleRequest request;
  request.context.request_id = "native-create-test-fixture";
  request.context.database_path = in.path.string();
  request.context.security_context_present = true;
  request.context.server_policy_seed_pack_root = in.policy_seed_pack_root;
  request.database_format = api::EngineDatabaseFormat::native;
  request.database_name = in.database_name;
  request.database_language = in.language;
  const auto total_pages = in.total_pages != 0 ? in.total_pages : (in.page_size == 8192 ? 2048u : 512u);
  request.option_envelopes.push_back("page_size:" + std::to_string(in.page_size));
  request.option_envelopes.push_back("total_pages:" + std::to_string(total_pages));
  request.option_envelopes.push_back("bootstrap_principal_name:" + in.bootstrap_principal_name);
  request.option_envelopes.push_back("bootstrap_credential_fingerprint:" + in.bootstrap_credential_fingerprint);
  request.option_envelopes.push_back("internal_service_principal_name:" + in.service_principal_name);
  NativeFixtureCreateResult out;
  out.api = api::EngineCreateLifecycle(request);
  if (!out.api.ok) {
    if (!out.api.diagnostics.empty()) {
      out.diagnostic_code = out.api.diagnostics.front().code;
      out.diagnostic_detail = out.api.diagnostics.front().detail;
    }
    return out;
  }
  out.ok = NativeFixtureReceiptUuid(out.api, "database_uuid", &out.database_uuid) &&
           NativeFixtureReceiptUuid(out.api, "filespace_uuid", &out.filespace_uuid) &&
           NativeFixtureReceiptUuid(out.api, "bootstrap_principal_uuid", &out.bootstrap_principal_uuid) &&
           NativeFixtureReceiptUuid(out.api, "service_principal_uuid", &out.service_principal_uuid) &&
           NativeFixtureReceiptUuid(out.api, "construction_transaction_uuid", &out.construction_transaction_uuid);
  if (!out.ok) out.diagnostic_code = "TEST.NATIVE_FIXTURE.RECEIPT_INCOMPLETE";
  return out;
}

}  // namespace scratchbird::tests
