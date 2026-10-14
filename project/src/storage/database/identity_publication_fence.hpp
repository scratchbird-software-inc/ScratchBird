// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "local_transaction_store.hpp"
#include <algorithm>
#include <thread>
#include <stdexcept>
#include <utility>

namespace scratchbird::engine::internal_api {
struct EngineCreateIdentityRequest;
struct EngineCreateIdentityResult;
EngineCreateIdentityResult EngineCreateIdentity(const EngineCreateIdentityRequest&);
}

namespace scratchbird::storage::database {
// Lexical execution custody, NOT authentication, permission or finality.
// Only the admitted engine identity composer issues it. Its durable rollback-
// only marker precedes every effect and is never released during unwinding.
// Ordinary writers cannot manufacture a borrowed execution exception to that
// marker. Neither request flags nor a retained snapshot establish this custody.
class IdentityPublicationFence final {
 public:
  IdentityPublicationFence(const IdentityPublicationFence&) = delete;
  IdentityPublicationFence& operator=(const IdentityPublicationFence&) = delete;
  ~IdentityPublicationFence() {
    if (std::this_thread::get_id() != thread_) std::terminate();
  }
  bool Allows(const std::string& path, const core::platform::Uuid& database,
              u64 local_id, const core::platform::Uuid& transaction,
              const transaction::mga::LocalTransactionInventory& inventory) const noexcept {
    if (thread_ != std::this_thread::get_id() || !armed_ || path != path_ ||
        database != database_ || transaction != transaction_ || local_id != local_id_ ||
        !inventory.publication_base || !marked_.publication_base ||
        *inventory.publication_base != *marked_.publication_base) return false;
    const auto entry = std::find_if(inventory.entries.begin(), inventory.entries.end(),
        [&](const auto& row) { return row.identity.local_id.value == local_id_; });
    return entry != inventory.entries.end() && entry->identity.transaction_uuid.value == transaction_ &&
        entry->state == transaction::mga::TransactionState::active && entry->rollback_only;
  }
 private:
  friend engine::internal_api::EngineCreateIdentityResult
      engine::internal_api::EngineCreateIdentity(const engine::internal_api::EngineCreateIdentityRequest&);
  IdentityPublicationFence(std::string path, core::platform::Uuid database,
                           u64 local_id, core::platform::Uuid transaction)
      : path_(std::move(path)), database_(database), transaction_(transaction), local_id_(local_id) {}
  LocalTransactionStoreResult Begin() {
    auto loaded = LoadLocalTransactionInventoryFromDatabase(path_);
    if (!loaded.ok()) return loaded;
    const auto entry = std::find_if(loaded.inventory.entries.begin(), loaded.inventory.entries.end(),
        [&](const auto& row) { return row.identity.local_id.value == local_id_; });
    if (!loaded.inventory.publication_base ||
        loaded.inventory.publication_base->database_uuid != database_ ||
        entry == loaded.inventory.entries.end() || entry->identity.transaction_uuid.value != transaction_ ||
        entry->state != transaction::mga::TransactionState::active || entry->rollback_only)
      throw std::logic_error("identity publication lost its admitted native transaction owner");
    auto marked = transaction::mga::MarkLocalTransactionRollbackOnly(
        std::move(loaded.inventory), transaction::mga::MakeLocalTransactionId(local_id_));
    if (!marked.ok()) {
      loaded.status = marked.status; loaded.diagnostic = std::move(marked.diagnostic);
      return loaded;
    }
    auto written = PersistLocalTransactionInventoryToDatabase(path_, std::move(marked.inventory));
    if (written.ok()) { marked_ = written.inventory; armed_ = true; }
    return written;
  }
  LocalTransactionStoreResult Complete() {
    // This invocation's exact publication base alone can release its exclusion.
    // Never reload a newer base to get around an ownership conflict.
    auto completed = marked_;
    auto entry = std::find_if(completed.entries.begin(), completed.entries.end(),
        [&](const auto& row) { return row.identity.local_id.value == local_id_; });
    if (!Allows(path_, database_, local_id_, transaction_, completed))
      throw std::logic_error("identity publication exclusion ownership is invalid");
    entry->rollback_only = false;
    auto result = PersistLocalTransactionInventoryToDatabase(path_, std::move(completed));
    if (result.ok()) armed_ = false;
    return result;
  }
  const std::string path_;
  const core::platform::Uuid database_, transaction_;
  const u64 local_id_;
  const std::thread::id thread_ = std::this_thread::get_id();
  transaction::mga::LocalTransactionInventory marked_;
  bool armed_ = false;
};
} // namespace scratchbird::storage::database
