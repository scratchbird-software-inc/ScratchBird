// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "durable_authorization_fixture.hpp"
#include "published_mga_table_fixture.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
namespace scratchbird::tests {
namespace api = engine::internal_api;
class FixtureEngineSession {
 public:
  explicit FixtureEngineSession(const api::EngineRequestContext& context) {
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open);
    open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.database_path_utf8 = context.database_path.data();
    open.database_path_size = context.database_path.size();
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    Check(sb_engine_open(&open, &engine_, nullptr), nullptr, "engine open");
    sb_engine_session_params_v1_t begin{};
    begin.struct_size = sizeof(begin);
    begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    std::copy(context.principal_uuid.bytes.begin(), context.principal_uuid.bytes.end(),
              begin.effective_user_uuid.bytes);
    std::copy(context.session_uuid.bytes.begin(), context.session_uuid.bytes.end(),
              begin.session_uuid.bytes);
    begin.default_language_utf8 = "en";
    begin.default_language_size = 2;
    begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    Check(sb_engine_session_begin(engine_, &begin, &session_, nullptr), nullptr,
          "session begin");
  }
  ~FixtureEngineSession() {
    sb_engine_session_end_params_v1_t end{};
    end.struct_size = sizeof(end);
    end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    end.rollback_active_transactions = 1;
    end.cancel_open_results = 1;
    (void)sb_engine_session_end(session_, &end, nullptr);
    (void)sb_engine_close(engine_, nullptr);
  }
  FixtureEngineSession(const FixtureEngineSession&) = delete;
  FixtureEngineSession& operator=(const FixtureEngineSession&) = delete;
  sb_engine_session_t get() const { return session_; }
  static void Check(sb_engine_status_t status, sb_engine_result_t result,
                    const char* phase) {
    if (status != SB_ENGINE_STATUS_OK && result != nullptr) {
      sb_engine_diagnostic_set_view_t diagnostics{};
      if (sb_engine_result_diagnostics(result, &diagnostics) == SB_ENGINE_STATUS_OK) {
        for (std::size_t i = 0; i < diagnostics.diagnostic_count; ++i) {
          const auto& d = diagnostics.diagnostics[i];
          for (const auto text : {d.symbolic_code, d.message_key, d.safe_detail}) {
            if (text.data) std::cerr.write(text.data, text.size_bytes);
            std::cerr << ':';
          }
          std::cerr << '\n';
        }
      }
    }
    if (result) sb_engine_result_release(result);
    if (status != SB_ENGINE_STATUS_OK)
      throw std::runtime_error(std::string("engine fixture ") + phase + " failed");
  }
 private:
  sb_engine_handle_t engine_ = nullptr;
  sb_engine_session_t session_ = nullptr;
};

class FixtureEngineStatement {
 public:
  FixtureEngineStatement(const FixtureEngineSession& session, const api::EngineRequestContext& base) {
    namespace bridge = scratchbird::server_engine_bridge;
    bridge::StatementContextAcquireRequest request;
    request.engine_context = &base;
    request.exact_transaction_uuid = base.transaction_uuid;
    bridge::StatementContextReceiptView view;
    sb_engine_result_t result = nullptr;
    const auto status = bridge::AcquireStatementContextReceipt(
        session.get(), &request, &receipt_, &view, &result);
    FixtureEngineSession::Check(status, result, "statement acquisition");
    result = nullptr;
    const auto copied = bridge::CopyStatementContextEngineContextV1(
        receipt_, &context, &result);
    FixtureEngineSession::Check(copied, result, "statement context copy");
  }
  ~FixtureEngineStatement() {
    (void)scratchbird::server_engine_bridge::ReleaseStatementContextReceipt(receipt_);
  }
  FixtureEngineStatement(const FixtureEngineStatement&) = delete;
  FixtureEngineStatement& operator=(const FixtureEngineStatement&) = delete;
  api::EngineRequestContext context;
 private:
  scratchbird::server_engine_bridge::StatementContextReceiptHandle receipt_;
};


// Returned test requests keep the engine-issued statement receipt alive through
// the API call; copying a request shares that receipt without minting authority.
template<class Request>
struct FixtureEngineRequest : Request {
  FixtureEngineRequest(const FixtureEngineSession& session,
                       const api::EngineRequestContext& context)
      : statement(std::make_shared<FixtureEngineStatement>(session, context)) {
    this->context = statement->context;
  }
 private:
  std::shared_ptr<FixtureEngineStatement> statement;
};

inline void ConfigureCredentialedFixtureBootstrap(storage::database::DatabaseCreateConfig& create) {
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  create.resource_seed_pack_root =
      (std::filesystem::path(__FILE__).lexically_normal().parent_path().parent_path().parent_path() /
       "resources/seed-packs/initial-resource-pack").string();
  create.bootstrap_principal_name = "fixture_owner";
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.bootstrap_credential_fingerprint =
      "local-password-pbkdf2-sha256:v1:iterations=600000:"
      "salt=0123456789abcdef0123456789abcdef:"
      "verifier=58a793aad0bd6840ad8d92f6627a23f6142c4ce58210c5f135ea3e2134d43142";
}
inline api::EngineRequestContext BootstrapFixtureOwnerContext(
    const storage::database::DatabaseCreateConfig& create) {
  api::EngineRequestContext context;
  context.database_path = create.path;
  context.database_uuid = create.database_uuid.value;
  context.default_root_uuid = create.filespace_uuid.value;
  context.session_uuid = api::GenerateCrudEngineUuid("object");
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.security_context_present = true;
  // Fresh bootstrap session epochs, matching engine session admission. Object
  // descriptor generations are obtained separately from catalog publication.
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.datatype_catalog_snapshot_uuid = api::kBootstrapDatatypeCatalogUuid;
  context.datatype_catalog_generation = api::kBootstrapDatatypeCatalogGeneration;
  context.datatype_registry_generation = api::kBootstrapDatatypeRegistryGeneration;
  const auto bootstrap = storage::database::ReadDatabaseBootstrapSecurityCatalog(create.path);
  if (!bootstrap.ok() || !bootstrap.state.present || !bootstrap.state.committed_by_inventory)
    throw std::runtime_error("fixture durable bootstrap owner is missing");
  context.principal_uuid = bootstrap.state.principal_uuid.value;
  MaterializeBootstrapFixtureAuthorization(context);
  return context;
}
} // namespace scratchbird::tests
