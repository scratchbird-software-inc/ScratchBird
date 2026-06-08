// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "common/common.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::parser::sbsql {

struct ServerExecutionResult {
  bool accepted{false};
  std::string operation_id;
  std::string cursor_uuid;
  std::uint64_t row_count{0};
  std::string row_packet;
  bool transaction_state_present{false};
  std::uint64_t local_transaction_id{0};
  std::uint64_t snapshot_visible_through_local_transaction_id{0};
  std::string transaction_uuid;
  std::string transaction_timestamp;
  MessageVectorSet messages;
};

struct ServerFetchResult {
  bool accepted{false};
  std::string cursor_uuid;
  std::uint64_t row_count{0};
  std::string row_packet;
  std::string detail;
  bool end_of_cursor{false};
  MessageVectorSet messages;
};

struct ServerCloseCursorResult {
  bool accepted{false};
  std::string cursor_uuid;
  std::string detail;
  MessageVectorSet messages;
};

struct ServerManagementResult {
  bool accepted{false};
  std::string operation_key;
  std::string payload;
  MessageVectorSet messages;
};

struct AuthCredentialEnvelope {
  std::string provider_family{"local_password"};
  std::string principal;
  std::string requested_database{"default"};
  std::string requested_language{"en"};
  std::string application_name;
  std::string credential_evidence;
  bool credential_evidence_present{false};
  bool credential_invalid{false};
  bool mfa_required{false};
  bool mfa_evidence_present{false};
};

struct PublicNameResolutionResult {
  bool resolved{false};
  std::string object_uuid;
  std::string canonical_name;
  std::string object_class;
  std::uint64_t catalog_epoch{0};
  std::uint64_t security_epoch{0};
  MessageVectorSet messages;
};

class SbpsClient {
 public:
  explicit SbpsClient(std::string endpoint);

  [[nodiscard]] bool configured() const { return !endpoint_.empty(); }

  bool SendHello(MessageVectorSet* messages) const;
  bool AuthenticateAndAttach(const AuthCredentialEnvelope& credentials,
                             const ParserConfig& config,
                             SessionContext* session,
                             MessageVectorSet* messages) const;
  bool AuthenticateAndAttach(std::string_view auth_payload,
                             const ParserConfig& config,
                             SessionContext* session,
                             MessageVectorSet* messages) const;
  PublicNameResolutionResult ResolveNamePublic(const SessionContext& session,
                                               std::string_view presented_name,
                                               bool quoted,
                                               std::string_view object_class,
                                               const ParserConfig& config) const;
  PublicNameResolutionResult RenderUuidPublic(const SessionContext& session,
                                              std::string_view object_uuid) const;
  ServerExecutionResult ExecuteSblr(const SessionContext& session,
                                    std::string_view encoded_sblr_envelope,
                                    bool cursor_requested = false) const;
  ServerFetchResult FetchCursor(const SessionContext& session,
                                std::string_view cursor_uuid,
                                std::uint64_t max_rows = 1,
                                std::uint64_t max_bytes = 0,
                                std::uint32_t fetch_flags = 0) const;
  ServerCloseCursorResult CloseCursor(const SessionContext& session,
                                      std::string_view cursor_uuid) const;
  ServerCloseCursorResult CancelCursor(const SessionContext& session,
                                       std::string_view cursor_uuid) const;
  ServerManagementResult Manage(const SessionContext& session,
                                std::string_view operation_key,
                                std::string_view target_uuid = {},
                                std::string_view mode = {},
                                std::string_view audit_reason = {},
                                std::uint64_t timeout_ms = 30000,
                                bool include_history = false) const;
  bool DisconnectSession(const SessionContext& session, MessageVectorSet* messages) const;

 private:
  std::string endpoint_;
};

} // namespace scratchbird::parser::sbsql
