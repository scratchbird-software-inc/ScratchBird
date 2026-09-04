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
#include <vector>

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
                                              std::string_view object_uuid);
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
                                std::string_view cursor_uuid,
                                const ipc::CursorStreamDescriptorV1& stream_descriptor,
                                std::uint64_t max_rows = 1,
                                std::uint64_t max_bytes = 0,
                                std::uint32_t fetch_flags = 0);
  ServerCloseCursorResult CloseCursor(const SessionContext& session,
                                      std::string_view cursor_uuid);
  ServerCloseCursorResult CancelCursor(const SessionContext& session,
                                       std::string_view cursor_uuid);
  ServerManagementResult Manage(const SessionContext& session,
                                std::string_view operation_key,
                                std::string_view target_uuid = {},
                                std::string_view mode = {},
                                std::string_view audit_reason = {},
                                std::uint64_t timeout_ms = 30000,
                                bool include_history = false);
  bool DisconnectSession(const SessionContext& session, MessageVectorSet* messages);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace scratchbird::parser::sbsql
