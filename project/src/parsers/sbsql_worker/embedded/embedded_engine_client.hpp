// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBSQL_EMBEDDED_ENGINE_CLIENT

#pragma once

#include "common/common.hpp"
#include "ipc/sbps_client.hpp"

#include <memory>
#include <string>
#include <vector>

#if defined(SB_EMBEDDED_CLIENT_EXTERNAL_ENGINE_TEST_SEAM)
namespace scratchbird::server {
struct HostedEngineState;
struct ServerSessionRegistry;
}  // namespace scratchbird::server
#endif

namespace scratchbird::parser::sbsql {

class EmbeddedEngineClient {
 public:
  explicit EmbeddedEngineClient(ParserConfig config);
  ~EmbeddedEngineClient();

  EmbeddedEngineClient(const EmbeddedEngineClient&) = delete;
  EmbeddedEngineClient& operator=(const EmbeddedEngineClient&) = delete;

  bool AuthenticateAndAttachSysarch(const AuthCredentialEnvelope& credentials,
                                    SessionContext* session,
                                    MessageVectorSet* messages);
  bool AuthenticateAndAttach(const AuthCredentialEnvelope& credentials,
                             SessionContext* session,
                             MessageVectorSet* messages);
  PublicNameResolutionResult ResolveNamePublic(const SessionContext& session,
                                               std::string_view presented_name,
                                               bool quoted,
                                               std::string_view object_class,
                                               const ParserConfig& config);
  std::vector<PublicNameResolutionResult> ResolveRelationDescriptorsPublic(
      const SessionContext& session,
      const std::vector<ipc::PublicRelationResolutionRequest>& requests,
      const ParserConfig& config);
  PublicNameResolutionResult RenderUuidPublic(const SessionContext& session,
                                              const scratchbird::core::platform::Uuid& object_uuid);
  ipc::ServerStatementContextResult AcquireNativeStatementContext(
      const SessionContext& session,
      const ipc::ParserTransactionSelector& transaction);
  ipc::ServerLiteralBindingResult NegotiateLiteralDescriptors(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbln);
  ipc::ServerLiteralBindingResult IssueContextualTextLiteralProfiles(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbtlnr);
  ipc::ServerLiteralBindingResult FinalizeLiteralBinding(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_sblf);
  ipc::ServerParameterBindingResult NegotiateParameterDescriptors(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbpr);
  ipc::ServerParameterBindingResult FinalizeParameterBinding(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_sbpf);
  ipc::ServerVariableBindingResult BindStmtPrepare(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateStmtPrepare(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult BindStmtExecuteDirect(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateStmtExecuteDirect(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateStmtExecute(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult BindStmtFree(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateStmtFree(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult BindStmtCancel(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateStmtCancel(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult BindParameterBind(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateParameterBind(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateResultPage(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult BindQueryExplain(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateQueryExplain(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult BindNameResolve(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateNameResolve(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateCatalogIntrospect(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateDdlCreateSchema(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateSecurityAlterPolicy(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateDdlCreateProcedure(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateProcedureInvoke(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateDdlCreateTrigger(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateDdlAlterTrigger(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateDdlDropTrigger(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult RecoverDdlCreateSchema(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult BindParseText(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateParseText(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult BindCatalogEpochCheck(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateCatalogEpochCheck(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult BindDatabaseAttach(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult RetainSourceArtifact(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateDatabaseAttach(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateOptimizerStatsRead(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateOptimizerStatsDrop(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateBulkImportStream(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerBulkImportBindResult BindBulkImportStream(
      const SessionContext& session,
      const scratchbird::wire::sbps_bulk_import::Bind& bind);
  ipc::ServerBulkImportChunkResult AppendBulkImportStream(
      const SessionContext& session,
      const scratchbird::wire::sbps_bulk_import::Chunk& chunk);
  ipc::ServerBulkImportSealResult SealBulkImportStream(
      const SessionContext& session,
      const scratchbird::wire::sbps_bulk_import::Seal& seal);
  ipc::ServerVariableBindingResult CoordinateDmlDeleteRowsBind(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateDmlUpdateRowsBind(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ipc::ServerVariableBindingResult CoordinateDmlPlanImportRowsBind(
      const SessionContext& session,
      const std::vector<std::uint8_t>& canonical_request);
  ServerExecutionResult ExecuteCanonicalSblrWithDataPacket(
      const SessionContext& session,
      const ipc::ParserStatementContext& statement_context,
      const ipc::ParserCanonicalSblrSubmission& submission,
      const std::vector<std::uint8_t>& data_packet,
      bool cursor_requested = false);
  ServerExecutionResult ExecuteSblr(const SessionContext& session,
                                    std::string_view encoded_sblr_envelope,
                                    bool cursor_requested = false);
  ServerExecutionResult ExecuteSblrWithDataPacket(
      const SessionContext& session,
      std::string_view encoded_sblr_envelope,
      const std::vector<std::uint8_t>& data_packet,
      bool cursor_requested = false);
  ServerFetchResult FetchCursor(const SessionContext& session,
                                const scratchbird::core::platform::Uuid& cursor_uuid,
                                const ipc::CursorStreamDescriptorV1& stream_descriptor,
                                std::uint64_t max_rows = 1,
                                std::uint64_t max_bytes = 0,
                                std::uint32_t fetch_flags = 0);
  ServerCloseCursorResult CloseCursor(const SessionContext& session,
                                      const scratchbird::core::platform::Uuid& cursor_uuid);
  ServerCloseCursorResult CancelCursor(const SessionContext& session,
                                       const scratchbird::core::platform::Uuid& cursor_uuid);
  ServerManagementResult Manage(const SessionContext& session,
                                std::string_view operation_key,
                                std::string_view target_uuid = {},
                                std::string_view mode = {},
                                std::string_view audit_reason = {},
                                std::uint64_t timeout_ms = 30000,
                                bool include_history = false);
  bool DisconnectSession(const SessionContext& session, MessageVectorSet* messages);

#if defined(SB_EMBEDDED_CLIENT_EXTERNAL_ENGINE_TEST_SEAM)
  // SEARCH_KEY: SB_EMBEDDED_CLIENT_EXTERNAL_ENGINE_TEST_SEAM (2026-12-12; the in-process route B test, native_route_b_test.cpp).
  // TEST ONLY. Compiled only into a test target that defines the macro AND compiles embedded_engine_client.cpp itself (the production libraries
  // sbl_sbsql_embedded_client_bridge / sbl_sbsql_parser_worker_core / scratchbird_client are built without it and carry neither symbol; nm-checked
  // by native_route_b_seam_symbols). Binds this client to an engine state and a session registry the TEST owns (the ones its served native
  // database and its sessions live in) instead of opening a database path: every handler the client calls then runs on them. The client does
  // not own them; the caller keeps them alive for the client's life.
  void BindExternalEngineForTest(scratchbird::server::HostedEngineState* engine_state,
                                 scratchbird::server::ServerSessionRegistry* registry);
  // The engine's own row packet of the most recent canonical execute (the native result vector, before the parser turns it into a client row).
  [[nodiscard]] const std::string& LastExecuteRowPacketForTest() const;
  // The server's own diagnostics (code{key=value;...}) of the most recent canonical execute, before the parser flattens them into its messages.
  [[nodiscard]] const std::string& LastExecuteServerDiagnosticsForTest() const;
  // Forgets both of the above (so a statement that never reaches the engine shows nothing stale).
  void ResetLastExecuteForTest();
#endif

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scratchbird::parser::sbsql
