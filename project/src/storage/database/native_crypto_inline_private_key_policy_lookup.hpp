// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "physical_mga_cow_store.hpp"
#include "catalog_crypto_inline_private_key_policy.hpp"
#include "catalog_policy_descriptor.hpp"
#include "row_version_observation.hpp"
#include "uuid.hpp"
#include <new>
#include <stdexcept>

namespace scratchbird::storage::database {
struct NativeCryptoInlinePrivateKeyPolicyLookupBinding {
  core::platform::Uuid policy_uuid, policy_version_uuid;
  u64 policy_generation=0;
};
enum class NativeCryptoInlinePrivateKeyPolicyLookupError {
  none, invalid_request, source_failure, descriptor_missing, body_missing,
  ambiguous, retired, provisional, inactive, family_mismatch, class_mismatch,
  target_mismatch, version_mismatch, origin_mismatch, invisible,
  visibility_failure, snapshot_failure, resource_exhausted
};
enum class NativeCryptoInlinePrivateKeyPolicyLookupSubject { none, descriptor, body };
struct NativeCryptoInlinePrivateKeyPolicyLookupResult {
  NativeCryptoInlinePrivateKeyPolicyLookupError error=NativeCryptoInlinePrivateKeyPolicyLookupError::invalid_request;
  NativeCryptoInlinePrivateKeyPolicyLookupSubject subject=NativeCryptoInlinePrivateKeyPolicyLookupSubject::none;
  // Owns the actual verified checkpoint/inventory/relation images, including
  // retained history. Committed-only observation has no snapshot UUID/pin.
  NativePinnedCatalogReadResult source;
  std::optional<core::catalog::CatalogPolicyDescriptor> descriptor;
  std::optional<core::catalog::CatalogCryptoInlinePrivateKeyPolicy> body;
  core::platform::Uuid descriptor_physical_version_uuid, body_physical_version_uuid;
  core::catalog::CatalogValueError record_error=core::catalog::CatalogValueError::none;
  transaction::mga::MgaObservationStatus visibility_diagnostic;
  DiagnosticRecord diagnostic;
  bool ok() const noexcept {
    return error==NativeCryptoInlinePrivateKeyPolicyLookupError::none&&source.ok()&&descriptor&&body;
  }
};
namespace native_crypto_inline_private_key_policy_detail {
namespace catalog=core::catalog;
namespace mga=transaction::mga;
using Uuid=core::platform::Uuid;
using E=NativeCryptoInlinePrivateKeyPolicyLookupError;
using Subject=NativeCryptoInlinePrivateKeyPolicyLookupSubject;
inline bool ValidBinding(const Uuid& database,const NativeCryptoInlinePrivateKeyPolicyLookupBinding& b) noexcept {
  return core::uuid::IsEngineIdentityUuid(database)&&core::uuid::IsEngineIdentityUuid(b.policy_uuid)&&
      core::uuid::IsEngineIdentityUuid(b.policy_version_uuid)&&b.policy_generation&&
      b.policy_uuid!=b.policy_version_uuid&&b.policy_uuid!=database&&b.policy_version_uuid!=database;
}
inline E Active(const catalog::CatalogMetadataVersion& m,bool provisional) noexcept {
  if(m.record.header.deleted||m.lifecycle==catalog::CatalogObjectLifecycle::dropped||
      m.status==catalog::CatalogObjectStatus::retired)return E::retired;
  if(provisional)return E::provisional;
  if(m.lifecycle!=catalog::CatalogObjectLifecycle::active||m.status!=catalog::CatalogObjectStatus::active)
    return E::inactive;
  return E::none;
}
struct RetainedRow {
  const page::RowDataRecord* row=nullptr;
  const catalog::CatalogMetadataVersion* metadata=nullptr;
  const mga::TransactionInventoryEntry* creator=nullptr;
};
inline RetainedRow At(const NativePinnedCatalogReadResult& source,std::size_t n) noexcept {
  if(n>=source.source.row_creators.size())return {};
  const auto& loc=source.source.row_creators[n];
  const auto& entries=source.source.checkpoint.checkpoint_inventory.inventory.entries;
  if(loc.catalog_page_index>=source.source.relation.catalogs.size()||loc.inventory_entry_index>=entries.size())return {};
  const auto& leaf=source.source.relation.catalogs[loc.catalog_page_index];
  if(!leaf.page||loc.catalog_row_index>=leaf.page->body.rows.size())return {};
  const auto& row=leaf.page->body.rows[loc.catalog_row_index];
  const auto found=leaf.metadata.find(row.version_uuid);
  if(found==leaf.metadata.end())return {};
  return {&row,&found->second,&entries[loc.inventory_entry_index]};
}
// Identical native row/inventory projection used by the qualified visibility
// readers. State comes only from this retained checkpoint's actual inventory.
inline mga::RowVersionMetadata Version(const RetainedRow& r) noexcept {
  const auto& row=*r.row;const auto& creator=*r.creator;
  const auto state=mga::InventoryVisibilityState(creator);
  mga::RowVersionMetadata m;
  m.identity={{row.row_uuid},creator.identity,row.row_version,row.version_uuid};
  m.chain={{core::platform::UuidKind::row,row.previous_version_uuid},
      {core::platform::UuidKind::row,row.next_version_uuid},row.previous_row_version,row.next_row_version};
  m.creator_transaction_state=state;m.creator_commit_sequence=creator.commit_sequence;m.payload_present=!row.cells.empty();
  if(row.deleted)m.state=mga::RowVersionState::delete_marker;
  else switch(state){
    case mga::TransactionState::committed:m.state=mga::RowVersionState::committed;break;
    case mga::TransactionState::rolled_back:case mga::TransactionState::failed_terminal:m.state=mga::RowVersionState::rolled_back;break;
    case mga::TransactionState::prepared:m.state=mga::RowVersionState::prepared;break;
    case mga::TransactionState::limbo:m.state=mga::RowVersionState::limbo;break;
    case mga::TransactionState::recovering:m.state=mga::RowVersionState::recovery_required;break;
    case mga::TransactionState::archived:m.state=mga::RowVersionState::unknown;break;
    default:m.state=mga::RowVersionState::uncommitted;break;
  }
  return m;
}
inline NativeCryptoInlinePrivateKeyPolicyLookupResult Select(const Uuid& database,
    NativePinnedCatalogReadResult source,const NativeCryptoInlinePrivateKeyPolicyLookupBinding& binding,
    const mga::VisibilitySnapshotView& snapshot) noexcept {
  NativeCryptoInlinePrivateKeyPolicyLookupResult out;out.source=std::move(source);
  const auto fail=[&](E error,Subject subject){
    out.error=error;out.subject=subject;out.descriptor.reset();out.body.reset();
    out.descriptor_physical_version_uuid={};out.body_physical_version_uuid={};return std::move(out);
  };
  try {
    if(!ValidBinding(database,binding))return fail(E::invalid_request,Subject::none);
    if(!out.source.ok())return fail(E::source_failure,Subject::none);
    // Current visible retirement/inactivity wins before historical selection:
    // retaining an earlier true permission never resurrects a retired object.
    const auto current=[&](const Uuid& object,Subject subject)->const NativeCatalogVersionRow* {
      const NativeCatalogVersionRow* found=nullptr;
      for(const auto& r:out.source.rows)if(r.metadata.record.header.object_uuid.value==object){
        if(found){out.error=E::ambiguous;out.subject=subject;return nullptr;}found=&r;
      }
      if(!found){out.error=subject==Subject::descriptor?E::descriptor_missing:E::body_missing;out.subject=subject;return nullptr;}
      const auto active=Active(found->metadata,found->provisional);
      if(active!=E::none){out.error=active;out.subject=subject;return nullptr;}
      return found;
    };
    const auto* current_descriptor=current(binding.policy_uuid,Subject::descriptor);
    if(!current_descriptor)return fail(out.error,out.subject);
    const auto decode_descriptor=[&](const catalog::CatalogMetadataVersion& m){
      auto decoded=catalog::DecodeCatalogPolicyDescriptor(m.record.payload);
      out.record_error=decoded.error;
      if(!decoded.ok()||!catalog::CatalogPolicyDescriptorMatchesMetadata(catalog::BorrowCatalogMetadataVersion(m))||
          catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(m))){out.error=E::family_mismatch;return decoded;}
      if(decoded.record->database_uuid!=database){out.error=E::target_mismatch;return decoded;}
      if(decoded.record->policy_class!=catalog::CatalogPolicyClass::authorization){out.error=E::class_mismatch;return decoded;}
      out.error=E::none;return decoded;
    };
    if(!decode_descriptor(current_descriptor->metadata).ok()||out.error!=E::none)return fail(out.error,Subject::descriptor);
    const NativeCatalogVersionRow* current_body=nullptr;
    const auto choose=[&](const Uuid& object,Subject subject,u64 generation,const Uuid* policy_version)->E {
      bool invisible=false,noncommitted=false;
      RetainedRow latest;
      std::size_t exact_visible_count=0;
      const auto* current_object=subject==Subject::descriptor?current_descriptor:current_body;
      const page::RowDataRecord* current_row=nullptr;
      for(std::size_t i=0;i<out.source.source.row_creators.size();++i){
        const auto retained=At(out.source,i);if(!retained.row)return E::source_failure;
        if(retained.row->version_uuid==current_object->version_uuid){if(current_row)return E::ambiguous;current_row=retained.row;}
      }
      if(!current_row)return E::source_failure;
      for(std::size_t i=0;i<out.source.source.row_creators.size();++i){
        const auto retained=At(out.source,i);
        if(!retained.row)return E::source_failure;
        const auto& m=*retained.metadata;
        if(m.record.header.object_uuid.value!=object)continue;
        bool exact=false;
        if(subject==Subject::descriptor){
          const auto decoded=decode_descriptor(m);if(!decoded.ok()||out.error!=E::none)return out.error;
          exact=decoded.record->generation==generation&&decoded.record->policy_version_uuid==*policy_version;
          const auto a=catalog::BorrowCatalogMetadataVersion(m),b=catalog::BorrowCatalogMetadataVersion(current_descriptor->metadata);
          if(!catalog::CatalogPolicyDescriptorPreservesOrigin(m.definition_version<=current_descriptor->metadata.definition_version?a:b,
              m.definition_version<=current_descriptor->metadata.definition_version?b:a))return E::origin_mismatch;
        }else{
          const auto decoded=catalog::DecodeCatalogCryptoInlinePrivateKeyPolicy(m.record.payload);out.record_error=decoded.error;
          if(!decoded.ok()||!catalog::CatalogCryptoInlinePrivateKeyPolicyMatchesMetadata(m)||
              catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(m)))return E::family_mismatch;
          if(decoded.record->database_uuid!=database)return E::target_mismatch;
          exact=decoded.record->generation==generation;
          const auto a=catalog::BorrowCatalogMetadataVersion(m),b=catalog::BorrowCatalogMetadataVersion(current_body->metadata);
          if(!catalog::CatalogCryptoInlinePrivateKeyPolicyPreservesOrigin(m.definition_version<=current_body->metadata.definition_version?a:b,
              m.definition_version<=current_body->metadata.definition_version?b:a))return E::origin_mismatch;
        }
        if(!exact)continue;
        const auto visible=mga::ObserveVersionEffectVisibility(Version(retained),snapshot);
        if(visible.decision==mga::VisibilityDecision::requires_recovery||
            (!visible.ok()&&visible.decision!=mga::VisibilityDecision::wait_for_transaction)){
          out.visibility_diagnostic=visible.outcome;return E::visibility_failure;
        }
        if(visible.decision!=mga::VisibilityDecision::visible){
          invisible=true;if(visible.decision==mga::VisibilityDecision::wait_for_transaction){out.visibility_diagnostic=visible.outcome;noncommitted=true;}
          continue;
        }
        const auto active=Active(m,!mga::HasCommittedInventoryOutcome(*retained.creator));
        if(active!=E::none)return active;
        if(!core::uuid::IsEngineIdentityUuid(retained.row->version_uuid))return E::source_failure;
        if(retained.row->row_uuid.value!=current_row->row_uuid.value||
            retained.row->row_version>current_row->row_version)return E::ambiguous;
        ++exact_visible_count;
        if(latest.row){
          if(latest.metadata->record.payload!=m.record.payload)return E::ambiguous;
          if(retained.row->row_version==latest.row->row_version)return E::ambiguous;
          if(retained.row->row_version<latest.row->row_version)continue;
        }
        // Retain only the greatest exact-visible physical sequence here.
        // The single chain walk below must prove ALL counted candidates.
        latest=retained;
      }
      if(!latest.row)return noncommitted?E::visibility_failure:invisible?E::invisible:E::version_mismatch;
      // Prove ancestry once per object, not once per candidate. All exact
      // candidates already passed family/origin/visibility/byte validation.
      // Counting their canonical payload on this one strictly decreasing
      // stable chain detects off-chain forks and duplicate physical candidates
      // without a second heap index or cubic repeated ancestry scans.
      std::size_t exact_visible_on_chain=0;
      const auto* chain_row=current_row;
      for(std::size_t steps=0;steps<out.source.source.row_creators.size();++steps){
        RetainedRow member;
        for(std::size_t i=0;i<out.source.source.row_creators.size();++i){
          const auto retained=At(out.source,i);if(!retained.row)return E::source_failure;
          if(retained.row->version_uuid!=chain_row->version_uuid)continue;
          if(member.row)return E::ambiguous;member=retained;
        }
        if(!member.row||member.row->row_uuid.value!=current_row->row_uuid.value||
            member.metadata->record.header.object_uuid.value!=object)return E::ambiguous;
        if(member.metadata->record.payload==latest.metadata->record.payload){
          const auto visible=mga::ObserveVersionEffectVisibility(Version(member),snapshot);
          if(visible.decision==mga::VisibilityDecision::visible)++exact_visible_on_chain;
        }
        // Predecessors below the oldest required candidate may legitimately
        // be retention-pruned. Do not require or manufacture those versions.
        if(exact_visible_on_chain==exact_visible_count)break;
        if(!chain_row->previous_row_version)return E::ambiguous;
        const page::RowDataRecord* previous=nullptr;
        for(std::size_t i=0;i<out.source.source.row_creators.size();++i){
          const auto retained=At(out.source,i);if(!retained.row)return E::source_failure;
          if(retained.row->version_uuid!=chain_row->previous_version_uuid)continue;
          if(previous)return E::ambiguous;previous=retained.row;
        }
        if(!previous||previous->row_uuid.value!=chain_row->row_uuid.value||
            previous->row_version!=chain_row->previous_row_version||previous->row_version>=chain_row->row_version)
          return E::ambiguous;
        chain_row=previous;
      }
      if(exact_visible_on_chain!=exact_visible_count)return E::ambiguous;
      // Identical semantic definitions may have multiple metadata-only
      // physical successors; issue the newest actual visible physical UUID.
      if(subject==Subject::descriptor){
        out.descriptor=*catalog::DecodeCatalogPolicyDescriptor(latest.metadata->record.payload).record;
        out.descriptor_physical_version_uuid=latest.row->version_uuid;
      }else{
        out.body=*catalog::DecodeCatalogCryptoInlinePrivateKeyPolicy(latest.metadata->record.payload).record;
        out.body_physical_version_uuid=latest.row->version_uuid;
      }
      return E::none;
    };
    auto selected=choose(binding.policy_uuid,Subject::descriptor,binding.policy_generation,&binding.policy_version_uuid);
    if(selected!=E::none)return fail(selected,Subject::descriptor);
    current_body=current(out.descriptor->body_uuid,Subject::body);
    if(!current_body)return fail(out.error,out.subject);
    selected=choose(out.descriptor->body_uuid,Subject::body,out.descriptor->body_generation,nullptr);
    if(selected!=E::none)return fail(selected,Subject::body);
    // Immutable origins must exist and be committed in the same retained
    // inventory; a current creator or a caller boolean is not that evidence.
    const auto origin=[&](const core::platform::TypedUuid& id,u64 number)->bool {
      const auto& inventory=out.source.source.checkpoint.checkpoint_inventory.inventory;
      const auto found=mga::LookupLocalTransaction(inventory,mga::MakeLocalTransactionId(number));
      if(!found.ok()){out.diagnostic=found.diagnostic;return false;}
      return found.entry.identity.transaction_uuid.kind==id.kind&&found.entry.identity.transaction_uuid.value==id.value&&
          found.entry.identity.scope==mga::TransactionScope::local_node&&
          mga::HasCommittedInventoryOutcome(found.entry);
    };
    if(!origin(out.descriptor->origin_transaction_uuid,out.descriptor->origin_local_transaction_id))return fail(E::origin_mismatch,Subject::descriptor);
    if(!origin(out.body->origin_transaction_uuid,out.body->origin_local_transaction_id))return fail(E::origin_mismatch,Subject::body);
    out.error=E::none;out.subject=Subject::none;return out;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted,Subject::none);}
  catch(const std::length_error&){return fail(E::resource_exhausted,Subject::none);}
}
} // namespace native_crypto_inline_private_key_policy_detail

