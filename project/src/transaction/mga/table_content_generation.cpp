// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "table_content_generation.hpp"
#include <algorithm>
#include <iterator>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace scratchbird::transaction::mga {
namespace {
using Code = TableContentGenerationSelectionStatus;
using Id = ContentGenerationUuid;
bool Uuid(const Id& id) { return (id[6]&0xf0)==0x70 && (id[8]&0xc0)==0x80; }
bool Nil(const Id& id) {
  return std::all_of(id.begin(),id.end(),[](auto byte){return byte==0;});
}
TableContentGenerationSelection Fail(Code code) {
  TableContentGenerationSelection result; result.status=code; return result;
}
RowVersionState RowState(TransactionState state) {
  switch(state) {
    case TransactionState::committed:
    case TransactionState::archived: return RowVersionState::committed;
    case TransactionState::rolled_back:
    case TransactionState::failed_terminal: return RowVersionState::rolled_back;
    case TransactionState::prepared: return RowVersionState::prepared;
    case TransactionState::limbo: return RowVersionState::limbo;
    case TransactionState::recovering: return RowVersionState::recovery_required;
    case TransactionState::created:
    case TransactionState::active:
    case TransactionState::preparing:
    case TransactionState::committing:
    case TransactionState::rolling_back:
    case TransactionState::read_only_active: return RowVersionState::uncommitted;
    case TransactionState::none: return RowVersionState::unknown;
  }
  return RowVersionState::unknown;
}
using InventoryIndex = std::map<u64,const TransactionInventoryEntry*>;
const TransactionInventoryEntry* Creator(const InventoryIndex& entries,u64 local,const Id& uuid) {
  const auto found=entries.find(local);
  return found!=entries.end() && found->second->identity.transaction_uuid.value.bytes==uuid
      ? found->second : nullptr;
}
bool Fields(const TableContentGenerationVersion& v,const Id& database) {
  return v.database_uuid==database && Uuid(v.schema_uuid) && Uuid(v.table_uuid) &&
    Uuid(v.catalog_row_uuid) && Uuid(v.generation_uuid) && Uuid(v.root_set_uuid) &&
    Uuid(v.statistics_generation_uuid) && Uuid(v.batch_uuid) &&
    Uuid(v.creator_transaction_uuid) && v.creator_local_transaction_id!=0 &&
    v.publication_effect_sequence!=0 && v.descriptor_generation!=0 &&
    v.batch_target_count>=1 && v.batch_target_count<=65535 &&
    v.batch_ordinal<v.batch_target_count &&
    (Nil(v.predecessor_generation_uuid) || Uuid(v.predecessor_generation_uuid));
}
}  // namespace

