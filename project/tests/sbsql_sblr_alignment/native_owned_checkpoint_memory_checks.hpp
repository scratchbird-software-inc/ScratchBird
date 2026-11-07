// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Included after the independent selected-source and actual-grant test helpers.
namespace owned_source_memory {
namespace d=scratchbird::storage::disk;
namespace c=checkpoint_inventory_memory;
using Source=db::NativeOwnedCheckpointSource;
using E=db::NativeOwnedSourceError;
static_assert(!std::is_copy_constructible_v<db::NativeOwnedCheckpointMemoryHandle>);
static_assert(std::is_move_constructible_v<db::NativeOwnedCheckpointMemoryHandle>);
struct Meter final:std::pmr::memory_resource {
 std::pmr::monotonic_buffer_resource pool;std::size_t used=0;
 void* do_allocate(std::size_t bytes,std::size_t alignment) override {
  if(used>std::numeric_limits<std::size_t>::max()-(alignment-1))throw std::bad_alloc();
  const auto at=(used+alignment-1)&~(alignment-1);
  if(bytes>std::numeric_limits<std::size_t>::max()-at)throw std::bad_alloc();
  auto* p=pool.allocate(bytes,alignment);used=at+bytes;return p;
 }
 void do_deallocate(void*,std::size_t,std::size_t) override {}
 bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override{return this==&other;}
};
// Common-envelope storage fixture only, not ordinary-table family admission or
// a catalog publication operation. Install before source ownership is acquired.
db::NativeCatalogLeafResult PopulatedLeaf(db::NativeCatalogLeafPage leaf,const mga::TransactionIdentity& creator,unsigned identity_offset=0){
 namespace catalog=scratchbird::core::catalog;
 Check(leaf.body.rows.empty(),"populate only a fresh empty fixture leaf");
 for(unsigned i=0;i<2;++i){
  page::RowDataRecord row;row.storage_generation=1;
  row.row_uuid={UuidKind::row,Id(60000+i+identity_offset)};row.version_uuid=Id(60010+i+identity_offset);
  row.transaction_uuid=creator.transaction_uuid;row.local_transaction_id=creator.local_id.value;
  row.internal_row_ordinal=row.stable_slot_id=i+1;
  catalog::CatalogMetadataVersion metadata;
  metadata.record.header.kind=catalog::CatalogRecordKind::sql_object;
  metadata.record.header.row_uuid=row.row_uuid;
  metadata.record.header.object_uuid={UuidKind::object,Id(60020+i+identity_offset)};
  metadata.record.header.parent_uuid={UuidKind::schema,Id(60030)};
  metadata.owning_schema_uuid=metadata.record.header.parent_uuid;
  metadata.owner_uuid={UuidKind::principal,Id(60031)};metadata.audit_uuid={UuidKind::object,Id(60032)};
  metadata.creator_transaction_uuid=creator.transaction_uuid;
  metadata.creator_local_transaction_id=creator.local_id.value;
  metadata.definition_version=metadata.schema_epoch=metadata.security_epoch=metadata.catalog_generation=1;
  metadata.dependency_generation=metadata.invalidation_generation=1;
  metadata.lifecycle=catalog::CatalogObjectLifecycle::active;metadata.status=catalog::CatalogObjectStatus::active;
  metadata.trace_search_key="GUARDED-LEAF-STORAGE-FIXTURE";
  metadata.object_subtype="application";metadata.retention_class="catalog_history";
  metadata.record.payload="guarded-leaf-common-envelope";
  const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata);
  Check(encoded.ok(),"complete populated common metadata envelope");
  page::RowDataCell cell;cell.column_ordinal=1;
  cell.value.type_id=scratchbird::core::datatypes::CanonicalTypeId::binary;
  cell.value.payload=encoded.bytes;row.cells.push_back(std::move(cell));leaf.body.rows.push_back(std::move(row));
 }
 return db::EncodeNativeCatalogLeaf(leaf);
}
// SB_NATIVE_FIXED_ROOT_READ (drift removal C3d): the owned-source adoption checks that
// lived here ran on hand-built multi-file graphs, which adoption through page
// zero's fixed roots refuses. Their still-valid purposes are tested over the
// production create in native_owned_adoption_test.cpp. Meter and PopulatedLeaf
// remain for the selected-lease checks that include this header (R4).
} // namespace owned_source_memory
