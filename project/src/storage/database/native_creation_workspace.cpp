// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_creation_workspace.hpp"
#include "catalog_name_path.hpp"
#include "allocation_map_inplace_page.hpp"
#include "native_construction_audit.hpp"
#include "native_creation_catalog_input.hpp"
#include "native_creation_catalog_layout.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"
#include "physical_mga_cow_store.hpp"
#include "native_creation_transaction_inventory.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>

namespace scratchbird::storage::database {
namespace {
using namespace core::platform;
using E=NativeCreationWorkspaceError;
namespace mga=transaction::mga;
NativeCreationWorkspaceResult Fail(E e) {NativeCreationWorkspaceResult r;r.error=e;return r;}
template<class Error> E Cause(Error error) {
  if constexpr(requires{Error::resource_exhausted;})
    if(error==Error::resource_exhausted)return E::resource_exhausted;
  if constexpr(requires{Error::hash_failure;})
    if(error==Error::hash_failure)return E::hash_failure;
  if constexpr(requires{Error::io_failure;})
    if(error==Error::io_failure)return E::io_failure;
  return E::graph_failure;
}
template<class Result> E GraphCause(const Result& result) {
  E cause=Cause(result.error);
  const auto nested=[&](auto error){if(cause==E::graph_failure)cause=Cause(error);};
  if constexpr(requires{result.catalog_error;})nested(result.catalog_error);
  if constexpr(requires{result.allocation_error;})nested(result.allocation_error);
  if constexpr(requires{result.directory_error;})nested(result.directory_error);
  if constexpr(requires{result.system_error;})nested(result.system_error);
  if constexpr(requires{result.horizon_error;})nested(result.horizon_error);
  if constexpr(requires{result.retention_error;})nested(result.retention_error);
  if constexpr(requires{result.checkpoint_inventory;}) {
    nested(result.checkpoint_inventory.error);nested(result.checkpoint_inventory.inventory_error);
  }
  return cause;
}

// SB_NATIVE_FIXED_ROOT_READ: the typed cause of a refused fixed-root read, or
// of a refused page-zero catalog source: a digest-provider failure stays a
// hash failure, an I/O failure an I/O failure, whatever stage it was found in.
template<class Source> E FixedRootCause(const Source& r) {
  using F=NativeFixedRootMemoryError;
  if(r.page_zero_error==disk::FilespacePageZeroError::hash_provider_failure)return E::hash_failure;
  if(r.page_zero_error==disk::FilespacePageZeroError::io_failure)return E::io_failure;
  if(r.page_zero_error==disk::FilespacePageZeroError::resource_exhausted)return E::resource_exhausted;
  if constexpr(requires{r.inventory_error;r.allocation_error;r.directory_error;}) {
    for(const E cause:{Cause(r.inventory_error),Cause(r.allocation_error),Cause(r.directory_error)})
      if(cause!=E::graph_failure)return cause;
  }
  if constexpr(requires{r.catalog_root_error;})
    if(const E cause=Cause(r.catalog_root_error);cause!=E::graph_failure)return cause;
  return r.fixed_root_error==F::resource_exhausted||r.fixed_root_error==F::memory_allocation_failure?E::resource_exhausted:
      r.fixed_root_error==F::io_failure?E::io_failure:E::graph_failure;
}

// Control offsets after the last allocation-map page. Page zero is separate.
// SB_NATIVE_COMMENT_RELATION: the first leaves of roles 1..6 only; role 7,
// the comments relation, is empty at create and has no base leaf (its first
// leaf is allocated by its first insert), so the control block is unchanged.
enum Control : unsigned {checkpoint=1, inventory, directory, system, retention,
  horizon, catalog, configuration, security, first_leaf, selector_first=16,
  selector_second=17, watermark_first=18, watermark_second=19, count=20};
static_assert(selector_first==first_leaf+detail::kNativeCreationFixedLeafRoles);
constexpr std::array<u32,count> types{0,0x300,0x301,9,8,0x303,0x302,5,10,11,
  6,6,6,6,6,6,0x30e,0x30e,0x500,0x500};
struct Image {
  disk::NativeCommonPageHeader header;
  Uuid owner, allocation;
  std::vector<byte> bytes;
  std::array<byte,32> sha256{};
};
u64 Coverage(u64 size) {
  u64 n=(size-384)/128;
  while (((384+(n+1)/2+7)&~u64{7})+128*n>size) --n;
  return n;
}
// An encoded in-place allocation map page, in the shape save() takes.
struct InplaceMapImage {
  page::NativeAllocationError error=page::NativeAllocationError::invalid_family;
  std::vector<byte> bytes;
  bool ok() const noexcept {return error==page::NativeAllocationError::none;}
};
}  // namespace

NativeCreationWorkspaceResult InitializeNativeCreationWorkspaceOnOpenDevice(
    disk::FileDevice& device,const NativeFilespaceInitializationRequest& request,u64 budget,
    core::uuid::StandaloneUuidV7Issuer& issuer) noexcept {
  return InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,{},budget,issuer);
}

NativeCreationWorkspaceResult InitializePopulatedNativeCreationWorkspaceOnOpenDevice(
    disk::FileDevice& device,const NativeFilespaceInitializationRequest& request,
    const NativeCreationCatalogSeed& seed,u64 budget,
    core::uuid::StandaloneUuidV7Issuer& issuer) noexcept {
  return InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,{},budget,issuer);
}

bool FillNativeCreationNamePaths(NativeCreationCatalogSeed& seed) {
  namespace c=core::catalog;
  c::CatalogNameIndex names;
  for(const auto& record:seed[1])if(record.name)names.Add(c::BorrowCatalogNamePayload(*record.name));
  std::vector<std::pair<std::size_t,c::CatalogNamePath>> paths;
  for(std::size_t i=0;i<seed[1].size();++i){
    const auto& record=seed[1][i];if(!record.name)continue;
    const auto* entry=std::get_if<c::CatalogNameEntry>(&*record.name);if(!entry)continue;
    auto path=c::ResolveCatalogNamePath(c::BorrowCatalogNameEntry(*entry),names);
    if(!path.ok())return false;
    paths.emplace_back(i,std::move(path));
  }
  // Keys first (the paths borrow the seed), then assigned.
  std::vector<std::pair<std::size_t,std::pair<std::vector<byte>,u64>>> keys;
  for(const auto& [i,path]:paths)keys.push_back({i,{c::CatalogNamePathLookupKey(path),path.components.size()}});
  for(auto& [i,key]:keys){auto& entry=std::get<c::CatalogNameEntry>(*seed[1][i].name);
    entry.full_path_lookup_key=std::move(key.first);entry.path_component_count=key.second;}
  return true;
}

