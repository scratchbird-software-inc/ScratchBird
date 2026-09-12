// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_filespace_record_codec.hpp"
#include "catalog_record_codec.hpp"
#include "catalog_page.hpp"
#include "database_lifecycle.hpp"
#include "startup_state.hpp"
#include "page_header.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
namespace c = scratchbird::core::catalog;
namespace p = scratchbird::core::platform;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace page = scratchbird::storage::page;
namespace uuid = scratchbird::core::uuid;
namespace fs = std::filesystem;
unsigned checks=0, failures=0;
void Check(bool ok,const char* message) {
  ++checks;
  if (!ok && ++failures < 20) std::cerr << "FAIL " << message << '\n';
}
void Setup(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
std::string Bytes(const c::CatalogValueEncodeResult& r) { return {r.bytes.begin(),r.bytes.end()}; }
void Put(std::string& s,std::size_t offset,p::u64 value,unsigned count) {
  for(unsigned i=0;i<count;++i) s[offset+i]=static_cast<char>((value>>(8*i))&255);
}
std::string Golden(const c::CatalogFilespaceRecord& r) {
  // Independent Core field offsets, widths and tags; no encoder/schema calls.
  std::string s(464, '\0');
  s.replace(0,4,"SBCV");
  Put(s,4,1,2); Put(s,6,24,2); Put(s,8,464,4); Put(s,12,37,4);
  Put(s,16,65538,4); Put(s,20,1,2);
  Put(s,24,1,2); Put(s,26,5,1); Put(s,28,16,4);
  s.replace(32,16,reinterpret_cast<const char*>(r.filespace_uuid.value.bytes.data()),16);
  Put(s,48,2,2); Put(s,50,5,1); Put(s,52,16,4);
  s.replace(56,16,reinterpret_cast<const char*>(r.database_uuid.value.bytes.data()),16);
  Put(s,72,3,2); Put(s,74,1,1); Put(s,76,8,4);
  Put(s,80,r.filespace_role,8);
  Put(s,88,4,2); Put(s,90,2,1); Put(s,92,1,4);
  Put(s,96,r.first_filespace,1);
  Put(s,97,5,2); Put(s,99,2,1); Put(s,101,1,4);
  Put(s,105,r.startup_authority,1);
  Put(s,106,6,2); Put(s,108,2,1); Put(s,110,1,4);
  Put(s,114,r.catalog_persistence_owner,1);
  Put(s,115,7,2); Put(s,117,2,1); Put(s,119,1,4);
  Put(s,123,r.filespace_manifest_owner,1);
  Put(s,124,8,2); Put(s,126,2,1); Put(s,128,1,4);
  Put(s,132,r.recovery_evidence_owner,1);
  Put(s,133,9,2); Put(s,135,2,1); Put(s,137,1,4);
  Put(s,141,r.read_only,1);
  Put(s,142,10,2); Put(s,144,1,1); Put(s,146,8,4);
  Put(s,150,r.state,8);
  Put(s,158,11,2); Put(s,160,1,1); Put(s,162,8,4);
  Put(s,166,r.physical_filespace_id,8);
  Put(s,174,12,2); Put(s,176,1,1); Put(s,178,8,4);
  Put(s,182,r.lifecycle_generation,8);
  Put(s,190,13,2); Put(s,192,1,1); Put(s,194,8,4);
  Put(s,198,r.filespace_manifest_generation,8);
  Put(s,206,14,2); Put(s,208,1,1); Put(s,210,8,4);
  Put(s,214,r.catalog_manifest_format_version,8);
  Put(s,222,15,2); Put(s,224,1,1); Put(s,226,8,4);
  Put(s,230,r.resource_seed_manifest_format_version,8);
  Put(s,238,16,2); Put(s,240,1,1); Put(s,242,8,4);
  Put(s,246,r.registered_txn,8);
  Put(s,254,17,2); Put(s,256,1,1); Put(s,258,8,4);
  Put(s,262,r.last_lifecycle_transaction,8);
  Put(s,270,18,2); Put(s,272,1,1); Put(s,274,8,4);
  Put(s,278,r.uuid_source,8);
  Put(s,286,19,2); Put(s,288,2,1); Put(s,290,1,4);
  Put(s,294,r.header_database_uuid_match_required,1);
  Put(s,295,20,2); Put(s,297,2,1); Put(s,299,1,4);
  Put(s,303,r.header_filespace_uuid_match_required,1);
  Put(s,304,21,2); Put(s,306,2,1); Put(s,308,1,4);
  Put(s,312,r.startup_state_coupled,1);
  Put(s,313,22,2); Put(s,315,2,1); Put(s,317,1,4);
  Put(s,321,r.page_header_coupled,1);
  Put(s,322,23,2); Put(s,324,2,1); Put(s,326,1,4);
  Put(s,330,r.open_validate_header,1);
  Put(s,331,24,2); Put(s,333,2,1); Put(s,335,1,4);
  Put(s,339,r.attach_admission_validate_header,1);
  Put(s,340,25,2); Put(s,342,2,1); Put(s,344,1,4);
  Put(s,348,r.transaction_admission_validate_filespace,1);
  Put(s,349,26,2); Put(s,351,2,1); Put(s,353,1,4);
  Put(s,357,r.maintenance_validate_header,1);
  Put(s,358,27,2); Put(s,360,2,1); Put(s,362,1,4);
  Put(s,366,r.verify_repair_validate_header,1);
  Put(s,367,28,2); Put(s,369,2,1); Put(s,371,1,4);
  Put(s,375,r.shutdown_validate_header,1);
  Put(s,376,29,2); Put(s,378,2,1); Put(s,380,1,4);
  Put(s,384,r.recovery_validate_header,1);
  Put(s,385,30,2); Put(s,387,2,1); Put(s,389,1,4);
  Put(s,393,r.drop_requires_database_lifecycle,1);
  Put(s,394,31,2); Put(s,396,2,1); Put(s,398,1,4);
  Put(s,402,r.quarantine_on_ambiguous,1);
  Put(s,403,32,2); Put(s,405,2,1); Put(s,407,1,4);
  Put(s,411,r.state_change_evidence_before_success,1);
  Put(s,412,33,2); Put(s,414,2,1); Put(s,416,1,4);
  Put(s,420,r.mga_visibility_required,1);
  Put(s,421,34,2); Put(s,423,2,1); Put(s,425,1,4);
  Put(s,429,r.path_is_locator_not_identity,1);
  Put(s,430,35,2); Put(s,432,2,1); Put(s,434,1,4);
  Put(s,438,r.duplicate_identity_refusal,1);
  Put(s,439,36,2); Put(s,441,2,1); Put(s,443,1,4);
  Put(s,447,r.stale_identity_refusal,1);
  Put(s,448,37,2); Put(s,450,1,1); Put(s,452,8,4);
  Put(s,456,r.creator_transaction_number,8);
  return s;
}
void Roundtrip(const c::CatalogFilespaceRecord& r) {
  const auto encoded=c::EncodeCatalogFilespaceRecord(r);
  Check(encoded.ok(),"valid payload encode");
  if(!encoded.ok())return;
  Check(Bytes(encoded)==Golden(r),"independent complete payload byte oracle");
  const auto decoded=c::DecodeCatalogFilespaceRecord(Bytes(encoded));
  Check(decoded.ok(),"valid payload decode");
  if(decoded.ok())Check(Golden(*decoded.record)==Golden(r),"all fields preserved");
}
void Refused(const std::string& bytes) {
  const auto d=c::DecodeCatalogFilespaceRecord(bytes);
  Check(!d.ok()&&!d.record.has_value(),"malformed payload returned authority");
}
void Codec(const c::CatalogFilespaceRecord& base) {
  Roundtrip(base);
  const auto valid=Golden(base);
  for(std::size_t size=0;size<valid.size();++size) Refused(valid.substr(0,size));
  Refused(valid+"x"); Refused(std::string(464,'x'));
  for(unsigned slot=0;slot<2;++slot) {
    for(unsigned kind=0;kind<256;++kind) {
      auto r=base; auto& id=slot==0?r.filespace_uuid:r.database_uuid;
      id.kind=static_cast<p::UuidKind>(kind);
      if(id.kind==(slot==0?p::UuidKind::filespace:p::UuidKind::database)) Roundtrip(r);
      else Check(!c::EncodeCatalogFilespaceRecord(r).ok(),"wrong identity kind encoded");
    }
    for(unsigned version=0;version<16;++version) for(unsigned variant=0;variant<4;++variant) {
      if(version==7&&variant==2)continue;
      auto r=base; auto& id=slot==0?r.filespace_uuid:r.database_uuid;
      id.value.bytes[6]=(id.value.bytes[6]&15)|(version<<4);
      id.value.bytes[8]=(id.value.bytes[8]&63)|(variant<<6);
      Check(!c::EncodeCatalogFilespaceRecord(r).ok(),"invalid identity encoded");
      Refused(Golden(r));
    }
    auto r=base; (slot==0?r.filespace_uuid:r.database_uuid).value={};
    Check(!c::EncodeCatalogFilespaceRecord(r).ok(),"nil identity encoded"); Refused(Golden(r));
  }
  const std::array<unsigned,37> offsets{24,48,72,88,97,106,115,124,133,142,158,174,190,206,222,238,254,270,286,295,304,313,322,331,340,349,358,367,376,385,394,403,412,421,430,439,448};
  const std::array<unsigned,37> tags{5,5,1,2,2,2,2,2,2,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,1};
  for(unsigned i=0;i<37;++i) {
    const auto offset=offsets[i];
    for(unsigned type=0;type<256;++type) {
      if(type==tags[i])continue;
      auto s=valid; Put(s,offset+2,type,1); Refused(s);
    }
    for(unsigned id : {0u,65535u}) {auto s=valid;Put(s,offset,id,2);Refused(s);}
    if(i>0) {auto s=valid;Put(s,offset,i,2);Refused(s);}
    auto s=valid;Put(s,offset+3,1,1);Refused(s);
    s=valid;Put(s,offset+4,0xffffffffu,4);Refused(s);
    if(tags[i]==2) {
      for(unsigned value=2;value<256;++value) {s=valid;Put(s,offset+8,value,1);Refused(s);}
    }
  }
  for(unsigned offset : {0,4,6,8,12,16,20,22}) {auto s=valid;Put(s,offset,0xff,1);Refused(s);}
  for(unsigned offset : {166,214,230}) {
    auto s=valid;Put(s,offset,0x100000000ull,8);Refused(s);
  }
  for(unsigned offset : {80,150,182,198,214,230,246,262,278,456}) {
    auto s=valid;Put(s,offset,0,8);Refused(s);
  }
  for(unsigned offset : {80,150,278}) {
    auto s=valid;Put(s,offset,17,8);Refused(s);
  }
  // Every field is round-tripped with a changed valid value as well.
  {auto r=base; r.filespace_role=r.filespace_role+1; Roundtrip(r);}
  {auto r=base; r.first_filespace=!r.first_filespace; Roundtrip(r);}
  {auto r=base; r.startup_authority=!r.startup_authority; Roundtrip(r);}
  {auto r=base; r.catalog_persistence_owner=!r.catalog_persistence_owner; Roundtrip(r);}
  {auto r=base; r.filespace_manifest_owner=!r.filespace_manifest_owner; Roundtrip(r);}
  {auto r=base; r.recovery_evidence_owner=!r.recovery_evidence_owner; Roundtrip(r);}
  {auto r=base; r.read_only=!r.read_only; Roundtrip(r);}
  {auto r=base; r.state=r.state+1; Roundtrip(r);}
  {auto r=base; r.physical_filespace_id=r.physical_filespace_id+1; Roundtrip(r);}
  {auto r=base; r.lifecycle_generation=r.lifecycle_generation+1; Roundtrip(r);}
  {auto r=base; r.filespace_manifest_generation=r.filespace_manifest_generation+1; Roundtrip(r);}
  {auto r=base; r.catalog_manifest_format_version=r.catalog_manifest_format_version+1; Roundtrip(r);}
  {auto r=base; r.resource_seed_manifest_format_version=r.resource_seed_manifest_format_version+1; Roundtrip(r);}
  {auto r=base; r.registered_txn=r.registered_txn+1; Roundtrip(r);}
  {auto r=base; r.last_lifecycle_transaction=r.last_lifecycle_transaction+1; Roundtrip(r);}
  {auto r=base; r.header_database_uuid_match_required=!r.header_database_uuid_match_required; Roundtrip(r);}
  {auto r=base; r.header_filespace_uuid_match_required=!r.header_filespace_uuid_match_required; Roundtrip(r);}
  {auto r=base; r.startup_state_coupled=!r.startup_state_coupled; Roundtrip(r);}
  {auto r=base; r.page_header_coupled=!r.page_header_coupled; Roundtrip(r);}
  {auto r=base; r.open_validate_header=!r.open_validate_header; Roundtrip(r);}
  {auto r=base; r.attach_admission_validate_header=!r.attach_admission_validate_header; Roundtrip(r);}
  {auto r=base; r.transaction_admission_validate_filespace=!r.transaction_admission_validate_filespace; Roundtrip(r);}
  {auto r=base; r.maintenance_validate_header=!r.maintenance_validate_header; Roundtrip(r);}
  {auto r=base; r.verify_repair_validate_header=!r.verify_repair_validate_header; Roundtrip(r);}
  {auto r=base; r.shutdown_validate_header=!r.shutdown_validate_header; Roundtrip(r);}
  {auto r=base; r.recovery_validate_header=!r.recovery_validate_header; Roundtrip(r);}
  {auto r=base; r.drop_requires_database_lifecycle=!r.drop_requires_database_lifecycle; Roundtrip(r);}
  {auto r=base; r.quarantine_on_ambiguous=!r.quarantine_on_ambiguous; Roundtrip(r);}
  {auto r=base; r.state_change_evidence_before_success=!r.state_change_evidence_before_success; Roundtrip(r);}
  {auto r=base; r.mga_visibility_required=!r.mga_visibility_required; Roundtrip(r);}
  {auto r=base; r.path_is_locator_not_identity=!r.path_is_locator_not_identity; Roundtrip(r);}
  {auto r=base; r.duplicate_identity_refusal=!r.duplicate_identity_refusal; Roundtrip(r);}
  {auto r=base; r.stale_identity_refusal=!r.stale_identity_refusal; Roundtrip(r);}
  {auto r=base; r.creator_transaction_number=r.creator_transaction_number+1; Roundtrip(r);}
  auto limits=base;
  limits.lifecycle_generation=~p::u64{0};
  limits.filespace_manifest_generation=~p::u64{0};
  limits.registered_txn=~p::u64{0};
  limits.last_lifecycle_transaction=~p::u64{0};
  limits.creator_transaction_number=~p::u64{0};
  Roundtrip(limits);
}
std::string ReadAll(const std::string& path) {
  std::ifstream in(path,std::ios::binary);Setup(in.is_open(),"read fixture");
  return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
p::u64 Fnv(const std::string& s) {
  p::u64 v=1469598103934665603ull;
  for(unsigned char b:s) {v^=b;v*=1099511628211ull;}
  return v;
}
int main() {
  fs::path root;
  try {
    const auto millis=static_cast<p::u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto id=uuid::GenerateEngineIdentityV7(p::UuidKind::database,millis);
    const auto fsid=uuid::GenerateEngineIdentityV7(p::UuidKind::filespace,millis);
    Setup(id.ok()&&fsid.ok(),"identity generation");
    root=fs::temp_directory_path()/("scratchbird_filespace_payload_"+uuid::UuidToString(id.value.value));
    Setup(fs::create_directory(root),"unique fixture directory");
    db::DatabaseCreateConfig cfg;
    cfg.path=(root/"primary.sdb").string();cfg.database_uuid=id.value;cfg.filespace_uuid=fsid.value;
    cfg.page_size=16384;cfg.creation_unix_epoch_millis=millis;
    cfg.allow_minimal_resource_bootstrap=true;cfg.require_resource_seed_pack=false;
    c::CatalogFilespaceRecord expected;
    expected.filespace_uuid=cfg.filespace_uuid;
    expected.database_uuid=cfg.database_uuid;
    expected.filespace_role=1;
    expected.first_filespace=true;
    expected.startup_authority=true;
    expected.catalog_persistence_owner=true;
    expected.filespace_manifest_owner=true;
    expected.recovery_evidence_owner=true;
    expected.read_only=false;
    expected.state=1;
    expected.physical_filespace_id=0;
    expected.lifecycle_generation=1;
    expected.filespace_manifest_generation=1;
    expected.catalog_manifest_format_version=1;
    expected.resource_seed_manifest_format_version=1;
    expected.registered_txn=1;
    expected.last_lifecycle_transaction=1;
    expected.uuid_source=1;
    expected.header_database_uuid_match_required=true;
    expected.header_filespace_uuid_match_required=true;
    expected.startup_state_coupled=true;
    expected.page_header_coupled=true;
    expected.open_validate_header=true;
    expected.attach_admission_validate_header=true;
    expected.transaction_admission_validate_filespace=true;
    expected.maintenance_validate_header=true;
    expected.verify_repair_validate_header=true;
    expected.shutdown_validate_header=true;
    expected.recovery_validate_header=true;
    expected.drop_requires_database_lifecycle=true;
    expected.quarantine_on_ambiguous=true;
    expected.state_change_evidence_before_success=true;
    expected.mga_visibility_required=true;
    expected.path_is_locator_not_identity=true;
    expected.duplicate_identity_refusal=true;
    expected.stale_identity_refusal=true;
    expected.creator_transaction_number=1;
    Codec(expected);
    const auto created=db::CreateDatabaseFile(cfg);
    if(!created.ok())std::cerr<<created.diagnostic.diagnostic_code<<'\n';
    Setup(created.ok(),"actual database create failed");
    db::DatabaseOpenConfig open;open.path=cfg.path;open.read_only=true;open.suppress_background_agents=true;
    const auto opened=db::OpenDatabaseFile(open);
    if(!opened.ok())std::cerr<<opened.diagnostic.diagnostic_code<<'\n';
    Check(opened.ok(),"fresh database opens with binary identity payload");
    p::u64 found_page=0;std::size_t found_offset=0;
    std::vector<p::byte> original;
    {
      disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing).ok(),"open real device");
      p::u64 number=db::kCatalogPageNumber;
      for(unsigned guard=0;number!=0&&guard<1000;++guard) {
        std::vector<p::byte> body(cfg.page_size-disk::kPageHeaderSerializedBytes);
        Setup(device.ReadAt(number*cfg.page_size+disk::kPageHeaderSerializedBytes,body.data(),body.size()).ok(),"read catalog page");
        const auto parsed=page::ParseCatalogPageBody(body,number);Setup(parsed.ok(),"parse real page");
        std::size_t offset=page::kCatalogPageBodyHeaderBytes;
        for(const auto& row:parsed.body.rows) {
          if(row.kind==page::CatalogPageRowKind::typed_catalog_record) {
            const auto record=c::DecodeCatalogTypedRecord(row);Setup(record.ok(),"decode actual catalog record");
            if(record.record.header.kind==c::CatalogRecordKind::filespace) {
              Setup(found_page==0,"duplicate filespace record");
              found_page=number;found_offset=offset;original=body;
              Check(record.record.payload==Golden(expected),"actual persisted filespace payload differs from independent oracle");
              Check(record.record.payload.find(uuid::UuidToString(cfg.database_uuid.value))==std::string::npos &&
                    record.record.payload.find(uuid::UuidToString(cfg.filespace_uuid.value))==std::string::npos,
                    "filespace payload still stores identity as text");
            }
          }
          offset+=20+row.payload.size();
        }
        number=parsed.body.next_page_number;
      }
    }
    Setup(found_page!=0,"actual filespace record missing");
    const auto write = [&](const std::vector<p::byte>& body) {
      disk::FileDevice device;Setup(device.Open(cfg.path,disk::FileOpenMode::open_existing).ok(),"mutation device open");
      Setup(device.WriteAt(found_page*cfg.page_size+disk::kPageHeaderSerializedBytes,body.data(),body.size()).ok(),"test page mutation");
      Setup(device.Sync().ok(),"test page sync");
    };
    const auto mutate = [&](std::string payload) {
      auto body=original;
      const auto row_size=p::LoadLittle32(body.data()+found_offset+8);
      std::string record(reinterpret_cast<const char*>(body.data()+found_offset+20),row_size);
      Setup(payload.size()==464&&record.size()==96+464,"fixed fixture size");
      record.replace(96,464,payload);
      std::copy(record.begin(),record.end(),body.begin()+found_offset+20);
      p::StoreLittle64(body.data()+found_offset+12,Fnv(record));
      p::StoreLittle64(body.data()+40,page::ComputeCatalogPageBodyChecksum(body));
      Setup(page::ParseCatalogPageBody(body,found_page).ok(),"mutated page must retain valid checksums");
      write(body);
      const auto before=ReadAll(cfg.path);
      const auto result=db::OpenDatabaseFile(open);
      Check(!result.ok(),"checksum-valid invalid filespace payload admitted");
      Check(ReadAll(cfg.path)==before,"refused read-only admission changed database");
      auto writable=open; writable.read_only=false;
      Check(!db::OpenDatabaseFile(writable).ok(),"invalid payload admitted writable");
      Check(ReadAll(cfg.path)==before,"failed writable admission changed database");
      write(original);
      Check(db::OpenDatabaseFile(open).ok(),"valid identity payload did not recover after refusal");
    };
    {auto foreign=expected;foreign.database_uuid=uuid::GenerateEngineIdentityV7(p::UuidKind::database,millis+1).value;
      mutate(Golden(foreign));}
    {auto bytes=Golden(expected);bytes[16]^=1;mutate(bytes);}
    {auto bytes=Golden(expected);bytes[32+6]&=15;mutate(bytes);}
    {auto foreign=expected;foreign.filespace_uuid=uuid::GenerateEngineIdentityV7(p::UuidKind::filespace,millis+2).value;
      mutate(Golden(foreign));}
    {auto bytes=Golden(expected);bytes[56+6]&=15;mutate(bytes);}
    // Alter each primary manifest field with valid page and row checksums.
    {auto bytes=Golden(expected);Put(bytes,80,2,8);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,96,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,105,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,114,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,123,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,132,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,141,1,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,150,2,8);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,166,1,8);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,182,2,8);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,198,2,8);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,214,2,8);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,230,2,8);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,246,2,8);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,262,2,8);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,278,2,8);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,294,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,303,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,312,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,321,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,330,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,339,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,348,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,357,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,366,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,375,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,384,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,393,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,402,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,411,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,420,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,429,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,438,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,447,0,1);mutate(bytes);}
    {auto bytes=Golden(expected);Put(bytes,456,2,8);mutate(bytes);}
    mutate(std::string("filespace_uuid=legacy\n")+std::string(442,'x'));
    if(failures) {std::cerr<<"fixture retained at "<<root<<'\n';return 1;}
    fs::remove_all(root); // Only the unique test-owned root created above.
    std::cout<<"checks="<<checks<<" failures="<<failures<<" real_filespace_payload=passed\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"; fixture retained at "<<root<<'\n';return 1;}
}
