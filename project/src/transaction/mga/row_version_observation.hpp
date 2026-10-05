// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "row_version.hpp"
#include "transaction_identity_observation.hpp"
#include <algorithm>
#include <span>

namespace scratchbird::transaction::mga {
// Borrowed exclusions retain the authority owner's lifetime. This carrier is
// calculation input, NOT a snapshot pin or permission to access an inventory.
struct VisibilitySnapshotView {
  LocalTransactionId reader_transaction;
  u64 visible_through_local_transaction_id = kInvalidLocalTransactionId;
  bool visible_through_local_transaction_id_is_boundary = false;
  bool allow_reader_own_uncommitted = true;
  bool recovery_context = false;
  std::span<const u64> active_excluded_local_transaction_ids;
  std::span<const u64> in_doubt_excluded_local_transaction_ids;
  u64 visible_through_commit_sequence = 0;
  bool visible_through_commit_sequence_is_boundary = false;
};
inline VisibilitySnapshotView BorrowVisibilitySnapshot(const VisibilitySnapshot& s) noexcept {
  return {s.reader_transaction,s.visible_through_local_transaction_id,
    s.visible_through_local_transaction_id_is_boundary,s.allow_reader_own_uncommitted,s.recovery_context,
    s.active_excluded_local_transaction_ids,s.in_doubt_excluded_local_transaction_ids,
    s.visible_through_commit_sequence,s.visible_through_commit_sequence_is_boundary};
}
namespace row_observation_detail {
inline MgaObservationStatus Error(const char* code,const char* key,const char* detail="") noexcept {
  return {{core::platform::StatusCode::platform_required_feature_missing,
    core::platform::Severity::error,core::platform::Subsystem::transaction_mga},code,key,detail};
}
} // namespace row_observation_detail
inline MgaObservationStatus ObserveRowIdentity(const RowIdentity& identity) noexcept {
  using row_observation_detail::Error;
  if(identity.row_uuid.kind!=UuidKind::row||!identity.row_uuid.valid())
    return Error("SB-ROW-INVALID-ROW-UUID-KIND","row_version.invalid_row_uuid_kind");
  if(!core::uuid::IsEngineIdentityUuid(identity.row_uuid.value))
    return Error("SB-ROW-ROW-UUID-MUST-BE-V7","row_version.row_uuid_must_be_v7");
  return {};
}
inline MgaObservationStatus ObserveRowVersionIdentity(const RowVersionIdentity& identity) noexcept {
  using row_observation_detail::Error;
  if(!core::uuid::IsEngineIdentityUuid(identity.version_uuid)||identity.version_uuid==identity.row.row_uuid.value)
    return Error("CATALOG.INVALID_INPUT","row_version.invalid_version_uuid");
  const auto row=ObserveRowIdentity(identity.row);if(!row.ok())return row;
  const auto tx=ObserveTransactionIdentity(identity.creator_transaction);if(!tx.ok())return tx;
  if(identity.version_sequence==kInvalidRowVersionSequence)
    return Error("SB-ROW-INVALID-VERSION-SEQUENCE","row_version.invalid_version_sequence");
  return {};
}
inline MgaObservationStatus ObserveRowVersionMetadata(const RowVersionMetadata& metadata) noexcept {
  using row_observation_detail::Error;
  const auto identity=ObserveRowVersionIdentity(metadata.identity);if(!identity.ok())return identity;
  if(metadata.state==RowVersionState::unknown)
    return Error("SB-ROW-UNKNOWN-VERSION-STATE","row_version.unknown_version_state");
  if(metadata.creator_transaction_state==TransactionState::none||metadata.creator_transaction_state==TransactionState::archived)
    return Error("SB-ROW-UNKNOWN-CREATOR-TRANSACTION-STATE","row_version.unknown_creator_transaction_state");
  if(!metadata.payload_present&&metadata.state!=RowVersionState::delete_marker&&metadata.state!=RowVersionState::rolled_back)
    return Error("SB-ROW-MISSING-VERSION-PAYLOAD","row_version.missing_version_payload",RowVersionStateName(metadata.state));
  if(metadata.chain.has_previous()&&metadata.chain.previous_version_sequence>=metadata.identity.version_sequence)
    return Error("SB-ROW-INVALID-PREVIOUS-VERSION-LINK","row_version.invalid_previous_version_link");
  if(metadata.chain.has_next()&&metadata.chain.next_version_sequence<=metadata.identity.version_sequence)
    return Error("SB-ROW-INVALID-NEXT-VERSION-LINK","row_version.invalid_next_version_link");
  return {};
}
struct VisibilityObservation {
  MgaObservationStatus outcome;
  VisibilityDecision decision=VisibilityDecision::unknown;
  bool ok() const noexcept {return outcome.ok();}
};
namespace row_observation_detail {
inline VisibilityObservation Evaluate(const RowVersionMetadata& metadata,
    const VisibilitySnapshotView& snapshot,bool delete_effect) noexcept {
  const auto validated=ObserveRowVersionMetadata(metadata);
  if(!validated.ok())return {validated,VisibilityDecision::unknown};
  const auto warning=[](VisibilityDecision d,const char* code,const char* key,const char* detail){
    auto outcome=Error(code,key,detail);outcome.status.severity=core::platform::Severity::warning;
    return VisibilityObservation{outcome,d};
  };
  auto state=metadata.state;
  if(delete_effect&&state==RowVersionState::delete_marker){
    switch(metadata.creator_transaction_state){
      case TransactionState::committed:state=RowVersionState::committed;break;
      case TransactionState::rolled_back:
      case TransactionState::failed_terminal:state=RowVersionState::rolled_back;break;
      case TransactionState::prepared:state=RowVersionState::prepared;break;
      case TransactionState::limbo:state=RowVersionState::limbo;break;
      case TransactionState::recovering:state=RowVersionState::recovery_required;break;
      case TransactionState::created:
      case TransactionState::active:
      case TransactionState::preparing:
      case TransactionState::committing:
      case TransactionState::rolling_back:
      case TransactionState::read_only_active:state=RowVersionState::uncommitted;break;
      default:return {Error("SB-ROW-UNKNOWN-CREATOR-TRANSACTION-STATE","row_version.unknown_creator_transaction_state"),VisibilityDecision::unknown};
    }
  }
  if(state==RowVersionState::recovery_required||state==RowVersionState::limbo)
    return warning(VisibilityDecision::requires_recovery,"SB-ROW-VISIBILITY-REQUIRES-RECOVERY",
      "row_version.visibility_requires_recovery",RowVersionStateName(metadata.state));
  if(state==RowVersionState::prepared||state==RowVersionState::uncommitted){
    if(snapshot.allow_reader_own_uncommitted&&snapshot.reader_transaction.valid()&&
       metadata.identity.creator_transaction.local_id.value==snapshot.reader_transaction.value)
      return {{},VisibilityDecision::visible};
    return warning(VisibilityDecision::wait_for_transaction,"SB-ROW-VISIBILITY-WAITS-FOR-TRANSACTION",
      "row_version.visibility_waits_for_transaction",RowVersionStateName(metadata.state));
  }
  if(state==RowVersionState::rolled_back||state==RowVersionState::delete_marker)
    return {{},VisibilityDecision::invisible};
  if(!(state==RowVersionState::committed&&metadata.creator_transaction_state==TransactionState::committed))
    return warning(VisibilityDecision::wait_for_transaction,"SB-ROW-CREATOR-NOT-COMMITTED",
      "row_version.creator_not_committed",TransactionStateName(metadata.creator_transaction_state));
  const auto creator=metadata.identity.creator_transaction.local_id.value;
  if((snapshot.visible_through_local_transaction_id_is_boundary||snapshot.visible_through_local_transaction_id!=kInvalidLocalTransactionId)&&
      creator>snapshot.visible_through_local_transaction_id)return {{},VisibilityDecision::invisible};
  if(snapshot.visible_through_commit_sequence_is_boundary){
    if(!metadata.creator_commit_sequence)return {Error("CATALOG.INVALID_INPUT","row_version.creator_commit_sequence_missing"),VisibilityDecision::unknown};
    if(metadata.creator_commit_sequence>snapshot.visible_through_commit_sequence)return {{},VisibilityDecision::invisible};
  }
  const auto excluded=[creator](auto values){return std::find(values.begin(),values.end(),creator)!=values.end();};
  if(excluded(snapshot.active_excluded_local_transaction_ids)||excluded(snapshot.in_doubt_excluded_local_transaction_ids))
    return {{},VisibilityDecision::invisible};
  return {{},VisibilityDecision::visible};
}
} // namespace row_observation_detail
inline VisibilityObservation ObserveVisibility(const RowVersionMetadata& m,const VisibilitySnapshotView& s) noexcept {
  return row_observation_detail::Evaluate(m,s,false);
}
inline VisibilityObservation ObserveVersionEffectVisibility(const RowVersionMetadata& m,const VisibilitySnapshotView& s) noexcept {
  return row_observation_detail::Evaluate(m,s,true);
}
} // namespace scratchbird::transaction::mga