NativeCreationWorkspaceResult InitializeIndexedNativeCreationWorkspaceOnOpenDevice(
    disk::FileDevice& device,const NativeFilespaceInitializationRequest& request,
    const NativeCreationCatalogSeed& seed,const NativeCreationCatalogIndexes& indexes,u64 budget,
    core::uuid::StandaloneUuidV7Issuer& issuer,const NativeCreationCatalogNameTrees& name_trees,
    const std::optional<page::NativeBtreeDependencies>& comment_tree) noexcept {
  try {
    // SB_NATIVE_NAME_TREES: both trees or neither.
    if(name_trees.authoritative.has_value()!=name_trees.uuid.has_value())return Fail(E::invalid_request);
    if(name_trees.requested()&&(!page::NativeBtreeDependenciesValid(*name_trees.authoritative)||
        !page::NativeBtreeDependenciesValid(*name_trees.uuid)))return Fail(E::invalid_catalog_input);
    // SB_NATIVE_COMMENT_TREE: over an indexed, empty comments relation.
    if(comment_tree&&(!page::NativeBtreeDependenciesValid(*comment_tree)||!indexes[detail::kNativeCommentRelationIndex]||
        !seed[detail::kNativeCommentRelationIndex].empty()))return Fail(E::invalid_catalog_input);
    const auto guard=device.AcquireOperationGuard();
    const auto& b=request.bootstrap;
    const auto v7=[](const Uuid& id){return core::uuid::IsEngineIdentityUuid(id);};
    if(disk::ValidateFilespaceBootstrap(b)!=disk::FilespaceBootstrapError::none||
       b.lifecycle_state!=7||b.filespace_role>4||!v7(request.operation_uuid)||
       !v7(request.writer_uuid)||!v7(request.creator.transaction_uuid.value)||
       !v7(request.policy_snapshot_uuid)||issuer.binding().database_uuid!=b.database_uuid||
       issuer.binding().policy_snapshot_uuid!=request.policy_snapshot_uuid||
       request.creator.transaction_uuid.kind!=UuidKind::transaction||request.creator.local_id.value!=1||
       request.creator.scope!=mga::TransactionScope::local_node||!request.creation_utc_millis||
       request.creation_utc_millis>0xffffffffffffULL) return Fail(E::invalid_request);
    if(b.flags&disk::FilespaceBootstrapFlag::cluster_authority_required) return Fail(E::cluster_requires_authority);
    if(b.flags&disk::FilespaceBootstrapFlag::payload_encrypted) return Fail(E::encrypted_requires_crypto_authority);
    // SB_NATIVE_DATA_FILE_OWNERSHIP (owner 2026-11-13, S1): native files are
    // built only on a device that owns them by its data-file flock alone
    // (FileDevice::Open(..., native_data_file)), never one that made a lock file.
    if(!device.is_open()||device.read_only()||!device.native_data_file_owner()) return Fail(E::invalid_device);
    const auto existing=device.Size();
    if(!existing.ok()) return Fail(E::io_failure);
    if(existing.size_bytes) return Fail(E::device_not_empty);
    // The fixed-root graph (the checkpoint-era graph went with that layer,
    // W4d). The control block after the maps has no checkpoint, horizon-root,
    // selector or watermark page; the rest is packed in control order.
    if(request.genesis_graph!=NativeCreationGenesisGraph::fixed_roots)return Fail(E::invalid_request);
    std::array<u64,count> offset{};
    std::array<unsigned,count> control_at{};
    {
      u64 next=1;
      for(unsigned c=1;c<count;++c) {
        const bool drift=c==checkpoint||c==horizon||c>=selector_first;
        if(!drift)offset[c]=next++;
        if(offset[c])control_at[offset[c]]=c;
      }
    }
    const u64 controls=detail::kNativeCreationControlPages;
    // SB_ALLOCATION_MAP_IN_PLACE (owner Q22 = 1): the in-place allocation map.
    const u64 size=b.page_size_bytes,total=request.total_pages,
        coverage=u64{page::AllocationMapInplaceSlotsPerPage(static_cast<u32>(size))};
    if(!coverage) return Fail(E::invalid_capacity);
    if(!total||total>std::numeric_limits<u64>::max()/size||
       !disk::CheckFileDeviceExtent(total*size,0).ok()) return Fail(E::invalid_capacity);
    const u64 maps=total/coverage+(total%coverage!=0);
    if(maps>total||total-maps<controls) return Fail(E::invalid_capacity);
    const auto at=[&](unsigned control){return maps+offset[control];};
    // The filespace lifecycle page zero and the directory record. Page zero is
    // written last, so a fixed-root file whose page zero exists is complete:
    // online (1), owner ruling 2026-11-07. The drift-era graph keeps the
    // request's initializing (7).
    auto written_bootstrap=b;
    written_bootstrap.lifecycle_state=NativeCreatedFilespaceLifecycle(request);
    // The transaction inventory is always the in-place one (W4d-3: the legacy
    // SBTINV01 format and the request's choice of format were removed).
    // Full graph + one in-flight encoder image + one physical readback image.
    // The final selected admission retains the maps twice plus four controls,
    // and the in-place inventory's header page as a fifth.
    const u64 admitted=5;
    if(budget/size<controls+2||maps>budget/size-controls-2||
       budget/size<admitted||maps>(budget/size-admitted)/2||
       maps>std::numeric_limits<std::size_t>::max()-controls) return Fail(E::resource_exhausted);

    auto planned=detail::PlanNativeCreationCatalogLayout(size,maps,total,budget,seed,indexes,name_trees.requested(),
        comment_tree.has_value());
    if(!planned.ok())return Fail(planned.error==detail::NativeCreationLayoutError::resource_exhausted?E::resource_exhausted:
        planned.error==detail::NativeCreationLayoutError::capacity_exceeded?E::catalog_capacity_exceeded:E::invalid_catalog_input);
    auto plan=std::move(*planned.layout);const auto& layouts=plan.relations;
    const auto image_count=plan.image_count,leaf_cells=plan.leaf_cells;
    std::set<Uuid> ids;
    for(const auto& id:{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid,
        b.checksum_profile_uuid,request.operation_uuid,request.writer_uuid,request.creator.transaction_uuid.value,
        request.policy_snapshot_uuid})
      if(!ids.insert(id).second) return Fail(E::invalid_request);
    // Validate the complete batch before issuing storage identities or writing.
    // Every admitted row is a first version under this construction transaction.
    // Keep supplied object/row identities reserved against generated identities.
    for(unsigned role=0;role<seed.size();++role) {
      const auto& relation=seed[role];
      if(indexes[role]) {
        const auto& index=*indexes[role];const auto& deps=index.dependencies;
        if(!v7(index.relation_uuid)||!page::NativeBtreeDependenciesValid(deps)||
           deps.descriptor_generation!=1||deps.storage_generation!=1||
           !ids.insert(index.relation_uuid).second||!ids.insert(deps.index_uuid).second)
          return Fail(E::invalid_catalog_input);
        for(const auto& id:{deps.key_profile_uuid,deps.visibility_profile_uuid,deps.dependency_map_uuid}) {
          if(id==index.relation_uuid||id==deps.index_uuid)return Fail(E::invalid_catalog_input);
        }
      }
      for(const auto& record:relation) {
        const auto& m=record.metadata;
        const bool name=m.record.header.kind==core::catalog::CatalogRecordKind::localized_name;
        if(m.authority_scope!=core::catalog::CatalogAuthorityScope::local||
           m.creator_transaction_uuid.kind!=UuidKind::transaction||
           m.creator_transaction_uuid.value!=request.creator.transaction_uuid.value||
           m.creator_local_transaction_id!=1||m.definition_version!=1||m.record.header.deleted||
           m.schema_epoch!=1||m.security_epoch!=1||m.catalog_generation!=1||
           !core::catalog::CatalogIdentifierProfileMatchesDatabase(
             core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogNamingDialectProfileMatchesDatabase(
             core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogNamingCollisionPolicyMatchesDatabase(
             core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !NativeConstructionIntentMatchesRequest(core::catalog::BorrowCatalogMetadataVersion(m),request)||
           !core::catalog::CatalogPolicyDescriptorMatchesDatabase(core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogVerificationPolicyMatchesDatabase(core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogEvidencePolicyMatchesDatabase(core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogAuthorizationPolicyMatchesDatabase(core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogTransactionPolicyMatchesDatabase(core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogLockPolicyMatchesDatabase(core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogIsolationProfileMatchesDatabase(core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogNativeCatalogIndexMatchesDatabase(core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogBuiltinTypeDomainMatchesDatabase(core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogManagementDefinitionMatchesDatabase(core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           !core::catalog::CatalogCryptoInlinePrivateKeyPolicyMatchesDatabase(core::catalog::BorrowCatalogMetadataVersion(m),b.database_uuid)||
           ((m.object_subtype==scratchbird::core::catalog::CatalogObjectSubtype::crypto_inline_private_key||core::catalog::IsCatalogCryptoInlinePrivateKeyPolicyPayload(m.record.payload))&&role!=3)||
           name!=record.name.has_value()||
           (name&&(!m.record.payload.empty()||!core::catalog::CatalogNamePayloadMatchesMetadata(*record.name,m)))||
           !ids.insert(m.record.header.object_uuid.value).second||
           !ids.insert(m.record.header.row_uuid.value).second) return Fail(E::invalid_catalog_input);
        if(core::catalog::IsCatalogNamingCollisionPolicyPayload(m.record.payload)){
          const auto body=core::catalog::DecodeCatalogNamingCollisionPolicy(m.record.payload);
          if(!body.ok())return Fail(E::invalid_catalog_input);
          // These are embedded definitions, not repeated references. Reserve
          // their ownership against controls and every other supplied object.
          for(std::size_t i=0;i<body.record->group_count;++i)
            if(!ids.insert(body.record->groups[i].group_uuid).second)return Fail(E::invalid_catalog_input);
        }
      }
    }
    for(const auto& index:indexes)if(index)
      for(const auto& id:{index->dependencies.key_profile_uuid,index->dependencies.visibility_profile_uuid,
          index->dependencies.dependency_map_uuid})ids.insert(id);
    for(const auto& relation:seed)for(const auto& record:relation)
      ReserveNativeCreationCatalogReferences(record,[&](const Uuid& id){ids.insert(id);});
    const auto issue=[&](UuidKind kind)->Uuid {
      const auto id=issuer.Issue(kind);
      if(id.error==core::uuid::StandaloneUuidV7Error::resource_exhausted) throw E::resource_exhausted;
      if(!id.ok()||!ids.insert(id.value->value).second) throw E::identity_failure;
      return id.value->value;
    };
    const Uuid map_object=issue(UuidKind::object);
    const Uuid timeline{};
    const Uuid locator=issue(UuidKind::object);
    const Uuid publication{};
    std::vector<Image> images(image_count);
    for(u64 n=0;n<images.size();++n) {
      auto& image=images[n];
      const u32 type=n==0?1:n<=maps?3:n<maps+controls?types[control_at[n-maps]]:6;
      image.header={b.page_size_bytes,type,b.database_uuid,b.filespace_uuid,
        issue(UuidKind::page),n,1,0,b.page_size_profile_uuid};
      image.allocation=issue(UuidKind::object);
      image.owner=n==0?b.filespace_uuid:n<=maps?map_object:issue(UuidKind::object);
    }
    // SB_TRANSACTION_HEADER_PAGE: the header page belongs to the inventory
    // object, with the inventory page type (a second family under 0x0301).
    const u64 header_page=plan.transaction_header_page;
    images[header_page].header.page_type=0x301;images[header_page].owner=images[at(inventory)].owner;
    const auto ref=[&](u64 n){return disk::NativePageReference{b.filespace_uuid,n,1,b.page_size_profile_uuid};};
    const auto root=[&](u16 kind,u64 n){return disk::FilespaceRootReference{
      kind,images[n].header.page_type,b.filespace_uuid,n,1,b.page_size_profile_uuid,images[n].owner};};
    const auto save=[&](u64 n,auto encoded) {
      if(!encoded.ok()) {
        using Error=decltype(encoded.error);
        if constexpr(requires{Error::resource_exhausted;})
          if(encoded.error==Error::resource_exhausted)throw E::resource_exhausted;
        if constexpr(requires{Error::hash_failure;})
          if(encoded.error==Error::hash_failure)throw E::hash_failure;
        throw E::encoding_failure;
      }
      images[n].bytes=std::move(encoded.bytes);
      const auto hash=core::hash::ComputeSha256Digest(images[n].bytes);
      if(!hash.ok()) throw E::hash_failure;
      images[n].sha256=hash.digest;
    };
    const Uuid creator=request.creator.transaction_uuid.value;
    auto begun=mga::BeginLocalTransaction(mga::MakeEmptyLocalTransactionInventory(),
      request.creator.transaction_uuid,request.creation_utc_millis);
    if(!begun.ok()) return Fail(E::encoding_failure);
    auto committed=mga::CommitLocalTransaction(std::move(begun.inventory),request.creator.local_id,request.creation_utc_millis);
    if(!committed.ok()) return Fail(E::encoding_failure);
    const auto creator_inventory=std::move(committed.inventory);
    {
    // SB_COMPACT_TRANSACTION_STATE_PAGE / SB_TRANSACTION_HEADER_PAGE (owner
    // 2026-11-05): the in-place transaction inventory. Transaction 1, the
    // construction transaction, is committed; Next is 2.
      const auto derived=mga::ComputeLocalTransactionHorizons(creator_inventory);
      if(!derived.ok()||request.creation_utc_millis>std::numeric_limits<u64>::max()/1'000'000)
        return Fail(E::encoding_failure);
      const auto encoded=detail::EncodeNativeCreationTransactionInventory(images[at(inventory)].header,
          images[header_page].header,creator,creator_inventory.next_local_transaction_id,derived.horizons,
          request.creation_utc_millis);
      if(!encoded)return Fail(E::encoding_failure);
      const auto keep=[&](u64 n,std::vector<byte> bytes){
        images[n].bytes=std::move(bytes);
        const auto hash=core::hash::ComputeSha256Digest(images[n].bytes);
        if(!hash.ok()) throw E::hash_failure;
        images[n].sha256=hash.digest;
      };
      keep(at(inventory),std::move(encoded->state));keep(header_page,std::move(encoded->headers));
    }

    NativeCreationWorkspaceReceipt receipt;
    receipt.database_uuid=b.database_uuid;receipt.filespace_uuid=b.filespace_uuid;
    receipt.operation_uuid=request.operation_uuid;receipt.page_zero_uuid=images[0].header.page_uuid;
    receipt.timeline_uuid=timeline;receipt.publication_uuid=publication;receipt.construction_transaction=request.creator;
    receipt.total_pages=total;receipt.free_pages=total-images.size();receipt.map_pages=maps;
    // SB_NATIVE_NAME_TREES: where each names-relation row lands (base page,
    // version), for the trees' cells.
    std::vector<std::pair<u64,Uuid>> name_placement(seed[1].size());
    for(unsigned i=0;i<page::kNativeCatalogRelationRoles;++i) {
      const auto& layout=layouts[i];
      const Uuid relation_uuid=indexes[i]?indexes[i]->relation_uuid:images[layout.bases.front()].owner;
      receipt.relation_uuids[i]=relation_uuid;
      std::vector<page::NativeBtreeCell> navigation_cells;
      if(indexes[i])navigation_cells.reserve(seed[i].size());
      for(std::size_t base=0;base<layout.bases.size();++base) {
      const u64 n=layout.bases[base];images[n].owner=relation_uuid;
      NativeCatalogLeafPage leaf;leaf.header=images[n].header;
      leaf.body.relation_uuid={UuidKind::object,relation_uuid};
      leaf.body.segment_id=leaf.body.segment_generation=leaf.body.page_generation=leaf.body.compaction_generation=1;
      leaf.body.page_number=n;
      u32 slot=0;
      const auto end=base+1<layout.starts.size()?layout.starts[base+1]:seed[i].size();
      for(auto at=layout.starts[base];at<end;++at) {
        const auto& record=seed[i][at];
        auto metadata=record.metadata;
        page::RowDataRecord row;row.row_uuid=metadata.record.header.row_uuid;
        row.version_uuid=issue(UuidKind::row);row.transaction_uuid=request.creator.transaction_uuid;
        row.local_transaction_id=1;row.internal_row_ordinal=at+1;row.stable_slot_id=++slot;
        if(i==1)name_placement[at]={n,row.version_uuid};
        row.row_version=row.storage_generation=1;
        if(record.name) {
          const core::catalog::CatalogNameVersionBinding binding{
            {UuidKind::database,b.database_uuid},{UuidKind::filespace,b.filespace_uuid},
            row.row_uuid,{UuidKind::row,row.version_uuid},metadata.record.header.object_uuid,
            row.transaction_uuid,n,slot,1,1,1,metadata.catalog_generation};
          const auto name=core::catalog::EncodeCatalogNameEnvelope({binding,*record.name});
          if(!name.ok()) return Fail(E::invalid_catalog_input);
          metadata.record.payload.assign(name.bytes.begin(),name.bytes.end());
        }
        const auto encoded=core::catalog::EncodeCatalogMetadataVersion(metadata);
        if(!encoded.ok()) return Fail(encoded.diagnostic.diagnostic_code=="SB-CORE-HASH-SHA256-FAILED"
            ?E::hash_failure:E::invalid_catalog_input);
        core::datatypes::DatatypeBinaryValue cell;
        cell.type_id=core::datatypes::CanonicalTypeId::binary;cell.payload=encoded.bytes;
        row.cells.push_back({1,std::move(cell)});
        if(indexes[i]) {
          page::NativeBtreeKey key;
          key.encoded_key.assign(row.row_uuid.value.bytes.begin(),row.row_uuid.value.bytes.end());
          key.row_uuid=row.row_uuid.value;key.version_uuid=row.version_uuid;
          // SB_NATIVE_BTREE_COMPRESSED_PAGE: the cell carries its version's
          // creating transaction (the create's, 1).
          navigation_cells.push_back({std::move(key),false,{},ref(n),row.local_transaction_id});
        }
        leaf.body.rows.push_back(std::move(row));
      }
      auto encoded=EncodeNativeCatalogLeaf(leaf);
      if(!encoded.ok()) return Fail(encoded.error==NativeCatalogLeafError::resource_exhausted
        ?E::resource_exhausted:encoded.error==NativeCatalogLeafError::hash_failure
        ?E::hash_failure:encoded.error==NativeCatalogLeafError::invalid_body
        ?E::catalog_capacity_exceeded:E::invalid_catalog_input);
      save(n,std::move(encoded));
      }
      receipt.catalog_rows[i]=seed[i].size();
      if(!indexes[i]) {
        receipt.relations[i]={static_cast<u16>(i+1),6,ref(layout.bases.front()),relation_uuid};
        continue;
      }
      const auto& deps=indexes[i]->dependencies;receipt.index_dependencies[i]=deps;
      std::sort(navigation_cells.begin(),navigation_cells.end(),[](const auto& a,const auto& b) {
        return page::CompareNativeBtreeKeys(a.key,b.key)<0;
      });
      const auto node=[&](u64 n,u32 type,u16 level) {
        images[n].header.page_type=type;images[n].owner=deps.index_uuid;
        page::NativeBtreePage p;p.header=images[n].header;p.dependencies=deps;
        p.creator_transaction_uuid=creator;p.creator_local_transaction_id=1;
        p.maintenance_state=1;p.tree_level=level;return p;
      };
      // Every level, bottom-up, through the builder recovery shares
      // (SB_CATALOG_BTREE_N_LEVEL): leaves, branches, then the root.
      std::optional<NativeCreationWorkspaceResult> tree_failure;
      const bool built=detail::BuildNativeCreationCatalogBtree(layout,leaf_cells,std::move(navigation_cells),ref,node,
          [&](u64 n,const page::NativeBtreePage& p) {
        auto encoded=page::EncodeNativeBtreePage(p);
        if(!encoded.ok()) {
          tree_failure=Fail(encoded.error==page::NativeBtreeError::resource_exhausted?E::resource_exhausted:
              encoded.error==page::NativeBtreeError::hash_failure?E::hash_failure:E::encoding_failure);
          tree_failure->btree_error=encoded.error;tree_failure->failed_page_number=n;return false;
        }
        save(n,std::move(encoded));return true;
      });
      if(tree_failure)return *tree_failure;
      if(!built)return Fail(E::encoding_failure);
      receipt.relations[i]={static_cast<u16>(i+1),0x200,ref(layout.root),deps.index_uuid};
    }
    if(name_trees.requested()) {
      // SB_NATIVE_NAME_TREES: the two name trees over the names relation, from
      // the same cell seeds the planner packed, now bound to the placed rows.
      if(!plan.name_trees)return Fail(E::encoding_failure);
      auto seeds=detail::BuildNativeCreationNameTreeCellSeeds(seed);
      if(!seeds)return Fail(E::invalid_catalog_input);
      const std::array<const page::NativeBtreeDependencies*,2> tuples{&*name_trees.authoritative,&*name_trees.uuid};
      for(unsigned t=0;t<2;++t) {
        const auto& layout=(*plan.name_trees)[t];const auto& deps=*tuples[t];
        if(layout.cells!=(*seeds)[t].size())return Fail(E::encoding_failure);
        std::vector<page::NativeBtreeCell> cells;cells.reserve(layout.cells);
        for(auto& cell_seed:(*seeds)[t]) {
          page::NativeBtreeKey key;key.encoded_key=std::move(cell_seed.key);
          key.row_uuid=seed[1][cell_seed.row].metadata.record.header.row_uuid.value;
          key.version_uuid=name_placement[cell_seed.row].second;
          page::NativeBtreeCell cell{std::move(key),false,{},ref(name_placement[cell_seed.row].first),1};
          cell.payload=std::move(cell_seed.payload);
          cells.push_back(std::move(cell));
        }
        // The planner's order (key, then row UUID) is the key order: rows are
        // distinct, so the version tie-break never decides.
        for(std::size_t i=1;i<cells.size();++i)
          if(page::CompareNativeBtreeKeys(cells[i-1].key,cells[i].key)>=0)return Fail(E::encoding_failure);
        const auto node=[&](u64 n,u32 type,u16 level) {
          images[n].header.page_type=type;images[n].owner=deps.index_uuid;
          page::NativeBtreePage p;p.header=images[n].header;p.dependencies=deps;
          p.creator_transaction_uuid=creator;p.creator_local_transaction_id=1;
          p.maintenance_state=1;p.tree_level=level;return p;
        };
        std::optional<NativeCreationWorkspaceResult> tree_failure;
        const bool built=detail::BuildNativeCreationCatalogBtree(layout.tree,0,std::move(cells),ref,node,
            [&](u64 n,const page::NativeBtreePage& p) {
          auto encoded=page::EncodeNativeBtreePage(p);
          if(!encoded.ok()) {
            tree_failure=Fail(encoded.error==page::NativeBtreeError::resource_exhausted?E::resource_exhausted:
                encoded.error==page::NativeBtreeError::hash_failure?E::hash_failure:E::encoding_failure);
            tree_failure->btree_error=encoded.error;tree_failure->failed_page_number=n;return false;
          }
          save(n,std::move(encoded));return true;
        },layout.leaf_starts);
        if(tree_failure)return *tree_failure;
        if(!built)return Fail(E::encoding_failure);
        receipt.name_tree_roots[t]=root(static_cast<u16>(22+t),layout.tree.root);
        receipt.name_tree_cells[t]=layout.cells;
        receipt.name_tree_pages[t]=1+layout.tree.navigation.size();
        for(const auto& level:layout.tree.branches)receipt.name_tree_pages[t]+=level.size();
      }
    }
    if(comment_tree) {
      // SB_NATIVE_COMMENT_TREE: the comment tree, its root alone and empty.
      if(!plan.comment_tree_root)return Fail(E::encoding_failure);
      const u64 n=*plan.comment_tree_root;
      images[n].header.page_type=0x200;images[n].owner=comment_tree->index_uuid;
      page::NativeBtreePage p;p.header=images[n].header;p.dependencies=*comment_tree;
      p.creator_transaction_uuid=creator;p.creator_local_transaction_id=1;
      p.maintenance_state=1;p.tree_level=0;
      auto encoded=page::EncodeNativeBtreePage(p);
      if(!encoded.ok()) {
        auto failed=Fail(encoded.error==page::NativeBtreeError::resource_exhausted?E::resource_exhausted:
            encoded.error==page::NativeBtreeError::hash_failure?E::hash_failure:E::encoding_failure);
        failed.btree_error=encoded.error;failed.failed_page_number=n;return failed;
      }
      save(n,std::move(encoded));
      receipt.comment_tree_root=root(24,n);
    }
    for(const auto control:{catalog,configuration,security}) {
      const u64 n=at(control);
      page::NativeCatalogRoot c;c.header=images[n].header;c.object_uuid=images[n].owner;
      c.root_kind=control==catalog?2:control==configuration?6:7;
      c.creator_transaction_uuid=creator;c.creator_local_transaction_id=1;
      c.catalog_generation=c.schema_epoch=c.security_epoch=1;
      if(control==catalog)c.roots.assign(receipt.relations.begin(),receipt.relations.end());
      else c.roots.push_back(receipt.relations[control==configuration?4:3]);
      save(n,page::EncodeNativeCatalogRoot(c));
    }
    page::NativeFilespaceDirectory d;d.header=images[at(directory)].header;
    d.object_uuid=images[at(directory)].owner;d.directory_generation=1;
    d.creator_transaction_uuid=creator;d.creator_local_transaction_id=1;d.total_records=1;
    d.records.push_back({written_bootstrap,locator,receipt.page_zero_uuid,1,1,total,1,{}});
    save(at(directory),page::EncodeNativeFilespaceDirectory(d));
// SB_MGA_DATABASE_STATE (owner 2026-11-07): the administrative mode only.
    // A complete create is an online, read-write database.
    NativeDatabaseState state;state.header=images[at(system)].header;state.object_uuid=images[at(system)].owner;
    state.creator_transaction_uuid=creator;state.creator_local_transaction_id=1;
    state.mode=NativeDatabaseMode::online;state.flags=0;
    save(at(system),EncodeNativeDatabaseState(state));
  
    page::NativeRetentionPage pins;pins.header=images[at(retention)].header;pins.object_uuid=images[at(retention)].owner;
    pins.epoch=1;pins.creator_transaction_uuid=creator;pins.creator_local_transaction_id=1;
    save(at(retention),page::EncodeNativeRetentionPage(pins));
    const auto derived=mga::ComputeLocalTransactionHorizons(creator_inventory);
    if(!derived.ok()) return Fail(E::encoding_failure);
    // The horizons are the in-place inventory's counters (no horizon root).
    for(u64 i=maps;i>0;--i) {
      page::AllocationMapInplaceFormat f;f.header=images[i].header;f.object_uuid=map_object;f.sequence=i-1;
      f.first_page=(i-1)*coverage;f.slots=static_cast<u32>(std::min(coverage,total-f.first_page));f.total_pages=total;
      f.creator_transaction_uuid=creator;f.creator_local_transaction_id=1;
      if(i<maps){f.next_page=i+1;f.next_generation=images[i+1].header.page_generation;}
      std::vector<page::NativeAllocationState> states(f.slots,page::NativeAllocationState::free);
      std::vector<page::NativeAllocationRecord> records;
      const auto end=std::min(f.first_page+f.slots,static_cast<u64>(images.size()));
      for(u64 n=f.first_page;n<end;++n) {
        const auto& image=images[n];states[n-f.first_page]=page::NativeAllocationState::allocated;
        records.push_back({n,image.allocation,image.header.page_uuid,image.owner,creator,1,1,0,image.header.page_type});
      }
      InplaceMapImage encoded;encoded.bytes.resize(size);
      encoded.error=page::EncodeAllocationMapInplacePage(f,states,records,encoded.bytes);
      save(i,std::move(encoded));
    }
    disk::FilespacePageZero zero;zero.bootstrap=written_bootstrap;zero.page_uuid=receipt.page_zero_uuid;
    zero.creation_operation_uuid=request.operation_uuid;zero.writer_identity_uuid=request.writer_uuid;
    zero.page_generation=zero.root_set_generation=1;zero.total_pages=total;zero.free_pages=receipt.free_pages;
    zero.creation_utc_millis=request.creation_utc_millis;
    const std::array<u64,10> zero_roots{at(system),at(catalog),1,at(inventory),at(directory),
      at(configuration),at(security),at(catalog),at(checkpoint),at(retention)};
    // The fixed-root graph names kinds 1-8 and 10 (no kind 9 checkpoint).
    for(unsigned i=0;i<zero_roots.size();++i)
      if(i+1!=9)zero.roots.push_back(root(i+1,zero_roots[i]));
    // SB_NATIVE_NAME_TREES: kinds 22 and 23, after the fixed roots.
    for(const auto& tree_root:receipt.name_tree_roots)if(tree_root)zero.roots.push_back(*tree_root);
    // SB_NATIVE_COMMENT_TREE: kind 24, after them.
    if(receipt.comment_tree_root)zero.roots.push_back(*receipt.comment_tree_root);
    auto encoded_zero=disk::EncodeFilespacePageZero(zero);
    if(!encoded_zero.ok()) return Fail(
        encoded_zero.error==disk::FilespacePageZeroError::resource_exhausted?E::resource_exhausted:
        encoded_zero.error==disk::FilespacePageZeroError::hash_provider_failure?E::hash_failure:E::encoding_failure);
    images[0].bytes=std::move(*encoded_zero.bytes);

    // No writes before the complete graph, identities and receipt are prepared.
    // Device writes are ordered/durable. Coalesce zero initialization within
    // the caller's existing image allowance instead of paying one durable
    // write per page. Keep one image of headroom and cap the working buffer;
    // exact-minimum callers still use the original single-page path.
    const u64 zero_batch_pages=std::min<u64>({16,total,budget/size-images.size()-1});
    std::vector<byte> scratch(zero_batch_pages*size,0);
    const auto write=[&](u64 n,const auto& bytes) {
      const auto io=device.WriteAt(n*size,bytes.data(),bytes.size());
      return io.ok()&&io.bytes_transferred==bytes.size();
    };
    const auto compare=[&](u64 n,const std::vector<byte>* expected)->E {
      const auto io=device.ReadAt(n*size,scratch.data(),scratch.size());
      if(!io.ok()||io.bytes_transferred!=scratch.size())return E::io_failure;
      if(expected?scratch!=*expected:!std::all_of(scratch.begin(),scratch.end(),[](byte b){return !b;}))return E::readback_mismatch;
      return E::none;
    };
    for(u64 n=0;n<total;) {
      const u64 pages=std::min(zero_batch_pages,total-n),bytes=pages*size;
      const auto io=device.WriteAt(n*size,scratch.data(),bytes);
      if(!io.ok()||io.bytes_transferred!=bytes)return Fail(E::io_failure);
      n+=pages;
    }
    scratch.resize(size);
// SB_NATIVE_FIXED_ROOT_READ: careful write. Every page but page zero,
    // a barrier and a full readback (page zero still zero); then page zero,
    // a barrier and its readback. Page zero is the commit point: a file
    // whose page zero is not complete is not a database.
    for(u64 n=1;n<images.size();++n)if(!write(n,images[n].bytes))return Fail(E::io_failure);
    if(!device.Sync().ok())return Fail(E::io_failure);
    for(u64 n=0;n<total;++n) {
      const auto error=compare(n,n>0&&n<images.size()?&images[n].bytes:nullptr);
      if(error!=E::none){auto failed=Fail(error);failed.failed_page_number=n;return failed;}
    }
    if(!write(0,images[0].bytes)||!device.Sync().ok())return Fail(E::io_failure);
    if(const auto error=compare(0,&images[0].bytes);error!=E::none){
      auto failed=Fail(error);failed.failed_page_number=0;return failed;
    }
  
    images.clear();std::vector<byte>().swap(scratch);
    const std::vector<disk::NativeFilespaceDevice> devices{{b.filespace_uuid,b.page_size_profile_uuid,&device}};
    // SB_NATIVE_FIXED_ROOT_READ: read the file back the way the server does.
    // The backed fixed-root lease verifies page zero, the in-place inventory,
    // the allocation chain and the directory, and binds every fixed root to
    // an allocated page of its type and object created by a committed
    // transaction. Every catalog relation page is then bound to its own
    // allocation record, and every relation is read through page zero below.
    if(budget>(std::numeric_limits<std::size_t>::max()-(std::size_t{1}<<20))/2)return Fail(E::resource_exhausted);
    const std::size_t backing_bytes=static_cast<std::size_t>(budget)*2+(std::size_t{1}<<20);
    std::unique_ptr<byte[]> backing(new byte[backing_bytes]);
    auto fixed=AcquireNativeFixedRootBackedLease(b.database_uuid,devices,b.filespace_uuid,
        {budget,backing_bytes},{backing.get(),backing_bytes});
    if(!fixed.ok()) {
      struct {NativeFixedRootMemoryError fixed_root_error;disk::FilespacePageZeroError page_zero_error;
        page::NativeInventoryError inventory_error;page::NativeAllocationError allocation_error;
        page::NativeDirectoryError directory_error;} findings{fixed.error,fixed.page_zero_error,fixed.inventory_error,
        fixed.allocation_error,fixed.directory_error};
      return Fail(FixedRootCause(findings));
    }
    const auto& lease=*fixed.lease;
    if(lease.identity().page_zero_uuid!=receipt.page_zero_uuid||lease.identity().filespace_uuid!=b.filespace_uuid||
        lease.identity().database_uuid!=b.database_uuid)return Fail(E::graph_failure);
    for(const auto& r:lease.roots()){
      if(r.kind==24) {  // SB_NATIVE_COMMENT_TREE
        if(!receipt.comment_tree_root||receipt.comment_tree_root->page_number!=r.page_number||
            receipt.comment_tree_root->object_uuid!=r.object_uuid||r.page_type!=0x200)return Fail(E::graph_failure);
        continue;
      }
      const bool tree=r.kind==22||r.kind==23;
      if(r.kind==9||(r.kind>10&&!tree)||(tree&&!name_trees.requested()))return Fail(E::graph_failure);
      if(tree&&(!receipt.name_tree_roots[r.kind-22]||receipt.name_tree_roots[r.kind-22]->page_number!=r.page_number||
          receipt.name_tree_roots[r.kind-22]->object_uuid!=r.object_uuid||r.page_type!=0x200))return Fail(E::graph_failure);
    }
    if(name_trees.requested()&&(!lease.Root(22)||!lease.Root(23)))return Fail(E::graph_failure);
    if(comment_tree.has_value()!=(lease.Root(24)!=nullptr))return Fail(E::graph_failure);
    const auto allocated=[&](const auto& target,u32 type,const Uuid& owner)->E {
      if(target.filespace_uuid!=b.filespace_uuid||target.page_size_profile_uuid!=b.page_size_profile_uuid)return E::graph_failure;
      std::vector<byte> bytes(size);
      const auto io=device.ReadAt(target.page_number*size,bytes.data(),bytes.size());
      if(!io.ok()||io.bytes_transferred!=bytes.size())return E::io_failure;
      const disk::NativeCommonPageHeaderBinding binding{{b.database_uuid,b.filespace_uuid,b.page_size_profile_uuid},
        target.page_number,target.page_generation,type,{}};
      const auto header=disk::DecodeNativeCommonPageHeader(bytes.data(),128,&binding);
      if(!header.ok())return Cause(header.error);
      const auto* record=detail::NativeFixedRootRecord(lease.allocation(),target.page_number);
      return record&&record->owner_uuid==owner&&record->page_uuid==header.header->page_uuid&&record->page_type==type&&
          record->page_generation==target.page_generation&&record->creator_transaction_uuid==creator&&
          record->creator_local_transaction_id==1&&record->creator_operation_uuid.is_nil()?E::none:E::graph_failure;
    };
    for(const auto& r:receipt.relations)
      if(const auto error=allocated(r.page,r.page_type,r.object_uuid);error!=E::none){auto failed=Fail(error);failed.failed_page_number=r.page.page_number;return failed;}
    for(unsigned i=0;i<layouts.size();++i) {
      for(const auto n:layouts[i].bases)
        if(const auto error=allocated(ref(n),6,receipt.relation_uuids[i]);error!=E::none){auto failed=Fail(error);failed.failed_page_number=n;return failed;}
      for(const auto n:layouts[i].navigation)
        if(const auto error=allocated(ref(n),0x202,receipt.index_dependencies[i]->index_uuid);error!=E::none){auto failed=Fail(error);failed.failed_page_number=n;return failed;}
      for(const auto& level:layouts[i].branches)for(const auto n:level)
        if(const auto error=allocated(ref(n),0x201,receipt.index_dependencies[i]->index_uuid);error!=E::none){auto failed=Fail(error);failed.failed_page_number=n;return failed;}
    }
    if(plan.comment_tree_root)  // SB_NATIVE_COMMENT_TREE: allocated to the tree's index.
      if(const auto error=allocated(ref(*plan.comment_tree_root),0x200,comment_tree->index_uuid);error!=E::none){
        auto failed=Fail(error);failed.failed_page_number=*plan.comment_tree_root;return failed;}
    if(plan.name_trees)for(unsigned t=0;t<2;++t) {
      // SB_NATIVE_NAME_TREES: every tree page is allocated to the tree's index.
      const auto& layout=(*plan.name_trees)[t].tree;const auto& index=t?name_trees.uuid->index_uuid:name_trees.authoritative->index_uuid;
      if(const auto error=allocated(ref(layout.root),0x200,index);error!=E::none){auto failed=Fail(error);failed.failed_page_number=layout.root;return failed;}
      for(const auto n:layout.navigation)
        if(const auto error=allocated(ref(n),0x202,index);error!=E::none){auto failed=Fail(error);failed.failed_page_number=n;return failed;}
      for(const auto& level:layout.branches)for(const auto n:level)
        if(const auto error=allocated(ref(n),0x201,index);error!=E::none){auto failed=Fail(error);failed.failed_page_number=n;return failed;}
    }
  
    const NativeCatalogSource catalog_source=NativeCatalogSource::PageZero(b.filespace_uuid);
    for(std::size_t i=0;i<receipt.relations.size();++i) {
      const auto& relation=receipt.relations[i];
      if(!receipt.index_dependencies[i]) {
        const auto leaf=ReadNativeCatalogLeafFromOpenDevice(device,b.database_uuid,relation);
        if(!leaf.ok()||leaf.page->body.rows.size()!=receipt.catalog_rows[i])return Fail(
            leaf.error==NativeCatalogLeafError::resource_exhausted?E::resource_exhausted:
            leaf.error==NativeCatalogLeafError::hash_failure?E::hash_failure:
            leaf.error==NativeCatalogLeafError::io_failure?E::io_failure:E::graph_failure);
      }
      if(receipt.catalog_rows[i]||receipt.index_dependencies[i]) {
        const auto committed=ReadNativeCommittedCatalogVersionsFromOpenDevices(b.database_uuid,
          devices,catalog_source,2,static_cast<u16>(i+1),
          {receipt.relation_uuids[i],receipt.index_dependencies[i]},budget);
        if(!committed.ok()||committed.rows.size()!=receipt.catalog_rows[i]) {
          auto failed=Fail(committed.error==NativePinnedCatalogReadError::resource_exhausted?E::resource_exhausted:
              committed.error==NativePinnedCatalogReadError::io_failure?E::io_failure:
              committed.source.relation.error==NativeCatalogRelationError::resource_exhausted?E::resource_exhausted:
              committed.source.relation.error==NativeCatalogRelationError::hash_failure?E::hash_failure:E::graph_failure);
          failed.failed_page_number=relation.page.page_number;
          failed.catalog_read_error=committed.error;failed.catalog_relation_error=committed.source.relation.error;
          failed.btree_error=committed.source.relation.tree_error;
          if(failed.error==E::graph_failure)failed.error=Cause(committed.source.error);
          if(failed.error==E::graph_failure)failed.error=Cause(committed.source.relation.error);
          if(failed.error==E::graph_failure)failed.error=Cause(committed.source.relation.tree_error);
          if(failed.error==E::graph_failure)failed.error=Cause(committed.source.relation.leaf_error);
          if(failed.error==E::graph_failure&&committed.source.page_zero_source)failed.error=FixedRootCause(committed.source);
          return failed;
        }
      }
    }
    NativeCreationWorkspaceResult result;result.error=E::none;result.receipt=std::move(receipt);return result;
  }catch(E error){return Fail(error);}
   catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}
   catch(const std::length_error&){return Fail(E::resource_exhausted);}
   catch(...){return Fail(E::io_failure);}
}
}  // namespace scratchbird::storage::database
