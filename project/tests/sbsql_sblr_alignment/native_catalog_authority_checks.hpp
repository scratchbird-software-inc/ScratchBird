// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual owning readers must not infer local authority from clear page flags.
namespace {
void NativeCatalogAuthorityChecks() {
  Fixture fixture;
  for(unsigned profile=0;profile<5;++profile) {
    const auto request=Request(profile);page_bytes=request.bootstrap.page_size_bytes;
    total_pages=request.total_pages;selector_page=17;
    for(bool index:{false,true}) {
      auto r=request;if(index)r.bootstrap.filespace_role=5;
      const auto path=fixture.Next();disk::FileDevice device;
      Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"authority fixture owned device");
      page::NativeCatalogRootReference leaf;
      page::NativeBtreePage node;
      if(index) {
        const auto created=db::InitializeNativeFilespaceOnOpenDevice(device,r,256*page_bytes,*fixture.issuer);
        Check(created.ok(),"real secondary data bootstrap for navigation reader");
        node.header={static_cast<u32>(page_bytes),0x200,Id(1),Id(2),Id(801),12,1,0,r.bootstrap.page_size_profile_uuid};
        node.dependencies={Id(802),1,1,Id(803),Id(804),Id(805),{}};
        node.dependencies.dependency_map_sha256.fill(1);
        node.creator_transaction_uuid=r.creator.transaction_uuid.value;node.creator_local_transaction_id=1;
        node.maintenance_state=1;
        const auto bytes=page::EncodeNativeBtreePage(node);
        Check(bytes.ok(),"valid empty native index root component image");
        Check(device.WriteAt(12*page_bytes,bytes.bytes.data(),bytes.bytes.size()).ok()&&device.Sync().ok(),"real index bytes for reader test, not allocation/publication evidence");
      } else {
        const auto created=db::InitializeNativeCreationWorkspaceOnOpenDevice(device,r,256*page_bytes,*fixture.issuer);
        Check(created.ok(),"actual primary catalog construction");leaf=created.receipt->relations[0];
      }
      const auto original=disk::ReadFilespacePageZeroFromOpenDevice(device);
      Check(original.ok(),"actual authority fixture page zero");
      const auto zero_write=[&](u32 flags){auto zero=*original.record;zero.bootstrap.flags=flags;
        if(flags&disk::FilespaceBootstrapFlag::payload_encrypted)zero.bootstrap.encryption_profile_uuid=Id(806);
        const auto encoded=disk::EncodeFilespacePageZero(zero);
        Check(encoded.ok(),"valid protected bootstrap, target page flags unchanged");
        Check(device.WriteAt(0,encoded.bytes->data(),encoded.bytes->size()).ok()&&device.Sync().ok(),"persist actual filespace authority");
        Check(device.Close().ok()&&device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"reopen protected filespace");
      };
      for(const u32 flags:{0u,2u,1u,3u,0u}) {
        zero_write(flags);
        Arm();const auto probe=disk::ReadFilespacePageZeroFromOpenDevice(device);Disarm();
        Check(probe.ok(),"independent protected page-zero read");const auto bootstrap_reads=calls[read_call];
        if(index) {
          const disk::NativePageReference ref{Id(2),12,1,r.bootstrap.page_size_profile_uuid};
          Arm();const auto read=page::ReadNativeBtreePageFromOpenDevice(device,Id(1),ref,0x200,node.dependencies);Disarm();
          if(!flags)Check(read.ok()&&read.page->header.flags==0,"unprotected owning index read still succeeds");
          else {const auto expected=(flags&1u)?page::NativeBtreeError::encrypted_requires_crypto_authority:page::NativeBtreeError::cluster_requires_authority;
            Check(!read.ok()&&!read.page&&read.bytes.empty()&&read.error==expected,"owning index reader preserves filespace authority");
            Check(calls[read_call]==bootstrap_reads,"index payload not read across authority boundary");}
          Check(!calls[write_call]&&!calls[sync_call],"single index read does not mutate protected source");
          Arm();const auto tree=page::ReadNativeBtreeTreeFromOpenDevices(Id(1),{{Id(2),r.bootstrap.page_size_profile_uuid,&device}},ref,node.dependencies,32*page_bytes);Disarm();
          if(!flags)Check(tree.ok()&&tree.pages.size()==1&&tree.leaves.size()==1,"unprotected owning tree read still succeeds");
          else Check(!tree.ok()&&tree.pages.empty()&&tree.leaves.empty()&&!tree.retained_image_bytes&&
              tree.error==((flags&1u)?page::NativeBtreeError::encrypted_requires_crypto_authority:page::NativeBtreeError::cluster_requires_authority),"whole owning tree exposes no protected prefix");
        } else {
          Arm();const auto read=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),leaf);Disarm();
          if(!flags)Check(read.ok()&&read.page->header.flags==0,"unprotected owning catalog read still succeeds");
          else {const auto expected=(flags&1u)?db::NativeCatalogLeafError::encrypted_requires_crypto_authority:db::NativeCatalogLeafError::cluster_requires_authority;
            Check(!read.ok()&&!read.page&&read.bytes.empty()&&read.error==expected,"owning catalog reader preserves filespace authority");
            Check(calls[read_call]==bootstrap_reads,"catalog payload not read across authority boundary");}
          Check(!calls[write_call]&&!calls[sync_call],"single catalog read does not mutate protected source");
          Arm();const auto relation=db::ReadNativeCatalogRelationImagesFromOpenDevices(Id(1),{{Id(2),r.bootstrap.page_size_profile_uuid,&device}},leaf,{leaf.object_uuid,{}},32*page_bytes);Disarm();
          if(!flags)Check(relation.ok()&&relation.catalogs.size()==1,"unprotected owning relation still succeeds");
          else Check(!relation.ok()&&relation.catalogs.empty()&&relation.bindings.empty()&&!relation.retained_image_bytes&&
              relation.error==db::NativeCatalogRelationError::leaf_failure&&
              relation.leaf_error==((flags&1u)?db::NativeCatalogLeafError::encrypted_requires_crypto_authority:db::NativeCatalogLeafError::cluster_requires_authority),"whole relation preserves authority diagnostic without rows");
          Check(!calls[write_call]&&!calls[sync_call],"relation reader never mutates protected source");
          for(const auto& root_ref:original.record->roots)if(root_ref.kind==2||root_ref.kind==6||root_ref.kind==7) {
            Arm();const auto root=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),root_ref);Disarm();
            if(!flags)Check(root.ok(),"unprotected owning catalog/config/security root still succeeds");
            else Check(!root.ok()&&!root.root&&root.bytes.empty()&&calls[read_call]==bootstrap_reads&&
                root.error==((flags&1u)?page::NativeCatalogRootError::encrypted_requires_crypto_authority:page::NativeCatalogRootError::cluster_requires_authority),"catalog/config/security root stops at actual bootstrap authority");
            Check(!calls[write_call]&&!calls[sync_call],"root reader performs no mutation");
            Arm();const auto history=page::ReadNativeCatalogRootRangeFromOpenDevices(Id(1),{{Id(2),r.bootstrap.page_size_profile_uuid,&device}},root_ref,root_ref,32*page_bytes);Disarm();
            if(!flags)Check(history.ok()&&history.roots.size()==1,"unprotected owning root range remains usable");
            else Check(!history.ok()&&history.roots.empty()&&!history.retained_image_bytes&&
                history.error==((flags&1u)?page::NativeCatalogRootError::encrypted_requires_crypto_authority:page::NativeCatalogRootError::cluster_requires_authority),"root history retains protected authority without prefix");
            Check(!calls[write_call]&&!calls[sync_call],"root history performs no mutation");
          }
        }
        Check(device.is_open()&&!calls[write_call]&&!calls[sync_call],"read refusal retains owner and performs no write/sync");
      }
      if(!index)for(const auto& root_ref:original.record->roots)if(root_ref.kind==2||root_ref.kind==6||root_ref.kind==7) {
        const auto plain=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),root_ref);
        Check(plain.ok(),"retain exact unprotected root for header-policy cases");
        for(const u32 flags:{2u,4u,8u,12u,14u}) {
          auto protected_root=*plain.root;protected_root.header.flags=flags;
          const auto encoded=page::EncodeNativeCatalogRoot(protected_root);
          Check(encoded.ok(),"structurally valid protected root image");
          Check(device.WriteAt(root_ref.page_number*page_bytes,encoded.bytes.data(),encoded.bytes.size()).ok()&&
              device.Sync().ok()&&device.Close().ok()&&device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"persist and reopen protected root header");
          Arm();const auto read=page::ReadNativeCatalogRootFromOpenDevice(device,Id(1),root_ref);Disarm();
          Check(!read.ok()&&!read.root&&read.bytes.empty()&&
              read.error==((flags&2u)?page::NativeCatalogRootError::cluster_requires_authority:page::NativeCatalogRootError::header_policy_requires_authority)&&
              !calls[write_call]&&!calls[sync_call]&&device.is_open(),"valid protected catalog root header confers no local authority");
        }
        Check(device.WriteAt(root_ref.page_number*page_bytes,plain.bytes.data(),plain.bytes.size()).ok()&&device.Sync().ok(),"restore exact unprotected root after policy cases");
      }
    }
  }
  std::error_code error;fs::remove_all(fixture.root,error);
  Check(!error&&!fs::exists(fixture.root),"authority test files cleaned after device closure");
}
}
