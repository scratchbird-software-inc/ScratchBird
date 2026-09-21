// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "session_registry.hpp"

namespace scratchbird::server {

// The server retains cleanup ownership until the engine confirms a terminal
// release. Invoke the owning engine operation outside the server map lock.
// Release is a private implementation seam, never parser-provided authority.
template <typename Release>
bool ReleaseOwnedStatementReceipt(
    ServerSessionRegistry* registry,
    const core::platform::Uuid& statement_uuid,
    Release&& release,
    const std::array<std::uint8_t, 16>* expected_session = nullptr) {
  if (registry == nullptr || registry->statement_context_mutex == nullptr ||
      !core::uuid::IsEngineIdentityUuid(statement_uuid)) return false;
  server_engine_bridge::StatementContextReceiptHandle receipt;
  core::platform::Uuid receipt_uuid;
  std::array<std::uint8_t, 16> owner_session{};
  {
    std::lock_guard lock(*registry->statement_context_mutex);
    const auto found = registry->statement_contexts_by_statement_uuid.find(statement_uuid);
    if (found == registry->statement_contexts_by_statement_uuid.end()) return false;
    auto& record = found->second;
    if (record.release_in_progress || !record.receipt ||
        (expected_session != nullptr && record.session_uuid != *expected_session)) return false;
    // Setting released prevents every later execution lookup from adopting the
    // receipt, including when revocation or physical cleanup subsequently fails.
    record.released = true;
    record.release_in_progress = true;
    receipt = record.receipt;
    receipt_uuid = record.view.receipt_uuid;
    owner_session = record.session_uuid;
  }
  sb_engine_status_t status = SB_ENGINE_STATUS_INTERNAL_ERROR;
  try {
    status = release(receipt, receipt_uuid);
  } catch (...) {
    // A destructor-driven release cannot throw away its only cleanup handle.
    // Leave the logically revoked owner in the registry for another attempt.
  }
  std::lock_guard lock(*registry->statement_context_mutex);
  const auto found = registry->statement_contexts_by_statement_uuid.find(statement_uuid);
  if (found == registry->statement_contexts_by_statement_uuid.end() ||
      found->second.receipt != receipt ||
      found->second.view.receipt_uuid != receipt_uuid ||
      found->second.session_uuid != owner_session) return false;
  found->second.release_in_progress = false;
  if (status != SB_ENGINE_STATUS_OK && status != SB_ENGINE_STATUS_ALREADY_RELEASED)
    return false;
  registry->statement_contexts_by_statement_uuid.erase(found);
  return true;
}

template <typename Release>
std::uint64_t ReleaseOwnedStatementReceiptsForSession(
    ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid,
    Release&& release) {
  if (registry == nullptr || registry->statement_context_mutex == nullptr) return 0;
  std::vector<core::platform::Uuid> statements;
  try {
    std::lock_guard lock(*registry->statement_context_mutex);
    for (const auto& [statement_uuid, record] :
         registry->statement_contexts_by_statement_uuid) {
      if (record.session_uuid == session_uuid) statements.push_back(statement_uuid);
    }
  } catch (const std::bad_alloc&) {
    // Enumeration has not revoked or removed any owner yet.
    return 0;
  }
  std::uint64_t released = 0;
  for (const auto& statement_uuid : statements)
    if (ReleaseOwnedStatementReceipt(registry, statement_uuid, release, &session_uuid)) ++released;
  return released;
}

}  // namespace scratchbird::server