// Real selector2/security role4 only. Relation is an exact physical binding,
// not permission to choose an arbitrary catalog role. Caller retains actual
// open-device custody and the pin through this synchronous read; returned
// images/values are observations, NOT a current source/admission lease.
// No .ok(), true field, effective transaction number, or observed epoch grants
// EXECUTE, scope applicability, activation, private-key use or publication.
inline NativeCryptoInlinePrivateKeyPolicyLookupResult ReadNativeCryptoInlinePrivateKeyPolicyFromOpenDevices(
    const core::platform::Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& checkpoint,const NativeCatalogRelationBinding& security_relation,
    const transaction::mga::TransactionIdentity& reader,const transaction::mga::PublishedSnapshotPin& pin,
    const NativeCryptoInlinePrivateKeyPolicyLookupBinding& binding,u64 budget) noexcept {
  using namespace native_crypto_inline_private_key_policy_detail;
  try {
    if(!budget||!ValidBinding(database,binding))return {};
    const auto captured=pin.Resolve();
    if(!captured.ok()){NativeCryptoInlinePrivateKeyPolicyLookupResult r;r.error=E::snapshot_failure;r.diagnostic=captured.diagnostic;return r;}
    const auto& d=captured.descriptor;
    mga::VisibilitySnapshotView visibility;
    visibility.reader_transaction=reader.local_id;visibility.visible_through_local_transaction_id=d.visible_committed_high_watermark;
    visibility.visible_through_local_transaction_id_is_boundary=true;
    visibility.active_excluded_local_transaction_ids=d.active_excluded_local_transaction_ids;
    visibility.in_doubt_excluded_local_transaction_ids=d.in_doubt_excluded_local_transaction_ids;
    // SnapshotVectorDescriptor deliberately carries no commit-sequence field.
    // Its qualified publisher captures committed IDs beyond the real commit
    // boundary in these immutable exclusions, including lower local IDs.
    // Preserve them exactly; never reconstruct a newer boundary from inventory.
    auto source=ReadNativePinnedCatalogVersionsFromOpenDevices(database,devices,checkpoint,2,4,security_relation,reader,pin,budget);
    auto result=Select(database,std::move(source),binding,visibility);
    if(result.ok()){
      const auto final=pin.Resolve();
      if(!final.ok()||final.descriptor.snapshot_uuid.value!=result.source.snapshot_uuid){
        result.error=E::snapshot_failure;result.diagnostic=final.diagnostic;result.descriptor.reset();result.body.reset();
        result.descriptor_physical_version_uuid={};result.body_physical_version_uuid={};
      }
    }
    return result;
  }catch(const std::bad_alloc&){NativeCryptoInlinePrivateKeyPolicyLookupResult r;r.error=E::resource_exhausted;return r;}
  catch(const std::length_error&){NativeCryptoInlinePrivateKeyPolicyLookupResult r;r.error=E::resource_exhausted;return r;}
}
// Construction/reopen observation uses actual committed inventory boundaries;
// it creates no active reader, pin, snapshot identity or admission authority.
inline NativeCryptoInlinePrivateKeyPolicyLookupResult ReadNativeCommittedCryptoInlinePrivateKeyPolicyFromOpenDevices(
    const core::platform::Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& checkpoint,const NativeCatalogRelationBinding& security_relation,
    const NativeCryptoInlinePrivateKeyPolicyLookupBinding& binding,u64 budget) noexcept {
  using namespace native_crypto_inline_private_key_policy_detail;
  if(!budget||!ValidBinding(database,binding))return {};
  auto committed=ReadNativeCommittedCatalogVersionsFromOpenDevices(database,devices,checkpoint,2,4,security_relation,budget);
  NativePinnedCatalogReadResult source;source.error=committed.error;source.source=std::move(committed.source);
  source.rows=std::move(committed.rows);source.observations=std::move(committed.observations);source.diagnostic=std::move(committed.diagnostic);
  mga::VisibilitySnapshotView visibility;visibility.allow_reader_own_uncommitted=false;
  if(source.ok()){
    const auto& inventory=source.source.checkpoint.checkpoint_inventory.inventory;
    visibility.visible_through_local_transaction_id=inventory.next_local_transaction_id-1;
    visibility.visible_through_local_transaction_id_is_boundary=true;
    visibility.visible_through_commit_sequence=inventory.next_commit_sequence-1;
    visibility.visible_through_commit_sequence_is_boundary=true;
  }
  return Select(database,std::move(source),binding,visibility);
}
} // namespace scratchbird::storage::database
