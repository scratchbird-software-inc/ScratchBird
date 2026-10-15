// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Physical hash-page/component composition. NOT transaction/security/runtime
// qualification. The bounded fixture container below is not a product format.
namespace blob_hash_page = scratchbird::storage::page;
using BlobHashId = scratchbird::core::platform::Uuid;
BlobHashId BlobHashFixtureId(unsigned n) {
  BlobHashId id{}; id.bytes[0]=1; id.bytes[6]=0x70; id.bytes[8]=0x80;
  id.bytes[14]=static_cast<byte>(n>>8); id.bytes[15]=static_cast<byte>(n);
  return id;
}
void BlobHashPut(std::span<byte> buffer,std::size_t at,std::uint64_t n) {
  for (unsigned i=0;i<8;++i) buffer[at+i]=static_cast<byte>(n>>(8*i));
}
void BlobHashFileWrite(disk::FileDevice& device,std::uint64_t& offset,std::span<const byte> bytes) {
  const auto written=device.WriteAt(offset,bytes.data(),bytes.size());
  Check(written.ok() && written.bytes_transferred==bytes.size(),"physical hash/component write failed");
  offset+=bytes.size();
}
void BlobHashFileRead(disk::FileDevice& device,std::uint64_t& offset,std::span<byte> bytes) {
  const auto read=device.ReadAt(offset,bytes.data(),bytes.size());
  Check(read.ok() && read.bytes_transferred==bytes.size(),"physical hash/component read failed");
  offset+=bytes.size();
}
std::vector<byte> BlobHashSource(unsigned row) {
  if (row<4) return BlobHashUnhex(kBlobComponentGoldenVectors[row+1].component_hex);
  if (row==4) return BlobHashUnhex(kBlobComponentGoldenVectors[3].component_hex); // another abc
  // Actual different content and digests; only its candidate is fault-collided.
  Fixture f;
  std::vector<byte> frame;
  {
    const std::array<byte,3> content{'a','b','d'};
    dt::BlobOwnedPersistenceBytesV3 source,encoded; f.Copy(content,source);
    auto grant=f.Grant();
    Check(dt::EncodeOwnedBlobComponentV3(f.profile,dt::BlobValueStateV3::value,
        source,grant,encoded).ok(),"collision source native encoding failed");
    frame.assign(encoded.bytes().begin(),encoded.bytes().end());
  }
  Clean(f); return frame;
}
dt::BlobHashCandidateFrameV3 BlobHashFixtureCandidate(
    const dt::BlobValidatedProfileHandleV3& profile,unsigned row,std::span<const byte> component) {
  dt::BlobHashCandidateFrameV3 candidate{};
  Check(dt::EncodeBlobHashCandidateNoAllocV3(profile,BlobHashGet(component,32),
      component.subspan(248,32),BlobHashFixtureId(row+100),1,1,candidate).ok(),
      "physical hash fixture candidate encode failed");
  if (row==5) {
    // Explicit candidate-collision fault. This is NOT a forged logical digest
    // or claimed authenticated provenance. Shape validation must not bless it
    // as equality; even a matching stored candidate requires real byte reads.
    const auto abc=BlobHashUnhex(kBlobHashGoldenVectors[2].projection_hex);
    std::copy(abc.begin(),abc.end(),candidate.begin()+232);
  }
  return candidate;
}
void BlobHashFileCreate(const std::filesystem::path& path,unsigned page_size) {
  Fixture f;
  auto created=blob_hash_page::InitializeIndexHashPhysicalIndex(
      {scratchbird::core::platform::UuidKind::object,BlobHashFixtureId(1)},page_size,0,
      blob_hash_page::kIndexHashHighAssuranceAlgorithmVersion,2);
  Check(created.ok(),"real BLOB candidate hash index creation failed");
  auto index=std::move(created.index);
  for (unsigned row=0;row<6;++row) {
    const auto frame=BlobHashSource(row);
    const auto candidate=BlobHashFixtureCandidate(f.profile,row,frame);
    const std::vector<byte> key(candidate.begin()+232,candidate.begin()+264);
    const auto route=blob_hash_page::LocateIndexHashBucket(index,key);
    Check(route.ok(),"native BLOB candidate bucket route failed");
    auto latch=blob_hash_page::AcquireIndexHashBucketExclusiveLatch(&index,route.bucket_page_number);
    Check(latch.active(),"actual candidate bucket latch missing");
    blob_hash_page::IndexHashPhysicalInsertRequest request;
    request.encoded_key=key;
    request.row_uuid={scratchbird::core::platform::UuidKind::row,BlobHashFixtureId(row+10)};
    request.version_uuid={scratchbird::core::platform::UuidKind::row,BlobHashFixtureId(row+100)};
    request.latch_evidence=latch.evidence();
    const auto inserted=blob_hash_page::InsertIndexHashEntry(&index,request);
    Check(inserted.ok() && inserted.inserted,"native BLOB candidate physical insertion failed");
  }
  const auto exported=blob_hash_page::ExportIndexHashPhysicalIndexImage(index);
  Check(exported.ok(),"native BLOB candidate physical export failed");
  const auto& image=exported.image;
  // A test-owned, bounded binary container preserves ALL native page images
  // and their actual seed/algorithm metadata for an independent exec reader.
  std::array<byte,96> header{};
  const std::array<byte,8> magic{'S','B','T','E','S','T','B','H'};
  std::copy(magic.begin(),magic.end(),header.begin());
  BlobHashPut(header,8,page_size); BlobHashPut(header,16,image.hash_algorithm_version);
  std::copy(image.index_uuid.value.bytes.begin(),image.index_uuid.value.bytes.end(),header.begin()+24);
  BlobHashPut(header,40,image.hash_seed); BlobHashPut(header,48,image.hash_seed_high64);
  BlobHashPut(header,56,image.directory_page_number); BlobHashPut(header,64,image.next_page_number);
  BlobHashPut(header,72,image.pages.size()); BlobHashPut(header,80,6);
  disk::FileDevice device;
  Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"hash fixture physical file creation failed");
  std::uint64_t offset=0; BlobHashFileWrite(device,offset,header);
  for (const auto& page:image.pages) {
    std::array<byte,16> record{};
    BlobHashPut(record,0,page.page_number); BlobHashPut(record,8,page.serialized.size());
    BlobHashFileWrite(device,offset,record); BlobHashFileWrite(device,offset,page.serialized);
  }
  for (unsigned row=0;row<6;++row) {
    const auto frame=BlobHashSource(row);
    const auto candidate=BlobHashFixtureCandidate(f.profile,row,frame);
    BlobHashFileWrite(device,offset,candidate);
    std::array<byte,8> length{}; BlobHashPut(length,0,frame.size());
    BlobHashFileWrite(device,offset,length); BlobHashFileWrite(device,offset,frame);
  }
  Check(device.Sync().ok(),"BLOB hash pages/components were not synchronized");
  Check(device.Close().ok(),"BLOB hash writer close failed"); Clean(f);
}
int BlobHashFileReopen(const char* path) {
  Fixture f; disk::FileDevice device;
  Check(device.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),"fresh-process BLOB hash open failed");
  std::uint64_t offset=0; std::array<byte,96> header{};
  BlobHashFileRead(device,offset,header);
  const std::array<byte,8> magic{'S','B','T','E','S','T','B','H'};
  Check(std::equal(magic.begin(),magic.end(),header.begin()),"wrong bounded physical fixture");
  const auto size=BlobHashGet(header,8);
  Check(size==8192 || size==16384 || size==32768 || size==65536 || size==131072,
        "unbounded physical hash fixture page size");
  blob_hash_page::IndexHashPhysicalIndexImage image;
  image.page_size=size; image.hash_algorithm_version=BlobHashGet(header,16);
  image.index_uuid.kind=scratchbird::core::platform::UuidKind::object;
  std::copy_n(header.begin()+24,16,image.index_uuid.value.bytes.begin());
  image.hash_seed=BlobHashGet(header,40); image.hash_seed_high64=BlobHashGet(header,48);
  image.directory_page_number=BlobHashGet(header,56); image.next_page_number=BlobHashGet(header,64);
  const auto pages=BlobHashGet(header,72);
  Check(pages>0 && pages<=16 && BlobHashGet(header,80)==6,"unbounded hash fixture inventory");
  for (std::uint64_t i=0;i<pages;++i) {
    std::array<byte,16> record{}; BlobHashFileRead(device,offset,record);
    const auto count=BlobHashGet(record,8);
    Check(count>0 && count<=size,"unbounded physical hash page length");
    blob_hash_page::IndexHashPhysicalPageImage page;
    page.page_number=BlobHashGet(record,0); page.serialized.resize(count);
    BlobHashFileRead(device,offset,page.serialized); image.pages.push_back(std::move(page));
  }
  auto imported=blob_hash_page::ImportIndexHashPhysicalIndexImage(image);
  Check(imported.ok(),"fresh-process actual hash page validation/import failed");
  struct Record {dt::BlobHashCandidateFrameV3 candidate{}; std::vector<byte> component;};
  std::array<Record,6> records;
  for (unsigned i=0;i<6;++i) {
    auto& record=records[i]; BlobHashFileRead(device,offset,record.candidate);
    Check(dt::ValidateBlobHashCandidateNoAllocV3(f.profile,record.candidate).ok() &&
        BlobHashGet(record.candidate,280)==1 && BlobHashGet(record.candidate,288)==1,
        "reopened candidate changed profile/reference/generations");
    Check(std::equal(record.candidate.begin()+264,record.candidate.begin()+280,
        BlobHashFixtureId(i+100).bytes.begin()),"reopened candidate lost exact binary version identity");
    std::array<byte,8> extent{}; BlobHashFileRead(device,offset,extent);
    const auto count=BlobHashGet(extent,0); Check(count>=320 && count<=65536,"unbounded component fixture extent");
    record.component.resize(count); BlobHashFileRead(device,offset,record.component);
    Check(record.component==BlobHashSource(i),"reopened BLOB content/native frame changed");
  }
  const auto extent=device.Size();
  Check(extent.ok() && extent.size_bytes==offset,"reopened BLOB hash fixture trailing/missing bytes");
  // Rebuild and stale profile substitutions are not accepted as current keys.
  auto stale=f.profile; stale.receipt.catalog_generation++;
  Check(!dt::ValidateBlobHashCandidateNoAllocV3(stale,records[2].candidate).ok(),"stale BLOB candidate profile admitted");
  const auto projection=BlobHashUnhex(kBlobHashGoldenVectors[2].projection_hex);
  const auto route=blob_hash_page::LocateIndexHashBucket(imported.index,projection);
  Check(route.ok(),"reopened BLOB hash route failed");
  auto latch=blob_hash_page::AcquireIndexHashBucketSharedLatch(imported.index,route.bucket_page_number);
  Check(latch.active(),"reopened BLOB hash read latch missing");
  blob_hash_page::IndexHashPhysicalProbeRequest request;
  request.encoded_key=projection; request.latch_evidence=latch.evidence();
  const auto found=blob_hash_page::ProbeIndexHashBucket(imported.index,request);
  Check(found.ok() && found.locators.size()==3,"reopened hash lost duplicate/collision candidates");
  for (const auto chunk:{1u,2u,3u,65536u}) {
    unsigned matches=0,rejected=0,rechecks=0; std::size_t octets=0;
    for (const auto& locator:found.locators) {
      Check(locator.mga_recheck_required && locator.security_recheck_required &&
          !locator.visibility_authority && !locator.authorization_authority &&
          !locator.transaction_finality_authority && !locator.recovery_authority,
          "physical hash candidate fabricated MGA/security/durability authority");
      const auto record=std::find_if(records.begin(),records.end(),[&](const auto& entry) {
        return std::equal(entry.candidate.begin()+264,entry.candidate.begin()+280,locator.version_uuid.value.bytes.begin());
      });
      Check(record!=records.end(),"hash result has no exact persisted binary recheck reference");
      {
        dt::BlobOwnedPersistenceBytesV3 source; f.Copy(record->component,source);
        auto grant=f.Grant(); dt::BlobOwnedPersistenceDecodedV3 decoded;
        Check(dt::DecodeOwnedBlobComponentV3(f.profile,source,false,grant,decoded,{chunk}).ok(),
              "candidate's actual persisted content failed integrity/readback");
        const auto content=decoded.content().bytes(); const std::array<byte,3> expected{'a','b','c'};
        bool same=content.size()==expected.size();
        for (std::size_t at=0;at<content.size();at+=std::min<std::size_t>(chunk,content.size()-at)) {
          const auto n=std::min<std::size_t>(chunk,content.size()-at);
          for (std::size_t i=0;i<n;++i) {
            same &= at+i<expected.size() && content[at+i]==expected[at+i]; ++octets;
          }
        }
        ++rechecks; if (same) ++matches; else ++rejected;
      }
      Clean(f);
    }
    Check(rechecks==3 && matches==2 && rejected==1 && octets==9,
          "digest-only equality or missing full duplicate/collision byte recheck");
  }
  Check(device.Close().ok(),"fresh-process BLOB hash close failed"); return 0;
}
void BlobHashFilePersistence(const char* executable) {
  scratchbird::tests::OwnedTempDirectory temporary;
  for (const unsigned size:{8192,16384,32768,65536,131072}) {
    const auto path=temporary.path()/std::to_string(size);
    BlobHashFileCreate(path,size);
    const auto pid=::fork(); Check(pid>=0,"fresh-process BLOB hash fork failed");
    if (pid==0) {::execl(executable,executable,"--reopen-hash",path.c_str(),static_cast<char*>(nullptr)); ::_exit(127);}
    int status=0;
    Check(::waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0,
          "independent physical BLOB hash persistence/recheck failed");
  }
  temporary.Cleanup();
}