TableContentGenerationSelection ResolveTableContentGeneration(
    const TableContentGenerationReadContext& context,
    const std::vector<TableContentGenerationVersion>& history,
    const std::vector<TableContentGenerationRollbackInterval>& rollbacks,
    const LocalTransactionInventory& inventory) noexcept {
  try {
    if(!Uuid(context.database_uuid) || !Uuid(context.table_uuid) ||
       !context.snapshot.visible_through_local_transaction_id_is_boundary ||
       (context.snapshot.reader_transaction.valid() ? !Uuid(context.reader_transaction_uuid)
                                                    : !Nil(context.reader_transaction_uuid)))
      return Fail(Code::invalid_request);
    if(inventory.next_local_transaction_id==0 ||
       context.snapshot.visible_through_local_transaction_id>=inventory.next_local_transaction_id)
      return Fail(Code::invalid_request);
    InventoryIndex entries;
    std::set<Id> transaction_ids;
    for(const auto& entry:inventory.entries) {
      if(!entry.identity.local_id.valid() || entry.identity.local_id.value>=inventory.next_local_transaction_id ||
         entry.identity.transaction_uuid.kind!=UuidKind::transaction ||
         !Uuid(entry.identity.transaction_uuid.value.bytes) ||
         RowState(entry.state)==RowVersionState::unknown ||
         (entry.identity.scope!=TransactionScope::local_node &&
          entry.identity.scope!=TransactionScope::cluster_global) ||
         !entries.emplace(entry.identity.local_id.value,&entry).second ||
         !transaction_ids.insert(entry.identity.transaction_uuid.value.bytes).second)
        return Fail(Code::inventory_required);
    }
    if(context.snapshot.reader_transaction.valid()) {
      const auto* reader=Creator(entries,context.snapshot.reader_transaction.value,context.reader_transaction_uuid);
      if(!reader) return Fail(Code::inventory_required);
      if(reader->identity.scope!=TransactionScope::local_node) return Fail(Code::cluster_authority_required);
    }

    std::map<Id,const TableContentGenerationVersion*> generations;
    std::map<Id,Id> table_rows;
    std::map<Id,Id> row_tables;
    std::set<Id> batches;
    u64 previous_sequence=0;
    // Validate complete publications before considering a requested table.
    for(std::size_t start=0;start<history.size();) {
      const auto& first=history[start];
      if(!Fields(first,context.database_uuid) || first.batch_ordinal!=0 ||
         first.publication_effect_sequence<=previous_sequence ||
         first.batch_target_count>history.size()-start || !batches.insert(first.batch_uuid).second)
        return Fail(Code::corrupt_history);
      previous_sequence=first.publication_effect_sequence;
      std::set<Id> targets;
      for(u32 ordinal=0;ordinal<first.batch_target_count;++ordinal) {
        const auto& version=history[start+ordinal];
        if(!Fields(version,context.database_uuid) || version.batch_uuid!=first.batch_uuid ||
           version.creator_transaction_uuid!=first.creator_transaction_uuid ||
           version.creator_local_transaction_id!=first.creator_local_transaction_id ||
           version.publication_effect_sequence!=first.publication_effect_sequence ||
           version.batch_target_count!=first.batch_target_count || version.batch_ordinal!=ordinal ||
           !targets.insert(version.table_uuid).second)
          return Fail(Code::corrupt_history);
        if(ordinal!=0) {
          const auto& prior=history[start+ordinal-1];
          if(std::make_pair(prior.schema_uuid,prior.table_uuid)>=
             std::make_pair(version.schema_uuid,version.table_uuid)) return Fail(Code::corrupt_history);
        }
        const auto row=table_rows.emplace(version.table_uuid,version.catalog_row_uuid);
        if(!row.second && row.first->second!=version.catalog_row_uuid) return Fail(Code::corrupt_history);
        const auto table=row_tables.emplace(version.catalog_row_uuid,version.table_uuid);
        if(!table.second && table.first->second!=version.table_uuid) return Fail(Code::corrupt_history);
        if(!Nil(version.predecessor_generation_uuid)) {
          const auto parent=generations.find(version.predecessor_generation_uuid);
          if(parent==generations.end() || parent->second->table_uuid!=version.table_uuid ||
             parent->second->publication_effect_sequence>=version.publication_effect_sequence)
            return Fail(Code::corrupt_history);
        }
        if(!generations.emplace(version.generation_uuid,&version).second)
          return Fail(Code::corrupt_history);
      }
      start+=first.batch_target_count;
    }
    for(const auto& version:history)
      if(!Creator(entries,version.creator_local_transaction_id,version.creator_transaction_uuid))
        return Fail(Code::inventory_required);
    std::set<Id> rollback_ids;
    using Interval=std::pair<u64,u64>;
    std::map<u64,std::vector<Interval>> invalidated_effects;
    for(const auto& rollback:rollbacks) {
      if(!Uuid(rollback.rollback_record_uuid) || !Uuid(rollback.creator_transaction_uuid) ||
         rollback.effect_sequence_lower_exclusive>=rollback.effect_sequence_upper_inclusive ||
         !rollback_ids.insert(rollback.rollback_record_uuid).second)
        return Fail(Code::corrupt_history);
      if(!Creator(entries,rollback.creator_local_transaction_id,rollback.creator_transaction_uuid))
        return Fail(Code::inventory_required);
      invalidated_effects[rollback.creator_local_transaction_id].emplace_back(
          rollback.effect_sequence_lower_exclusive,rollback.effect_sequence_upper_inclusive);
    }
    for(auto& item:invalidated_effects) {
      auto& intervals=item.second;
      std::sort(intervals.begin(),intervals.end());
      std::size_t retained=0;
      for(const auto& interval:intervals) {
        if(retained && interval.first<=intervals[retained-1].second)
          intervals[retained-1].second=std::max(intervals[retained-1].second,interval.second);
        else intervals[retained++]=interval;
      }
      intervals.resize(retained);
    }
    TableContentGenerationSelection result;
    result.status=Code::not_visible;
    std::set<u64> pending;
    for(const auto& version:history) {
      if(version.table_uuid!=context.table_uuid) continue;
      const auto* creator=Creator(entries,version.creator_local_transaction_id,version.creator_transaction_uuid);
      if(creator->identity.scope!=TransactionScope::local_node)
        return Fail(Code::cluster_authority_required);
      const auto state=RowState(creator->state);
      if(state==RowVersionState::limbo || state==RowVersionState::recovery_required)
        return Fail(Code::recovery_required);
      if((creator->state==TransactionState::committed || creator->state==TransactionState::archived) &&
         (creator->rollback_only || (creator->evidence_record_required && !creator->evidence_record_written)))
        return Fail(Code::inventory_required);
      bool invalidated=false;
      const auto scope=invalidated_effects.find(version.creator_local_transaction_id);
      if(scope!=invalidated_effects.end()) {
        const auto& intervals=scope->second;
        const auto after=std::lower_bound(intervals.begin(),intervals.end(),version.publication_effect_sequence,
            [](const Interval& interval,u64 sequence){return interval.first<sequence;});
        invalidated=after!=intervals.begin() && version.publication_effect_sequence<=std::prev(after)->second;
      }
      if(invalidated) continue;
      RowVersionMetadata metadata;
      metadata.identity.row.row_uuid.kind=UuidKind::row;
      metadata.identity.row.row_uuid.value.bytes=version.catalog_row_uuid;
      metadata.identity.creator_transaction=creator->identity;
      metadata.identity.version_sequence=version.publication_effect_sequence;
      metadata.state=state;
      metadata.creator_transaction_state=creator->state;
      metadata.payload_present=true;
      const auto visibility=EvaluateVisibility(metadata,context.snapshot);
      if(visibility.decision==VisibilityDecision::unknown) return Fail(Code::inventory_required);
      if(visibility.decision==VisibilityDecision::requires_recovery) return Fail(Code::recovery_required);
      if(visibility.decision==VisibilityDecision::wait_for_transaction) {
        if(pending.insert(creator->identity.local_id.value).second)
          result.pending_creators.push_back(creator->identity);
        continue;
      }
      if(visibility.decision!=VisibilityDecision::visible) continue;
      if(result.binding ? version.predecessor_generation_uuid!=result.binding->generation_uuid
                        : !Nil(version.predecessor_generation_uuid))
        return Fail(Code::corrupt_history);
      result.binding=version;
      result.status=Code::selected;
    }
    return result;
  } catch(const std::bad_alloc&) { return Fail(Code::resource_exhausted); }
    catch(const std::length_error&) { return Fail(Code::resource_exhausted); }
}
}  // namespace scratchbird::transaction::mga
