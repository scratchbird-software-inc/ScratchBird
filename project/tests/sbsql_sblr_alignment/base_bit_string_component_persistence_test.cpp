// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "datatype_binary_view.hpp"
#include "datatype_bit_string.hpp"
#include "datatype_bit_string_projection.hpp"
#include "datatype_physical_encoding.hpp"
#include "disk_device.hpp"
#include "hash_digest.hpp"

#include <array>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <iostream>

namespace dt=scratchbird::core::datatypes;
namespace platform=scratchbird::core::platform;
namespace disk=scratchbird::storage::disk;
namespace fs=std::filesystem;
namespace {
void Require(bool c,const char* m){if(!c)throw std::runtime_error(m);}
platform::Uuid Nonzero(platform::byte v){platform::Uuid u{};u.bytes[0]=v;u.bytes[6]=0x70;u.bytes[8]=0x80;return u;}
dt::BitStringDescriptorProfileV3 Profile(){
  const platform::Uuid descriptor{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x29}};
  const auto historical=dt::LookupDatatypeTypeCodecIdentityV3(dt::kDatatypeCohortV6,6,6,descriptor,1);
  Require(historical.ok&&!dt::IsExactCanonicalBitStringTypeCodecIdentityV3(historical.row),"d706 historical bit identity");
  dt::BitStringProfileRequestV3 historical_request;historical_request.identity=historical.row;historical_request.receipt={Nonzero(6),dt::kDatatypeCohortV6,6,6};Require(!dt::BuildBitStringDescriptorProfileV3(historical_request).ok(),"d706 historical profile refusal");
  for(const auto& row:dt::CurrentDatatypeTypeCodecIdentityRowsV3())if(dt::IsExactCanonicalBitStringTypeCodecIdentityV3(row)){dt::BitStringProfileRequestV3 q;q.identity=row;q.receipt={Nonzero(7),row.legacy_fields.catalog_snapshot_uuid,row.legacy_fields.catalog_generation,row.legacy_fields.registry_generation};auto p=dt::BuildBitStringDescriptorProfileV3(q);Require(p.ok(),"profile");return p.profile;}throw std::runtime_error("identity");}
std::vector<platform::byte> Hex(const char* s){std::vector<platform::byte> out;for(;s[0]&&s[1];s+=2){auto n=[](char c){return c<='9'?c-'0':c-'a'+10;};out.push_back(static_cast<platform::byte>((n(s[0])<<4)|n(s[1])));}return out;}

std::vector<platform::byte> Oracle(
    const dt::BitStringDescriptorProfileV3& profile,
    dt::BitStringValueStateV3 state,
    const std::vector<platform::byte>& frame) {
  std::vector<platform::byte> record(200 + frame.size(), 0);
  std::memcpy(record.data(), "SBBTOR01", 8);
  platform::StoreLittle16(record.data() + 8, 1);
  platform::StoreLittle16(record.data() + 10, 200);
  record[12] = state == dt::BitStringValueStateV3::sql_null ? 1 : 0;
  const auto& id = profile.identity.legacy_fields;
  std::memcpy(record.data() + 16, profile.receipt.catalog_snapshot_uuid.bytes.data(), 16);
  platform::StoreLittle64(record.data() + 32, profile.receipt.catalog_generation);
  platform::StoreLittle64(record.data() + 40, profile.receipt.registry_generation);
  std::memcpy(record.data() + 48, id.descriptor_uuid.bytes.data(), 16);
  platform::StoreLittle64(record.data() + 64, id.descriptor_generation);
  std::memcpy(record.data() + 72, id.type_uuid.bytes.data(), 16);
  platform::StoreLittle64(record.data() + 88, id.type_generation);
  std::memcpy(record.data() + 96, id.codec_uuid.bytes.data(), 16);
  platform::StoreLittle32(record.data() + 112, id.codec_version);
  platform::StoreLittle64(record.data() + 120, id.codec_generation);
  std::memcpy(record.data() + 128, profile.profile_fingerprint.data(), 32);
  platform::StoreLittle64(record.data() + 160, frame.size());
  std::memcpy(record.data() + 200, frame.data(), frame.size());
  static constexpr char domain[] = "ScratchBird.BaseBitString.TestOracle.V1";
  std::vector<platform::byte> material;
  material.insert(material.end(), domain, domain + sizeof(domain) - 1);
  material.insert(material.end(), record.begin(), record.begin() + 168);
  material.insert(material.end(), frame.begin(), frame.end());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
  Require(digest.ok(), "oracle hash provider");
  std::memcpy(record.data() + 168, digest.digest.data(), digest.digest.size());
  return record;
}

