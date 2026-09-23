// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_reservation_release_coordinator.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
namespace a=scratchbird::engine::internal_api;
static void Check(bool value,std::source_location at=std::source_location::current()) {
  if(!value){std::cerr<<"failure at "<<at.line()<<'\n';std::abort();}
}
static a::EngineUuid Id(unsigned n){a::EngineUuid id;id.bytes={1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};return id;}
int main(){
  auto pattern=(std::filesystem::temp_directory_path()/"sb-reservation-binary-XXXXXX").string();
  auto* root=mkdtemp(pattern.data());Check(root);
  struct Cleanup{std::filesystem::path path;~Cleanup(){std::filesystem::remove_all(path);}}cleanup{root};
  a::EngineRequestContext c;c.database_path=(cleanup.path/"database").string();
  c.database_uuid=Id(1);c.transaction_uuid=Id(2);c.statement_uuid=Id(3);c.local_transaction_id=11;
  c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;
  c.trace_tags={"private_transaction_relation_reservation","right:SBLR_TRANSACTION_RESERVATION_ADMIN"};
  auto publish=[&](const auto& context){return a::PublishSblrRelationReservation(context,context.statement_uuid,Id(4),1,2,3);};
  auto published=publish(c);Check(published.ok);const auto& p=published.snapshot;
  Check(p.database_uuid==c.database_uuid&&p.transaction_uuid==c.transaction_uuid&&p.relation_uuid==Id(4));
  Check(a::CoordinateSblrReservationRelease(c,c.statement_uuid,Id(4),1,7).ok);
  auto release=[&](const auto& context,const auto& snap){return a::ReleaseSblrRelationReservation(context,snap.reservation_uuid,snap.reservation_generation,snap.relation_uuid,snap.reservation_evidence_sha256,7);};
  for(unsigned bit=0;bit<128;++bit){
    auto wrong=c;wrong.database_uuid.bytes[bit/8]^=1u<<(bit%8);Check(!release(wrong,p).ok);
    wrong=c;wrong.transaction_uuid.bytes[bit/8]^=1u<<(bit%8);Check(!release(wrong,p).ok);
    auto wrong_snapshot=p;wrong_snapshot.reservation_uuid.bytes[bit/8]^=1u<<(bit%8);Check(!release(c,wrong_snapshot).ok);
    wrong_snapshot=p;wrong_snapshot.relation_uuid.bytes[bit/8]^=1u<<(bit%8);Check(!release(c,wrong_snapshot).ok);
  }
  const auto journal=c.database_path+".sb.sblr_relation_reservation.v2";
  const auto saved=cleanup.path/"saved";
  std::filesystem::rename(journal,saved);std::filesystem::create_directory(journal);
  Check(!release(c,p).ok);
  Check(a::CoordinateSblrReservationRelease(c,c.statement_uuid,Id(4),1,7).ok);
  std::filesystem::remove(journal);std::filesystem::rename(saved,journal);
  Check(release(c,p).ok);Check(!release(c,p).ok);
  Check(!std::filesystem::exists(c.database_path+".sb.sblr_relation_reservation.v1"));
  std::ifstream input(journal,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(input)),{});
  Check(bytes.size()==294);
  for(unsigned n=0;n<2;++n){
    auto offset=n*147;Check(bytes.substr(offset,8)=="SBRSRV02"&&bytes[offset+8]==(n?'R':'P'));
    unsigned field=0;
    for(const auto& identity:{c.database_uuid,c.statement_uuid,p.reservation_uuid,c.transaction_uuid,Id(4)}){
      for(unsigned j=0;j<16;++j)Check(static_cast<std::uint8_t>(bytes[offset+9+field*16+j])==identity.bytes[j]);
      ++field;
    }
    Check(static_cast<std::uint8_t>(bytes[offset+146])==(n?2:1));
  }
  auto other=c;other.database_uuid=Id(5);other.statement_uuid=Id(6);other.database_path=(cleanup.path/"other").string();
  c.statement_uuid=Id(7);Check(publish(c).ok);Check(publish(other).ok);
  std::filesystem::rename(journal,saved);std::filesystem::create_directory(journal);
  Check(a::RecoverSblrRelationReservations(c).error);
  Check(a::CoordinateSblrReservationRelease(c,c.statement_uuid,Id(4),2,7).ok);
  std::filesystem::remove(journal);std::filesystem::rename(saved,journal);
  Check(!a::RecoverSblrRelationReservations(c).error);
  Check(!a::CoordinateSblrReservationRelease(c,c.statement_uuid,Id(4),2,7).ok);
  Check(a::CoordinateSblrReservationRelease(other,other.statement_uuid,Id(4),2,7).ok);
  Check(!a::RecoverSblrRelationReservations(other).error);
}
