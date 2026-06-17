// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "auth/auth_relay.hpp"
#include "cache/sblr_template_cache.hpp"
#include "metrics/parser_metrics.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <memory>

namespace scratchbird::parser::sbsql {

struct AuthCredentialEnvelope;
struct PublicNameResolutionResult;
struct ServerCloseCursorResult;
struct ServerExecutionResult;
struct ServerFetchResult;
struct ServerManagementResult;
class EmbeddedEngineClient;
struct ServerManagementCommand {
  std::string operation_key;
  std::string operation_id;
  std::string mode;
  std::string audit_reason;
};

struct WireResponse {
  bool close{false};
  std::string text;
};

class SbsqlTestWireSession {
 public:
  SbsqlTestWireSession(ParserConfig config, ParserMetrics* metrics, SblrTemplateCache* cache);
  ~SbsqlTestWireSession();
  WireResponse HandleLine(std::string_view line);
  int ServeFd(std::intptr_t fd);
  PipelineResult RunPipeline(std::string_view sql,
                             bool submit,
                             bool cursor_requested = false,
                             std::uint64_t stream_row_count = 0,
                             bool autocommit_emulation = false);
  PipelineResult RunSblrEnvelope(std::string_view encoded_sblr_envelope,
                                 bool cursor_requested = false);
  ServerFetchResult FetchCursorOnRoute(std::string_view cursor_uuid,
                                       std::uint64_t max_rows = 1,
                                       std::uint64_t max_bytes = 0,
                                       std::uint32_t fetch_flags = 0);
  ServerCloseCursorResult CloseCursorOnRoute(std::string_view cursor_uuid);
  ServerCloseCursorResult CancelCursorOnRoute(std::string_view cursor_uuid);
  bool AuthenticateCredentials(const AuthCredentialEnvelope& credentials,
                               MessageVectorSet* messages);
  [[nodiscard]] const SessionContext& session() const { return session_; }

 private:
  ParserConfig config_;
  ParserMetrics* metrics_;
  SblrTemplateCache* cache_;
  SessionContext session_;
  std::string last_cursor_uuid_;
  std::unique_ptr<EmbeddedEngineClient> embedded_client_;

  bool HasExecutionRoute() const;
  ServerExecutionResult ExecuteSblrOnRoute(std::string_view encoded_sblr_envelope,
                                           bool cursor_requested = false);
  PublicNameResolutionResult ResolveNameOnRoute(std::string_view presented_name,
                                                bool quoted,
                                                std::string_view object_class);
  bool DisconnectExecutionRoute(MessageVectorSet* messages);
  PipelineResult RunServerManagementCommand(const ServerManagementCommand& command);
  int ServeSbwp(std::intptr_t fd);
};

} // namespace scratchbird::parser::sbsql
