// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_relation_lease_reader.hpp"
#include "row_version_observation.hpp"
#include "catalog_name_envelope.hpp"
#include <numeric>

namespace scratchbird::storage::database {
struct NativeCatalogVisibleRow {
  core::catalog::CatalogMetadataVersionView metadata;
  Uuid version_uuid,previous_version_uuid;
  std::optional<core::catalog::CatalogNamePayloadView> name_payload;
  bool provisional=false;
  core::catalog::CatalogObjectLifecycle effective_lifecycle=core::catalog::CatalogObjectLifecycle::creating;
  core::catalog::CatalogObjectStatus effective_status=core::catalog::CatalogObjectStatus::proposed;
};
inline usize NativeCatalogVisibilityWorkspaceBytes(const NativeCatalogRelationMemoryLimits& l) noexcept {
  usize bytes=NativeCatalogRelationWorkspaceBytes(l);if(!bytes)return 0;
  constexpr auto max=std::numeric_limits<usize>::max();
  const auto add=[&]<class T>(usize n){const auto pad=alignof(T)-1;
    if(n>(max-pad)/sizeof(T)||n*sizeof(T)+pad>max-bytes)return false;
    bytes+=n*sizeof(T)+pad;return true;};
  if(!add.template operator()<usize>(l.maximum_rows)||!add.template operator()<usize>(l.maximum_rows)||
     !add.template operator()<usize>(l.maximum_rows)||!add.template operator()<bool>(l.maximum_rows)||
     !add.template operator()<NativeCatalogVisibleRow>(l.maximum_rows)||
     !add.template operator()<NativeCatalogVisibilityObservation>(l.maximum_rows))return 0;
  return bytes;
}
class NativeCatalogVisibilityLeaseReader;
struct NativeCatalogVisibilityPreparedResult;
class NativeCatalogVisibilityPreparedMemory {
 public:
  NativeCatalogVisibilityPreparedMemory() noexcept=default;
  NativeCatalogVisibilityPreparedMemory(const NativeCatalogVisibilityPreparedMemory&)=delete;
  NativeCatalogVisibilityPreparedMemory& operator=(const NativeCatalogVisibilityPreparedMemory&)=delete;
  NativeCatalogVisibilityPreparedMemory(NativeCatalogVisibilityPreparedMemory&&) noexcept=default;
  NativeCatalogVisibilityPreparedMemory& operator=(NativeCatalogVisibilityPreparedMemory&&) noexcept=default;
  explicit operator bool() const noexcept{return bool(arena_)&&bool(relation_);}
 private:
  NativeStorageArena arena_;
  NativeCatalogRelationPreparedMemory relation_;
  std::span<usize> row_order_,version_order_,object_order_;
  std::span<bool> candidates_;
  std::span<NativeCatalogVisibleRow> rows_;
  std::span<NativeCatalogVisibilityObservation> observations_;
  friend class NativeCatalogVisibilityLeaseReader;
  friend NativeCatalogVisibilityPreparedResult PrepareNativeCatalogVisibilityRead(
    const NativeCatalogRelationMemoryLimits&,NativeStorageMemory&,const NativeStorageMemoryBinding&) noexcept;
};
struct NativeCatalogVisibilityPreparedResult {
  NativeCatalogRelationLeaseError error=NativeCatalogRelationLeaseError::invalid_request;
  NativeStorageMemoryError memory_error=NativeStorageMemoryError::none;
  core::platform::Status allocation_status;
  core::platform::DiagnosticRecord allocation_diagnostic;
  NativeCatalogVisibilityPreparedMemory workspace;
  bool ok() const noexcept{return error==NativeCatalogRelationLeaseError::none&&bool(workspace);}
};
inline NativeCatalogVisibilityPreparedResult PrepareNativeCatalogVisibilityRead(
    const NativeCatalogRelationMemoryLimits& l,NativeStorageMemory& memory,const NativeStorageMemoryBinding& binding) noexcept {
  using E=NativeCatalogRelationLeaseError;NativeCatalogVisibilityPreparedResult out;
  try{
    const auto bytes=NativeCatalogVisibilityWorkspaceBytes(l);if(!bytes)return out;
    NativeCatalogVisibilityPreparedMemory w;
    auto allocation=memory.CreateArena(binding,bytes-NativeCatalogRelationWorkspaceBytes(l),alignof(std::max_align_t));
    if(!allocation.ok()){out.error=E::memory_failure;out.memory_error=allocation.error;
      out.allocation_status=allocation.backing.status;out.allocation_diagnostic=std::move(allocation.backing.diagnostic);return out;}
    w.arena_=std::move(allocation.arena);
    const auto array=[&]<class T>()->std::span<T>{static_assert(std::is_trivially_destructible_v<T>);
      if(!l.maximum_rows)return {};auto a=w.arena_.Allocate(l.maximum_rows*sizeof(T),alignof(T));
      if(!a.ok())throw std::bad_alloc();auto* p=static_cast<T*>(a.pointer);
      std::uninitialized_value_construct_n(p,l.maximum_rows);return {p,l.maximum_rows};};
    w.row_order_=array.template operator()<usize>();w.version_order_=array.template operator()<usize>();
    w.object_order_=array.template operator()<usize>();w.candidates_=array.template operator()<bool>();
    w.rows_=array.template operator()<NativeCatalogVisibleRow>();w.observations_=array.template operator()<NativeCatalogVisibilityObservation>();
    auto relation=PrepareNativeCatalogRelationRead(l,memory,binding);
    if(!relation.ok()){out.error=relation.error;out.memory_error=relation.memory_error;out.allocation_status=relation.allocation_status;
      out.allocation_diagnostic=std::move(relation.allocation_diagnostic);return out;}
    w.relation_=std::move(relation.workspace);out.workspace=std::move(w);out.error=E::none;
  }catch(const std::bad_alloc&){out.error=E::resource_exhausted;}
   catch(const std::length_error&){out.error=E::resource_exhausted;}
   catch(const std::system_error&){out.error=E::lock_failure;}
  return out;
}
struct NativeCatalogVisibilityLeaseResult {
  NativePinnedCatalogReadError error=NativePinnedCatalogReadError::invalid_reader;
  NativeCatalogRelationLeaseResult source;
  Uuid snapshot_uuid;
  std::span<const NativeCatalogVisibleRow> rows;
  std::span<const NativeCatalogVisibilityObservation> observations;
  transaction::mga::MgaObservationStatus visibility_diagnostic;
  transaction::mga::PublishedSnapshotObservation snapshot_diagnostic;
  bool ok() const noexcept{return error==NativePinnedCatalogReadError::none&&source.ok();}
};
// Actual source and prepared memory are required; caller-built relation views
// never grant authority. Retain source/pin/backing throughout borrowed use.
class NativeCatalogVisibilityLeaseReader {
 public:
  static NativeCatalogVisibilityLeaseResult ReadCommitted(const NativeSelectedCheckpointMemoryLease& source,
      u16 selector,u16 role,const NativeCatalogRelationBinding& binding,u64 image_budget,
      NativeCatalogVisibilityPreparedMemory& w) noexcept {
    return Read(source,selector,role,binding,nullptr,nullptr,image_budget,w);
  }
  static NativeCatalogVisibilityLeaseResult ReadPinned(const NativeSelectedCheckpointMemoryLease& source,
      u16 selector,u16 role,const NativeCatalogRelationBinding& binding,
      const transaction::mga::TransactionIdentity& reader,const transaction::mga::PublishedSnapshotPin& pin,
      u64 image_budget,NativeCatalogVisibilityPreparedMemory& w) noexcept {
    return Read(source,selector,role,binding,&reader,&pin,image_budget,w);
  }
 private:
  static NativeCatalogVisibilityLeaseResult Read(const NativeSelectedCheckpointMemoryLease& source,
      u16 selector,u16 role,const NativeCatalogRelationBinding& binding,
      const transaction::mga::TransactionIdentity* reader,const transaction::mga::PublishedSnapshotPin* pin,
      u64 image_budget,NativeCatalogVisibilityPreparedMemory& w) noexcept {
    namespace mga=transaction::mga;namespace catalog=core::catalog;
    using E=NativePinnedCatalogReadError;using core::platform::UuidKind;NativeCatalogVisibilityLeaseResult out;
    const auto fail=[&](E e){out.error=e;out.rows={};out.observations={};out.snapshot_uuid={};
      auto& s=out.source;s.roots.catalogs={};s.roots.retained_image_bytes=0;s.roots.feature_root_index=0;
      s.tree.pages={};s.tree.leaves={};s.tree.retained_image_bytes=0;s.bases={};s.bindings={};
      s.row_creators={};s.navigation_creators={};s.retained_image_bytes=0;};
    try{
      if(!w)return out;
      mga::PublishedSnapshotObservation captured;
      if(pin){
        if(!reader||!mga::ObserveTransactionIdentity(*reader).ok()||reader->scope!=mga::TransactionScope::local_node)return out;
        captured=pin->Observe();if(!captured.ok()){out.snapshot_diagnostic=captured;fail(E::snapshot_failure);return out;}
        if(captured.descriptor->owning_transaction_uuid.value!=reader->transaction_uuid.value||
            captured.descriptor->owning_transaction.value!=reader->local_id.value){fail(E::reader_mismatch);return out;}
      }
      out.source=NativeCatalogRelationLeaseReader::Read(source,selector,role,binding,image_budget,w.relation_);
      if(!out.source.ok()){fail(E::source_failure);return out;}
      const auto& inventory=source.selection().checkpoint_inventory.inventory;
      mga::VisibilitySnapshotView snapshot;
      if(pin){const auto& s=*captured.descriptor;
        const auto owner=std::lower_bound(inventory.entries.begin(),inventory.entries.end(),reader->local_id.value,
          [](const auto& e,u64 n){return e.identity.local_id.value<n;});
        if(owner==inventory.entries.end()||owner->identity.local_id.value!=reader->local_id.value||
          owner->identity.transaction_uuid.value!=reader->transaction_uuid.value||owner->identity.scope!=reader->scope||
          (owner->state!=mga::TransactionState::active&&owner->state!=mga::TransactionState::read_only_active)||
          inventory.next_local_transaction_id<s.publication_inventory_next_local_transaction_id){fail(E::reader_mismatch);return out;}
        snapshot.reader_transaction=reader->local_id;snapshot.visible_through_local_transaction_id=s.visible_committed_high_watermark;
        snapshot.visible_through_local_transaction_id_is_boundary=true;
        snapshot.active_excluded_local_transaction_ids=s.active_excluded_local_transaction_ids;
        snapshot.in_doubt_excluded_local_transaction_ids=s.in_doubt_excluded_local_transaction_ids;
      }else{
        if(source.selection().checkpoint_inventory.checkpoint->flags&4){out.source.error=NativeCatalogRelationLeaseError::cluster_requires_authority;fail(E::source_failure);return out;}
        for(const auto n:out.source.navigation_creators)if(!mga::HasCommittedInventoryOutcome(inventory.entries[n])){fail(E::navigation_not_committed);return out;}
        snapshot.allow_reader_own_uncommitted=false;snapshot.visible_through_local_transaction_id=inventory.next_local_transaction_id-1;
        snapshot.visible_through_local_transaction_id_is_boundary=true;snapshot.visible_through_commit_sequence=inventory.next_commit_sequence-1;
        snapshot.visible_through_commit_sequence_is_boundary=true;
      }
      const auto sources=out.source.row_creators;const auto count=sources.size();
      if(count>w.rows_.size()){fail(E::resource_exhausted);return out;}
      const auto row=[&](usize n)->const page::RowDataRecordView&{const auto& b=sources[n];return out.source.bases[b.catalog_page_index].page.body.rows[b.catalog_row_index];};
      const auto meta=[&](usize n)->const catalog::CatalogMetadataVersionView&{const auto& a=out.source.bases[sources[n].catalog_page_index].page.metadata;
        return std::lower_bound(a.begin(),a.end(),row(n).version_uuid,[](const auto& m,const auto& id){return m.version_uuid<id;})->metadata;};
      const auto creator=[&](usize n)->const auto&{return inventory.entries[sources[n].inventory_entry_index];};
      const auto version=[&](usize n){const auto& r=row(n);const auto& c=creator(n);const auto state=mga::InventoryVisibilityState(c);
        mga::RowVersionMetadata m;m.identity={ {r.row_uuid},c.identity,r.row_version,r.version_uuid};
        m.chain={{UuidKind::row,r.previous_version_uuid},{UuidKind::row,r.next_version_uuid},r.previous_row_version,r.next_row_version};
        m.creator_transaction_state=state;m.creator_commit_sequence=c.commit_sequence;m.payload_present=!r.cells.empty();
        if(r.deleted)m.state=mga::RowVersionState::delete_marker;
        else switch(state){
          case mga::TransactionState::committed:m.state=mga::RowVersionState::committed;break;
          case mga::TransactionState::rolled_back:case mga::TransactionState::failed_terminal:m.state=mga::RowVersionState::rolled_back;break;
          case mga::TransactionState::prepared:m.state=mga::RowVersionState::prepared;break;
          case mga::TransactionState::limbo:m.state=mga::RowVersionState::limbo;break;
          case mga::TransactionState::recovering:m.state=mga::RowVersionState::recovery_required;break;
          case mga::TransactionState::archived:m.state=mga::RowVersionState::unknown;break;
          default:m.state=mga::RowVersionState::uncommitted;break;
        }return m;};
      const auto name=[&](usize n){const auto& r=row(n);const auto& m=meta(n);const auto& p=out.source.bases[sources[n].catalog_page_index].page;
        catalog::CatalogNameVersionBinding b{{UuidKind::database,p.header.database_uuid},{UuidKind::filespace,p.header.filespace_uuid},
          r.row_uuid,{UuidKind::row,r.version_uuid},m.record.header.object_uuid,r.transaction_uuid,p.body.page_number,r.stable_slot_id,
          r.storage_generation,r.row_version,r.local_transaction_id,m.catalog_generation};
        auto decoded=catalog::DecodeCatalogNameEnvelopeView({reinterpret_cast<const byte*>(m.record.payload.data()),m.record.payload.size()},b);
        if(decoded.ok()&&!catalog::CatalogNamePayloadMatchesMetadata(decoded.record->payload,m))decoded={catalog::CatalogNameEnvelopeError::binding_mismatch,{}};
        return decoded;};
      auto rows=w.row_order_.first(count),versions=w.version_order_.first(count),objects=w.object_order_.first(count);
      std::iota(rows.begin(),rows.end(),usize{0});std::iota(versions.begin(),versions.end(),usize{0});std::iota(objects.begin(),objects.end(),usize{0});
      std::fill(w.candidates_.begin(),w.candidates_.end(),false);
      std::sort(rows.begin(),rows.end(),[&](usize a,usize b){return row(a).row_uuid.value==row(b).row_uuid.value?
        row(a).row_version>row(b).row_version:row(a).row_uuid.value<row(b).row_uuid.value;});
      std::sort(versions.begin(),versions.end(),[&](usize a,usize b){return row(a).version_uuid<row(b).version_uuid;});
      std::sort(objects.begin(),objects.end(),[&](usize a,usize b){return meta(a).record.header.object_uuid.value<meta(b).record.header.object_uuid.value;});
      constexpr auto absent=std::numeric_limits<usize>::max();
      const auto previous=[&](usize n){const auto& id=row(n).previous_version_uuid;
        const auto found=std::lower_bound(versions.begin(),versions.end(),id,[&](usize i,const Uuid& u){return row(i).version_uuid<u;});
        return found!=versions.end()&&row(*found).version_uuid==id?*found:absent;};
      usize reservation=absent;
      for(usize i=0;i<count;++i){const auto validated=mga::ObserveRowVersionMetadata(version(i));
        if(!validated.ok()){out.visibility_diagnostic=validated;fail(E::visibility_failure);return out;}}
      for(const auto i:objects){const auto state=mga::InventoryVisibilityState(creator(i));
        if(state==mga::TransactionState::rolled_back||state==mga::TransactionState::failed_terminal)continue;
        if(reservation!=absent&&meta(i).record.header.object_uuid.value==meta(reservation).record.header.object_uuid.value&&
           meta(i).record.header.row_uuid.value!=meta(reservation).record.header.row_uuid.value){fail(E::duplicate_identity);return out;}reservation=i;}
      for(usize n=0;n<count;++n){const auto i=rows[n];const auto& r=row(i);const auto& m=meta(i);
        if(n&&row(rows[n-1]).row_uuid.value==r.row_uuid.value){const auto& prior=meta(rows[n-1]);
          if(row(rows[n-1]).row_version==r.row_version||prior.record.header.object_uuid.value!=m.record.header.object_uuid.value||
             prior.record.header.kind!=m.record.header.kind){fail(E::invalid_chain);return out;}}
        if(!r.previous_row_version)continue;
        const auto p=previous(i);
        if(p!=absent){const auto& before=meta(p);
          if(row(p).row_uuid.value!=r.row_uuid.value||row(p).row_version!=r.previous_row_version){fail(E::invalid_chain);return out;}
          if(m.record.header.kind==catalog::CatalogRecordKind::localized_name){const auto a=name(p),b=name(i);
            if(!a.ok()||!b.ok()||!catalog::CatalogNamePayloadPreservesIdentity(a.record->payload,b.record->payload)){fail(E::invalid_chain);return out;}}
          if(!catalog::CatalogMetadataPreservesFamilyOrigin(before,m)||before.definition_version==std::numeric_limits<u64>::max()||
            m.definition_version!=before.definition_version+1||m.schema_epoch<before.schema_epoch||m.security_epoch<before.security_epoch||
            m.resource_epoch<before.resource_epoch||m.catalog_generation<before.catalog_generation||m.dependency_generation<before.dependency_generation||
            m.invalidation_generation<before.invalidation_generation){fail(E::invalid_chain);return out;}
        }
        // Even an absent declared UUID cannot disagree with an actually retained sequence.
        const auto seq=std::lower_bound(rows.begin(),rows.end(),i,[&](usize a,usize b){return row(a).row_uuid.value==row(b).row_uuid.value?
          row(a).row_version>row(b).previous_row_version:row(a).row_uuid.value<row(b).row_uuid.value;});
        if(seq!=rows.end()&&row(*seq).row_uuid.value==r.row_uuid.value&&row(*seq).row_version==r.previous_row_version&&
          row(*seq).version_uuid!=r.previous_version_uuid){fail(E::invalid_chain);return out;}
      }
      for(const auto& bound:out.source.bindings){const auto found=std::lower_bound(sources.begin(),sources.end(),bound,[](const auto& a,const auto& b){
        return a.catalog_page_index==b.catalog_page_index?a.catalog_row_index<b.catalog_row_index:a.catalog_page_index<b.catalog_page_index;});
        if(found==sources.end()||found->catalog_page_index!=bound.catalog_page_index||found->catalog_row_index!=bound.catalog_row_index){fail(E::invalid_chain);return out;}
        w.candidates_[found-sources.begin()]=true;}
      usize selected=0,observed=0;
      for(usize begin=0;begin<count;){usize end=begin+1;bool candidate=w.candidates_[rows[begin]];
        while(end<count&&row(rows[end]).row_uuid.value==row(rows[begin]).row_uuid.value){candidate|=w.candidates_[rows[end]];++end;}
        if(candidate)for(usize n=begin;n<end;++n){const auto i=rows[n];const auto decision=mga::ObserveVersionEffectVisibility(version(i),snapshot);
          if(decision.decision==mga::VisibilityDecision::requires_recovery){out.visibility_diagnostic=decision.outcome;fail(E::requires_recovery);return out;}
          if(!decision.ok()&&decision.decision!=mga::VisibilityDecision::wait_for_transaction){out.visibility_diagnostic=decision.outcome;fail(E::visibility_failure);return out;}
          w.observations_[observed++]={i,decision.decision};
          if(decision.decision!=mga::VisibilityDecision::visible){if(row(i).previous_row_version&&previous(i)==absent){fail(E::missing_version);return out;}continue;}
          const auto& m=meta(i);NativeCatalogVisibleRow value{m,row(i).version_uuid,row(i).previous_version_uuid,{},!mga::HasCommittedInventoryOutcome(creator(i)),m.lifecycle,m.status};
          if(m.record.header.kind==catalog::CatalogRecordKind::localized_name){const auto decoded=name(i);if(!decoded.ok()){fail(E::invalid_chain);return out;}value.name_payload=decoded.record->payload;}
          if(value.provisional){value.effective_status=catalog::CatalogObjectStatus::proposed;value.effective_lifecycle=m.record.header.deleted?
            catalog::CatalogObjectLifecycle::dropping:m.definition_version==1?catalog::CatalogObjectLifecycle::creating:catalog::CatalogObjectLifecycle::altering;}
          w.rows_[selected++]=value;break;
        }begin=end;}
      if(pin){const auto final=pin->Observe();if(!final.ok()){out.snapshot_diagnostic=final;fail(E::snapshot_failure);return out;}
        out.snapshot_uuid=captured.descriptor->snapshot_uuid.value;}
      out.rows=w.rows_.first(selected);out.observations=w.observations_.first(observed);out.error=E::none;return out;
    }catch(const std::bad_alloc&){fail(E::resource_exhausted);}
     catch(const std::length_error&){fail(E::resource_exhausted);}
     catch(...){fail(E::io_failure);}
    return out;
  }
};
} // namespace scratchbird::storage::database
