// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "physical_mga_cow_store.hpp"
#include "catalog_schema_definition.hpp"
#include "catalog_metric_retention_policy.hpp"
#include "catalog_metric_descriptor.hpp"
#include "catalog_metric_label_schema.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <set>

namespace scratchbird::storage::database {
namespace {
namespace disk=scratchbird::storage::disk;
namespace page=scratchbird::storage::page;
namespace catalog=scratchbird::core::catalog;
namespace mga=scratchbird::transaction::mga;
namespace uuid=scratchbird::core::uuid;
using Uuid=scratchbird::core::platform::Uuid;
using Kind=scratchbird::core::platform::UuidKind;
using E=NativeCatalogVersionStageError;
NativeCatalogVersionStageResult Fail(E error){NativeCatalogVersionStageResult r;r.error=error;return r;}
bool Same(const TypedUuid& a,const TypedUuid& b){return a.kind==b.kind&&a.value==b.value;}
}

NativeCatalogVersionStageResult StageNativeCatalogVersionFromOpenDevices(
    const std::vector<disk::NativeFilespaceDevice>& supplied,
    const disk::FilespaceRootReference& checkpoint,u16 selector,u16 role,
    const NativeCatalogRelationBinding& binding,const mga::PublishedSnapshotPin& pin,
    const NativeCatalogVersionMutation& request,const NativeCatalogLeafPage& destination,u64 budget) noexcept {
  try {
    const auto& h=destination.header;const auto& owner=request.transaction;const auto& desired=request.metadata;
    if(!budget||supplied.empty()||!owner.valid()||owner.scope!=mga::TransactionScope::local_node||
        owner.transaction_uuid.kind!=Kind::transaction||!uuid::IsEngineIdentityUuid(owner.transaction_uuid.value)||
        !destination.body.rows.empty()||destination.body.next_page_number||
        !Same(request.relation_uuid,destination.body.relation_uuid)||request.relation_uuid.value!=binding.relation_uuid||
        request.page_number!=h.page_number||!Same(owner.transaction_uuid,desired.creator_transaction_uuid)||
        owner.local_id.value!=desired.creator_local_transaction_id||desired.authority_scope==catalog::CatalogAuthorityScope::cluster||
        (!request.expected_version_uuid.is_nil()&&!uuid::IsEngineIdentityUuid(request.expected_version_uuid)))return Fail(E::invalid_request);
    const bool name=desired.record.header.kind==catalog::CatalogRecordKind::localized_name;
    if(name!=request.name_payload.has_value()||(name&&(!desired.record.payload.empty()||
        !catalog::CatalogNamePayloadMatchesMetadata(*request.name_payload,desired))))return Fail(E::invalid_metadata);
    const auto validated=catalog::EncodeCatalogMetadataVersion(desired);
    if(!validated.ok()){auto r=Fail(E::invalid_metadata);r.diagnostic=validated.diagnostic;return r;}
    auto devices=supplied;std::sort(devices.begin(),devices.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    std::set<disk::FileDevice*> handles;std::vector<std::unique_lock<std::recursive_mutex>> guards;guards.reserve(devices.size());
    for(std::size_t i=0;i<devices.size();++i){const auto& f=devices[i];if(!f.device||!handles.insert(f.device).second||
        (i&&devices[i-1].filespace_uuid==f.filespace_uuid))return Fail(E::invalid_request);guards.push_back(f.device->AcquireOperationGuard());}
    auto source=ReadNativePinnedCatalogVersionsFromOpenDevices(h.database_uuid,devices,checkpoint,selector,role,binding,owner,pin,budget);
    if(!source.ok()){auto r=Fail(E::source_failure);r.source_error=source.error;r.diagnostic=source.diagnostic;return r;}
    const auto snapshot_uuid=source.snapshot_uuid;
    const auto& inventory=source.source.checkpoint.checkpoint_inventory.inventory;
    const auto actor=mga::LookupLocalTransaction(inventory,owner.local_id);
    if(!actor.ok()||!Same(actor.entry.identity.transaction_uuid,owner.transaction_uuid)||actor.entry.state!=mga::TransactionState::active||actor.entry.rollback_only)return Fail(E::writer_not_writable);
    const NativeCatalogVersionRow* previous=nullptr;for(const auto& row:source.rows)if(Same(row.metadata.record.header.row_uuid,desired.record.header.row_uuid))previous=&row;
    u64 maximum=0,previous_sequence=0,latest_sequence=0;Uuid latest_uuid;bool retained_row=false;
    for(const auto& bound:source.source.row_creators){const auto& image=source.source.relation.catalogs[bound.catalog_page_index];
      const auto& row=image.page->body.rows[bound.catalog_row_index];const auto& metadata=image.metadata.at(row.version_uuid);
      const auto& creator=inventory.entries[bound.inventory_entry_index];const auto outcome=mga::InventoryVisibilityState(creator);
      const bool released=outcome==mga::TransactionState::rolled_back||outcome==mga::TransactionState::failed_terminal;
      if(metadata.record.header.object_uuid.value==desired.record.header.object_uuid.value&&row.row_uuid.value!=desired.record.header.row_uuid.value&&!released)return Fail(E::object_reserved);
      if(row.row_uuid.value!=desired.record.header.row_uuid.value)continue;
      retained_row=true;maximum=std::max(maximum,row.row_version);
      if(!Same(metadata.record.header.object_uuid,desired.record.header.object_uuid)||metadata.record.header.kind!=desired.record.header.kind)return Fail(E::row_reserved);
      if(previous&&row.version_uuid==previous->version_uuid)previous_sequence=row.row_version;
      if(released)continue;
      if(!mga::HasCommittedInventoryOutcome(creator)&&!Same(creator.identity.transaction_uuid,owner.transaction_uuid))return Fail(E::row_reserved);
      if(row.row_version>latest_sequence){latest_sequence=row.row_version;latest_uuid=row.version_uuid;}
    }
    if(request.expected_version_uuid.is_nil()){
      if(retained_row||previous||desired.definition_version!=1||desired.record.header.deleted)return Fail(E::stale_version);
    }else{
      if(!previous||previous->version_uuid!=request.expected_version_uuid||previous->version_uuid!=latest_uuid||!previous_sequence||
          !Same(previous->metadata.record.header.object_uuid,desired.record.header.object_uuid)||previous->metadata.record.header.kind!=desired.record.header.kind||previous->metadata.record.header.deleted||
          previous->metadata.definition_version==std::numeric_limits<u64>::max()||desired.definition_version!=previous->metadata.definition_version+1||
          desired.schema_epoch<previous->metadata.schema_epoch||desired.security_epoch<previous->metadata.security_epoch||desired.resource_epoch<previous->metadata.resource_epoch||
          desired.catalog_generation<previous->metadata.catalog_generation||desired.dependency_generation<previous->metadata.dependency_generation||desired.invalidation_generation<previous->metadata.invalidation_generation||
          !catalog::CatalogSchemaDefinitionPreservesOrigin(previous->metadata,desired)||
          !catalog::CatalogMetricRetentionPolicyPreservesOrigin(previous->metadata,desired)||
          !catalog::CatalogMetricDescriptorPreservesOrigin(previous->metadata,desired)||
          !catalog::CatalogMetricLabelSchemaPreservesOrigin(previous->metadata,desired)||
          (name&&(!previous->name_payload||!catalog::CatalogNamePayloadPreservesIdentity(*previous->name_payload,*request.name_payload))))return Fail(E::stale_version);
    }
    if(maximum==std::numeric_limits<u64>::max())return Fail(E::version_overflow);
    mga::RowIdentity identity;identity.row_uuid=desired.record.header.row_uuid;
    const auto plan=mga::PlanLocalCopyOnWriteMutationForTransaction(actor.entry,identity,
        previous?mga::CopyOnWriteMutationKind::update:mga::CopyOnWriteMutationKind::insert,previous_sequence,maximum+1);
    if(!plan.ok()){auto r=Fail(E::invalid_metadata);r.diagnostic=plan.diagnostic;return r;}
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto issued=uuid::GenerateDurableEngineIdentityV7(Kind::row,static_cast<u64>(now));
    if(!issued.ok()){auto r=Fail(E::identity_failure);r.diagnostic=issued.diagnostic;return r;}
    auto leaf=destination;page::RowDataRecord row;row.row_uuid=desired.record.header.row_uuid;row.transaction_uuid=owner.transaction_uuid;
    row.local_transaction_id=owner.local_id.value;row.version_uuid=issued.value.value;row.row_version=maximum+1;
    row.previous_version_uuid=previous?previous->version_uuid:Uuid{};row.previous_row_version=previous_sequence;
    row.internal_row_ordinal=1;row.stable_slot_id=1;row.storage_generation=h.page_generation;
    auto metadata=desired;
    if(name){catalog::CatalogNameVersionBinding resident;resident.database_uuid={Kind::database,h.database_uuid};resident.filespace_uuid={Kind::filespace,h.filespace_uuid};
      resident.row_uuid=row.row_uuid;resident.version_uuid={Kind::row,row.version_uuid};resident.catalog_object_uuid=metadata.record.header.object_uuid;resident.creating_transaction_uuid=owner.transaction_uuid;
      resident.page_id=h.page_number;resident.slot_id=row.stable_slot_id;resident.storage_generation=row.storage_generation;resident.version_sequence=row.row_version;resident.creating_transaction_number=row.local_transaction_id;resident.catalog_generation=metadata.catalog_generation;
      const auto envelope=catalog::EncodeCatalogNameEnvelope({resident,*request.name_payload});if(!envelope.ok())return Fail(E::invalid_metadata);
      metadata.record.payload.assign(envelope.bytes.begin(),envelope.bytes.end());
    }
    const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata);if(!encoded.ok()){auto r=Fail(E::invalid_metadata);r.diagnostic=encoded.diagnostic;return r;}
    page::RowDataCell cell;cell.column_ordinal=1;cell.value.type_id=scratchbird::core::datatypes::CanonicalTypeId::binary;cell.value.payload=encoded.bytes;row.cells.push_back(std::move(cell));leaf.body.rows.push_back(row);
    PhysicalMgaCowRowReceipt receipt;receipt.database_uuid={Kind::database,h.database_uuid};receipt.filespace_uuid={Kind::filespace,h.filespace_uuid};receipt.relation_uuid=request.relation_uuid;receipt.row_uuid=row.row_uuid;receipt.page_uuid={Kind::page,h.page_uuid};
    receipt.creator=owner;receipt.version_uuid=row.version_uuid;receipt.previous_version_uuid=row.previous_version_uuid;receipt.page_number=h.page_number;receipt.page_generation=h.page_generation;receipt.row_version=row.row_version;receipt.storage_generation=row.storage_generation;receipt.stable_slot_id=row.stable_slot_id;
    source=NativePinnedCatalogReadResult{};
    const auto valid_pin=[&](){const auto current=pin.Resolve();return current.ok()&&current.descriptor.snapshot_uuid.value==snapshot_uuid&&Same(current.descriptor.owning_transaction_uuid,owner.transaction_uuid)&&current.descriptor.owning_transaction.value==owner.local_id.value;};
    if(!valid_pin())return Fail(E::snapshot_failure);
    auto staged=StageNativeCatalogLeafFromOpenDevices(devices,checkpoint,owner,leaf,budget);
    if(!staged.ok()){auto r=Fail(E::stage_failure);r.stage=std::move(staged);return r;}
    if(!valid_pin())return Fail(E::snapshot_failure);
    NativeCatalogVersionStageResult result;result.error=E::none;result.stage=std::move(staged);result.row=receipt;return result;
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::io_failure);}
}
}  // namespace scratchbird::storage::database
