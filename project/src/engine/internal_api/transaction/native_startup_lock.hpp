// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "transaction/mga/lock_wait_lifecycle.hpp"
#include <optional>
#include <exception>
#include <string_view>

namespace scratchbird::server {class NativeHostedDatabaseRuntime;}
namespace scratchbird::engine::internal_api {
// Native in-memory representation of MGALockRequest/MGALockGrant. Optional
// identities use nil, not allocated text. The first local startup has no
// statement, route, cluster, donor, record, table or deadline. These records
// describe ownership; copying them cannot acquire or restore a lock.
struct NativeStartupLockObservation {
  core::platform::Uuid request_id,owner_uuid,transaction_uuid,statement_uuid,operation_id;
  core::platform::Uuid scope_uuid,record_uuid,table_uuid,database_uuid,cluster_uuid;
  std::optional<core::platform::u64> route_generation;
  std::string_view route_owner_token,donor_profile;
  core::platform::Uuid deadline_uuidv7,requested_at_uuidv7;
  transaction::mga::MGALockOwnerKind owner_kind=transaction::mga::MGALockOwnerKind::transaction;
  transaction::mga::MGALockScopeKind scope_kind=transaction::mga::MGALockScopeKind::database;
  transaction::mga::MGALockMode mode=transaction::mga::MGALockMode::maintenance_exclusive;
  transaction::mga::MGAWaitPolicy wait_policy=transaction::mga::MGAWaitPolicy::no_wait;
  transaction::mga::MGAPriorityClass priority_class=transaction::mga::MGAPriorityClass::maintenance;
  core::platform::u64 policy_epoch=0;
  core::platform::Uuid grant_id,granted_at_uuidv7;
  transaction::mga::MGALockMode granted_mode=transaction::mga::MGALockMode::maintenance_exclusive;
  core::platform::u64 grant_generation=0;
  transaction::mga::MGALockReleaseCondition release_condition=transaction::mga::MGALockReleaseCondition::transaction_end;
  transaction::mga::MGADurableRecoverySource durable_recovery_source=transaction::mga::MGADurableRecoverySource::transaction_inventory;
  bool operator==(const NativeStartupLockObservation&) const=default;
};
class NativeStartupLockRegistration;
// One exact exclusive database slot for the private bootstrap interval. This
// slot must drain before the host can admit ordinary work. It does not pretend
// to be a generic wait queue, deadlock graph, or clustered write-intent table.
class NativeStartupLockTable final {
  friend class scratchbird::server::NativeHostedDatabaseRuntime;
  friend class NativeStartupLockRegistration;
  NativeStartupLockTable()=default;
  ~NativeStartupLockTable(){if(registration_)std::terminate();}
  NativeStartupLockTable(const NativeStartupLockTable&)=delete;
  NativeStartupLockTable& operator=(const NativeStartupLockTable&)=delete;
  NativeStartupLockRegistration* registration_=nullptr;
  core::platform::u64 generation_=0;
};
// Stable placement in the actual governed transaction arena. Only the host can
// register it, after authenticated policy admission and selected durable audit.
// Destruction retires process-local ownership; it never commits or rolls back
// the inventory. A new host must reconcile unfinished inventory separately.
class NativeStartupLockRegistration final {
 public:
  ~NativeStartupLockRegistration(){
    if(table_){
      if(table_->registration_!=this||table_->generation_!=observation_.grant_generation)std::terminate();
      table_->registration_=nullptr;
    }
  }
  NativeStartupLockRegistration(const NativeStartupLockRegistration&)=delete;
  NativeStartupLockRegistration& operator=(const NativeStartupLockRegistration&)=delete;
 private:
  friend class scratchbird::server::NativeHostedDatabaseRuntime;
  friend class NativeStartupTransactionOwner;
  NativeStartupLockRegistration()=default;
  NativeStartupLockTable* table_=nullptr;
  NativeStartupLockObservation observation_;
};
} // namespace scratchbird::engine::internal_api
