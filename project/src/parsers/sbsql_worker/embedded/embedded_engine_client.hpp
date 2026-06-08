// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SBSQL_EMBEDDED_ENGINE_CLIENT

#pragma once

#include "ipc/sbps_client.hpp"

#include <memory>

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
  PublicNameResolutionResult RenderUuidPublic(const SessionContext& session,
                                              std::string_view object_uuid);
  ServerExecutionResult ExecuteSblr(const SessionContext& session,
                                    std::string_view encoded_sblr_envelope,
                                    bool cursor_requested = false);
  ServerFetchResult FetchCursor(const SessionContext& session,
                                std::string_view cursor_uuid,
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