enum class OracleResult { ok, frame_invalid, profile_invalid, integrity_failed, component_invalid };
OracleResult CheckOracle(const dt::BitStringDescriptorProfileV3& profile,
                         const std::vector<platform::byte>& record) {
  if (record.size() < 200 || std::memcmp(record.data(), "SBBTOR01", 8) != 0 ||
      platform::LoadLittle16(record.data() + 8) != 1 ||
      platform::LoadLittle16(record.data() + 10) != 200 ||
      record[12] > 1 || record[13] || record[14] || record[15] ||
      platform::LoadLittle32(record.data() + 116) != 0 ||
      platform::LoadLittle64(record.data() + 160) != record.size() - 200)
    return OracleResult::frame_invalid;
  const auto& id = profile.identity.legacy_fields;
  if (std::memcmp(record.data()+16,profile.receipt.catalog_snapshot_uuid.bytes.data(),16) ||
      platform::LoadLittle64(record.data()+32)!=profile.receipt.catalog_generation ||
      platform::LoadLittle64(record.data()+40)!=profile.receipt.registry_generation ||
      std::memcmp(record.data()+48,id.descriptor_uuid.bytes.data(),16) ||
      platform::LoadLittle64(record.data()+64)!=id.descriptor_generation ||
      std::memcmp(record.data()+72,id.type_uuid.bytes.data(),16) ||
      platform::LoadLittle64(record.data()+88)!=id.type_generation ||
      std::memcmp(record.data()+96,id.codec_uuid.bytes.data(),16) ||
      platform::LoadLittle32(record.data()+112)!=id.codec_version ||
      platform::LoadLittle64(record.data()+120)!=id.codec_generation ||
      std::memcmp(record.data()+128,profile.profile_fingerprint.data(),32))
    return OracleResult::profile_invalid;
  static constexpr char domain[]="ScratchBird.BaseBitString.TestOracle.V1";
  std::vector<platform::byte> material;
  material.insert(material.end(),domain,domain+sizeof(domain)-1);
  material.insert(material.end(),record.begin(),record.begin()+168);
  material.insert(material.end(),record.begin()+200,record.end());
  const auto digest=scratchbird::core::hash::ComputeSha256Digest(material);
  if(!digest.ok() || !std::equal(digest.digest.begin(),digest.digest.end(),record.begin()+168))
    return OracleResult::integrity_failed;
  const auto state=record[12]?dt::BitStringValueStateV3::sql_null:dt::BitStringValueStateV3::present;
  const auto decoded=dt::DecodeBitStringSbdvalComposedNoAllocV3(
      profile,true,std::span<const platform::byte>(record.data()+200,record.size()-200));
  if(!decoded.ok() || decoded.value.state!=state) return OracleResult::component_invalid;
  return OracleResult::ok;
}
}
int main(){
  Require(dt::kDatatypeBinaryPayloadChecksumV1Seed==1469598103934665603ull,"frozen seed");
  Require(dt::ComputeDatatypeBinaryPayloadChecksumV1(nullptr,0)==1469598103934665603ull,"empty checksum");
  Require(dt::kDatatypeBinaryPayloadChecksumV1Seed!=14695981039346656037ull,"nonstandard FNV terminology identity");
  auto profile=Profile();std::array<platform::byte,1> one{0x80};
  dt::BitStringValueViewV3 empty{&profile,dt::BitStringValueStateV3::present,0,{},dt::BitStringOwnershipV3::borrowed};
  dt::BitStringValueViewV3 onev{&profile,dt::BitStringValueStateV3::present,1,one,dt::BitStringOwnershipV3::borrowed};
  dt::BitStringValueViewV3 nullv{&profile,dt::BitStringValueStateV3::sql_null,0,{},dt::BitStringOwnershipV3::borrowed};
  const auto expected_empty=Hex("53424456414c30312e0100000000200004000000000000003331a286a046543100000000");
  const auto expected_one=Hex("53424456414c30312e010000000020000500000000000000c65549489ad2b5670100000080");
  const auto expected_null=Hex("53424456414c30312e01000001002000000000000000000083039d73b00f6514");
  auto ee=dt::EncodeBitStringSbdvalComposedV3(empty,true);auto eo=dt::EncodeBitStringSbdvalComposedV3(onev,true);auto en=dt::EncodeBitStringSbdvalComposedV3(nullv,true);
  Require(ee.ok()&&ee.bytes==expected_empty&&ee.bytes.size()==36,"empty SBDVAL");Require(eo.ok()&&eo.bytes==expected_one&&eo.bytes.size()==37,"one SBDVAL");Require(en.ok()&&en.bytes==expected_null&&en.bytes.size()==32,"null SBDVAL");
  for(const auto* frame:{&ee.bytes,&eo.bytes,&en.bytes}){auto structural=dt::DecodeDatatypeBinaryStructuralValueViewNoAlloc(frame->data(),frame->size());Require(structural.ok()&&structural.value.type_id==dt::CanonicalTypeId::bit_string,"structural SBDVAL");}
  Require(dt::DecodeBitStringSbdvalComposedNoAllocV3(profile,true,eo.bytes).ok(),"composed SBDVAL decode");
  auto oracle_empty=Oracle(profile,dt::BitStringValueStateV3::present,ee.bytes);
  auto oracle_one=Oracle(profile,dt::BitStringValueStateV3::present,eo.bytes);
  auto oracle_null=Oracle(profile,dt::BitStringValueStateV3::sql_null,en.bytes);
  Require(oracle_empty.size()==236&&oracle_one.size()==237&&oracle_null.size()==232,"SBBTOR01 exact extents");
  Require(oracle_empty==Hex("534242544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000019d000000007000800000000000d8290100000000000000019d000000007000800000000000d82a0100000000000000019d000000007000800000000000d82b010000000000000001000000000000006b591a716ed950c86c2559ff1d81912ce9d60af9a19b4bb34263d722e35743e8240000000000000009a5e3a1c8a3db3e77fcf7cc97e8e8bb9cd7b5c646830bf4de18b9c44740a10353424456414c30312e0100000000200004000000000000003331a286a046543100000000"),"sealed SBBTOR01 empty");
  Require(oracle_one==Hex("534242544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000019d000000007000800000000000d8290100000000000000019d000000007000800000000000d82a0100000000000000019d000000007000800000000000d82b010000000000000001000000000000006b591a716ed950c86c2559ff1d81912ce9d60af9a19b4bb34263d722e35743e82500000000000000c6715944bd3005fcc8b01b16d6c88e384a0e0827f0d886428b03a874ff95fbe053424456414c30312e010000000020000500000000000000c65549489ad2b5670100000080"),"sealed SBBTOR01 one");
  Require(oracle_null==Hex("534242544f5230310100c80001000000019d000000007000800000000000d7100a000000000000000a00000000000000019d000000007000800000000000d8290100000000000000019d000000007000800000000000d82a0100000000000000019d000000007000800000000000d82b010000000000000001000000000000006b591a716ed950c86c2559ff1d81912ce9d60af9a19b4bb34263d722e35743e82000000000000000d4e9557606cde93f9184a133b0990c0845c9b8d83282b74a070b85ccf2986a7953424456414c30312e01000001002000000000000000000083039d73b00f6514"),"sealed SBBTOR01 null");
  Require(CheckOracle(profile,oracle_empty)==OracleResult::ok&&CheckOracle(profile,oracle_one)==OracleResult::ok&&CheckOracle(profile,oracle_null)==OracleResult::ok,"SBBTOR01 positive vectors");
  auto reserved=oracle_one;reserved[13]=1;Require(reserved==Hex("534242544f5230310100c80000010000019d000000007000800000000000d7100a000000000000000a00000000000000019d000000007000800000000000d8290100000000000000019d000000007000800000000000d82a0100000000000000019d000000007000800000000000d82b010000000000000001000000000000006b591a716ed950c86c2559ff1d81912ce9d60af9a19b4bb34263d722e35743e82500000000000000c6715944bd3005fcc8b01b16d6c88e384a0e0827f0d886428b03a874ff95fbe053424456414c30312e010000000020000500000000000000c65549489ad2b5670100000080")&&CheckOracle(profile,reserved)==OracleResult::frame_invalid,"sealed SBBTOR01 reserved refusal");
  auto evidence=oracle_one;evidence[168]^=1;Require(evidence==Hex("534242544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000019d000000007000800000000000d8290100000000000000019d000000007000800000000000d82a0100000000000000019d000000007000800000000000d82b010000000000000001000000000000006b591a716ed950c86c2559ff1d81912ce9d60af9a19b4bb34263d722e35743e82500000000000000c7715944bd3005fcc8b01b16d6c88e384a0e0827f0d886428b03a874ff95fbe053424456414c30312e010000000020000500000000000000c65549489ad2b5670100000080")&&CheckOracle(profile,evidence)==OracleResult::integrity_failed,"sealed SBBTOR01 evidence refusal");
  std::vector<platform::byte> truncated(oracle_null.begin(),oracle_null.begin()+199);Require(truncated==Hex("534242544f5230310100c80001000000019d000000007000800000000000d7100a000000000000000a00000000000000019d000000007000800000000000d8290100000000000000019d000000007000800000000000d82a0100000000000000019d000000007000800000000000d82b010000000000000001000000000000006b591a716ed950c86c2559ff1d81912ce9d60af9a19b4bb34263d722e35743e82000000000000000d4e9557606cde93f9184a133b0990c0845c9b8d83282b74a070b85ccf2986a")&&CheckOracle(profile,truncated)==OracleResult::frame_invalid,"sealed SBBTOR01 truncated refusal");
  std::array<platform::byte,5> dirty_component{1,0,0,0,0x81};std::vector<platform::byte> dirty_frame(37);auto dirty_structural=dt::EncodeDatatypeBinaryStructuralValueIntoNoAlloc({dt::CanonicalTypeId::bit_string,false,false,dirty_component.data(),dirty_component.size()},dirty_frame.data(),dirty_frame.size());Require(dirty_structural.ok(),"dirty structural fixture");auto dirty_oracle=Oracle(profile,dt::BitStringValueStateV3::present,dirty_frame);Require(dirty_oracle==Hex("534242544f5230310100c80000000000019d000000007000800000000000d7100a000000000000000a00000000000000019d000000007000800000000000d8290100000000000000019d000000007000800000000000d82a0100000000000000019d000000007000800000000000d82b010000000000000001000000000000006b591a716ed950c86c2559ff1d81912ce9d60af9a19b4bb34263d722e35743e82500000000000000ebe983070fed7f6d986a7d575d4683eac3a4d346239113c2e65528025a38eb6b53424456414c30312e010000000020000500000000000000795749489ad3b5670100000081")&&CheckOracle(profile,dirty_oracle)==OracleResult::component_invalid,"sealed SBBTOR01 dirty-tail refusal");
  auto stale_oracle=oracle_one;stale_oracle[48]^=1;Require(CheckOracle(profile,stale_oracle)==OracleResult::profile_invalid,"SBBTOR01 profile refusal");
  auto corrupt=eo.bytes;corrupt.back()^=1;Require(!dt::DecodeDatatypeBinaryStructuralValueViewNoAlloc(corrupt.data(),corrupt.size()).ok(),"checksum corruption");
  auto corrupt_null_payload=eo.bytes;platform::StoreLittle16(corrupt_null_payload.data()+12,1);corrupt_null_payload[24]^=1;auto binary_precedence=dt::DecodeDatatypeBinaryStructuralValueViewNoAlloc(corrupt_null_payload.data(),corrupt_null_payload.size());Require(!binary_precedence.ok()&&binary_precedence.diagnostic.diagnostic_code=="CTB.BINARY.INTEGRITY_FAILED","SBDVAL checksum precedes payload-bearing NULL");
  dt::DatatypeBinaryValue raw{dt::CanonicalTypeId::bit_string,false,false,{1,0,0,0,0x80}};auto raw_encoded=dt::EncodeDatatypeBinaryValue(raw);Require(!raw_encoded.ok()&&raw_encoded.diagnostic.diagnostic_code=="CTB.BIT.SERIALIZATION_PROFILE_MISSING","raw SBDVAL refusal");
  auto raw_null=dt::EncodeDatatypeBinaryValue({dt::CanonicalTypeId::bit_string,true,false,{}});Require(!raw_null.ok()&&raw_null.diagnostic.diagnostic_code=="CTB.BIT.SERIALIZATION_PROFILE_MISSING","raw null SBDVAL refusal");
  auto pe=dt::EncodeBitStringSbdpvComposedV3(onev,true);Require(pe.ok(),"composed SBDPV");auto ps=dt::DecodeDatatypePhysicalStructuralValueViewNoAlloc(pe.bytes.data(),pe.bytes.size());Require(ps.ok()&&ps.value.type_id==dt::CanonicalTypeId::bit_string,"structural SBDPV");Require(dt::DecodeBitStringSbdpvComposedNoAllocV3(profile,true,pe.bytes).ok(),"composed SBDPV decode");
  auto corrupt_physical_null=pe.bytes;platform::StoreLittle16(corrupt_physical_null.data()+12,static_cast<platform::u16>(dt::DatatypePhysicalValueState::sql_null));corrupt_physical_null[20]^=1;auto physical_precedence=dt::DecodeDatatypePhysicalStructuralValueViewNoAlloc(corrupt_physical_null.data(),corrupt_physical_null.size());Require(!physical_precedence.ok()&&physical_precedence.diagnostic.diagnostic_code=="SB-DATATYPE-PHYSICAL-CHECKSUM-MISMATCH","SBDPV checksum precedes payload-bearing NULL");
  dt::DatatypePhysicalValue praw{dt::CanonicalTypeId::bit_string,dt::DatatypePhysicalValueState::value,{1,0,0,0,0x80}};auto pre=dt::EncodeDatatypePhysicalValue(praw);Require(!pre.ok()&&pre.diagnostic.diagnostic_code=="CTB.BIT.SERIALIZATION_PROFILE_MISSING","raw SBDPV refusal");
  auto pre_null=dt::EncodeDatatypePhysicalValue({dt::CanonicalTypeId::bit_string,dt::DatatypePhysicalValueState::sql_null,{}});Require(!pre_null.ok()&&pre_null.diagnostic.diagnostic_code=="CTB.BIT.SERIALIZATION_PROFILE_MISSING","raw null SBDPV refusal");
  std::vector<platform::byte> image;for(auto* f:{&ee.bytes,&eo.bytes,&en.bytes,&pe.bytes}){platform::byte n[4];platform::StoreLittle32(n,f->size());image.insert(image.end(),n,n+4);image.insert(image.end(),f->begin(),f->end());}
  auto backup=dt::EncodeBitStringBackupTupleV3(onev);Require(backup.ok(),"backup tuple encode");const auto backup_offset=image.size();std::array<platform::byte,70> backup_bytes{};std::memcpy(backup_bytes.data(),backup.tuple.catalog_snapshot_uuid.bytes.data(),16);platform::StoreLittle64(backup_bytes.data()+16,backup.tuple.catalog_generation);platform::StoreLittle64(backup_bytes.data()+24,backup.tuple.registry_generation);std::memcpy(backup_bytes.data()+32,backup.tuple.profile_fingerprint.data(),32);backup_bytes[64]=static_cast<platform::byte>(backup.tuple.state);platform::StoreLittle32(backup_bytes.data()+65,backup.tuple.logical_bit_count);backup_bytes[69]=backup.tuple.packed_msb0[0];image.insert(image.end(),backup_bytes.begin(),backup_bytes.end());
  auto path=fs::temp_directory_path()/"sb-bit-string-component.test";std::error_code ec;fs::remove(path,ec);disk::FileDevice writer;Require(writer.Open(path.string(),disk::FileOpenMode::create_new).ok(),"open write");auto wr=writer.WriteAt(0,image.data(),image.size());Require(wr.ok()&&wr.bytes_transferred==image.size()&&writer.Sync().ok()&&writer.Close().ok(),"write close");disk::FileDevice reader;Require(reader.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok(),"reopen");std::vector<platform::byte> restored(image.size());auto rr=reader.ReadAt(0,restored.data(),restored.size());Require(rr.ok()&&reader.Close().ok()&&restored==image,"reopen exact");dt::BitStringOwnedBackupTupleV3 reopened_backup;std::memcpy(reopened_backup.catalog_snapshot_uuid.bytes.data(),restored.data()+backup_offset,16);reopened_backup.catalog_generation=platform::LoadLittle64(restored.data()+backup_offset+16);reopened_backup.registry_generation=platform::LoadLittle64(restored.data()+backup_offset+24);std::memcpy(reopened_backup.profile_fingerprint.data(),restored.data()+backup_offset+32,32);reopened_backup.state=static_cast<dt::BitStringValueStateV3>(restored[backup_offset+64]);reopened_backup.logical_bit_count=platform::LoadLittle32(restored.data()+backup_offset+65);reopened_backup.packed_msb0.assign(restored.begin()+backup_offset+69,restored.end());auto reopened_value=dt::DecodeBitStringBackupTupleV3(profile,reopened_backup,true);Require(reopened_value.ok()&&reopened_value.value.state==dt::BitStringValueStateV3::present&&reopened_value.value.logical_bit_count==1&&reopened_value.value.packed_msb0==std::vector<platform::byte>{0x80},"backup tuple persistence reopen");fs::remove(path,ec);
  std::vector<platform::byte> maximum_component(2'097'156,0);platform::StoreLittle32(maximum_component.data(),16'777'216);dt::DatatypeBinaryValueView maxraw{dt::CanonicalTypeId::bit_string,false,false,maximum_component.data(),maximum_component.size()};std::vector<platform::byte> maxframe(2'097'188);auto mx=dt::EncodeDatatypeBinaryStructuralValueIntoNoAlloc(maxraw,maxframe.data(),maxframe.size());Require(mx.ok()&&mx.bytes_written==2'097'188,"maximum structural extent");Require(200+mx.bytes_written==2'097'388,"maximum oracle extent");
  std::cout<<"base_bit_string_component_persistence=passed\n";
}
