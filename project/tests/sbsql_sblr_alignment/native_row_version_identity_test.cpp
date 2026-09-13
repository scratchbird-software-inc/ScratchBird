// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Native storage and independent-process reopen; not SQL/IPC qualification.
#include "row_data_page.hpp"
#include "row_data_physical_sweep.hpp"
#include "repair_history_inspection.hpp"
#include "page_header.hpp"
#include "disk_device.hpp"
#include "physical_mga_cow_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include "../common/single_tu_allocation_fault.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace p = scratchbird::core::platform;
namespace page = scratchbird::storage::page;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace mga = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
namespace memory = scratchbird::core::memory;
namespace api = scratchbird::engine::internal_api;
namespace fs = std::filesystem;
unsigned checks = 0;
void Check(bool ok, const char* detail) { ++checks; if (!ok) throw std::runtime_error(detail); }
template<class T> void Good(const T& result) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << ':' << result.diagnostic.message_key << '\n';
    throw std::runtime_error("native storage operation failed");
  }
  ++checks;
}
p::u64 Now() { return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count(); }
p::Uuid Id(p::u64 n) {
  p::Uuid id{{1,2,3,4,5,6,0x70,0,0x80,0,0,0,0,0,0,0}};
  for (unsigned i=0;i<7;++i) id.bytes[15-i] = static_cast<p::byte>(n>>(8*i));
  return id;
}
p::TypedUuid Typed(p::UuidKind kind,p::u64 n) { return {kind,Id(n)}; }
page::RowDataCell Cell(p::u32 n) {
  page::RowDataCell cell;
  cell.column_ordinal=1;
  cell.value.type_id=scratchbird::core::datatypes::CanonicalTypeId::int32;
  cell.value.payload.resize(4);p::StoreLittle32(cell.value.payload.data(),n);
  return cell;
}
p::u64 Hash(const p::byte* bytes,std::size_t n) {
  p::u64 value=1469598103934665603ull;
  for(std::size_t i=0;i<n;++i) { value^=bytes[i];value*=1099511628211ull; }
  return value;
}
void BodyChecksum(std::vector<p::byte>& bytes) {
  p::StoreLittle64(bytes.data()+32,0);
  p::StoreLittle64(bytes.data()+32,Hash(bytes.data(),bytes.size()));
}
void RowChecksum(std::vector<p::byte>& bytes) {
  // Independent fixed-format oracle, not the production row encoder.
  const auto row_bytes=p::LoadLittle32(bytes.data()+96+56);
  p::StoreLittle64(bytes.data()+96+64,0);
  const auto hash=Hash(bytes.data()+96,row_bytes);
  p::StoreLittle64(bytes.data()+96+64,hash);
  const auto slots=p::LoadLittle32(bytes.data()+88);
  p::StoreLittle64(bytes.data()+slots+16,hash);
  BodyChecksum(bytes);
}
void Codec() {
  static_assert(sizeof(page::RowDataRecord::version_uuid)==16);
  static_assert(sizeof(page::RowDataRecord::row_version)==8);
  page::RowDataPageBody body;
  body.relation_uuid=Typed(p::UuidKind::object,1);body.segment_id=1;
  body.segment_generation=2;body.page_number=200;body.page_generation=3;
  page::RowDataRecord row;
  row.storage_generation = 1;
  row.row_uuid=Typed(p::UuidKind::row,2);row.version_uuid=Id(3);
  row.transaction_uuid=Typed(p::UuidKind::transaction,4);row.local_transaction_id=5;
  row.row_version=(p::u64{1}<<40)+7;row.stable_slot_id=8;
  row.previous_row_version=9;row.previous_version_uuid=Id(5);
  row.next_row_version=row.row_version+1;row.next_version_uuid=Id(6);
  row.cells={Cell(42)};body.rows={row};
  const auto built=page::BuildRowDataPageBody(body,8192);Good(built);
  Check(std::string(built.serialized.begin(),built.serialized.begin()+8)=="SBROW004","wrong row-page version");
  const auto& b=built.serialized;
  Check(p::LoadLittle64(b.data()+96+136)==1,"residency generation not at specified offset");
  Check(p::LoadLittle64(b.data()+96+40)==row.row_version,"64-bit sequence truncated");
  for(const auto [offset,id]:{std::pair<unsigned,p::Uuid>{0,row.row_uuid.value},
      {16,row.transaction_uuid.value},{72,row.version_uuid},
      {104,row.previous_version_uuid},{120,row.next_version_uuid}})
    Check(std::equal(id.bytes.begin(),id.bytes.end(),b.begin()+96+offset),"native identity not raw16 at specified offset");
  const auto parsed=page::ParseRowDataPageBody(b,200);Good(parsed);
  Check(parsed.body.rows.size()==1 && parsed.body.rows[0].row_version==row.row_version &&
      parsed.body.rows[0].version_uuid==row.version_uuid &&
      parsed.body.rows[0].storage_generation==row.storage_generation &&
      parsed.body.rows[0].previous_version_uuid==row.previous_version_uuid &&
      parsed.body.rows[0].next_version_uuid==row.next_version_uuid,"native version roundtrip lost identity");
  auto locator=page::MakeDenseRowOrdinalLocator(page::MakeDenseRowOrdinalScope(parsed.body),parsed.body.rows[0],true,true);
  Check(page::ValidateDenseRowOrdinalLocator(parsed.body,locator).accepted,"exact ordinal locator refused");
  for(p::u64 generation:{p::u64{0},p::u64{4}}) {
    auto malformed=parsed.body;malformed.rows.front().storage_generation=generation;
    Check(!page::ValidateDenseRowOrdinalLocator(malformed,locator).accepted,
          "ordinal acceleration accepted invalid residency generation");
  }
  locator.version_uuid=Id(7);
  Check(!page::ValidateDenseRowOrdinalLocator(parsed.body,locator).accepted,"ordinal locator accepted another version");
  auto refuse=[&](std::vector<p::byte> bytes,bool refresh_row=true) {
    if(refresh_row)RowChecksum(bytes);else BodyChecksum(bytes);
    const auto result=page::ParseRowDataPageBody(bytes,200);
    Check(!result.ok() && result.body.rows.empty() && result.serialized.empty(),"malformed native page published partial rows");
  };
  for(p::u64 generation:{p::u64{0},p::u64{4},std::numeric_limits<p::u64>::max()}) {
    auto bytes=b;p::StoreLittle64(bytes.data()+96+136,generation);refuse(bytes);
    auto invalid=body;invalid.rows.front().storage_generation=generation;
    Check(!page::BuildRowDataPageBody(invalid,8192).ok(),"invalid residency generation writer accepted");
  }
  for(p::u64 generation:{p::u64{1},p::u64{2},p::u64{3},std::numeric_limits<p::u64>::max()}) {
    auto edge=body;edge.page_generation=generation;edge.rows.front().storage_generation=generation;
    const auto encoded=page::BuildRowDataPageBody(edge,8192);Good(encoded);
    const auto decoded=page::ParseRowDataPageBody(encoded.serialized,200);Good(decoded);
    Check(decoded.body.rows.front().storage_generation==generation,"residency generation narrowed");
  }
  auto newer_page=body;newer_page.page_generation=99;
  const auto retained=page::BuildRowDataPageBody(newer_page,8192);Good(retained);
  Check(p::LoadLittle64(retained.serialized.data()+96+136)==1,"page rewrite changed retained residency generation");
  for(unsigned offset:{0u,16u,72u,104u,120u}) {
    for(unsigned version=0;version<16;++version) if(version!=7) {
      auto bytes=b;bytes[96+offset+6]=static_cast<p::byte>(version<<4);refuse(bytes);
    }
    auto bytes=b;std::fill_n(bytes.begin()+96+offset,16,0);refuse(bytes);
    for(unsigned variant:{0u,1u,3u}) { bytes=b;bytes[96+offset+8]=static_cast<p::byte>(variant<<6);refuse(bytes); }
  }
  for(unsigned offset:{32u,40u}) {auto bytes=b;p::StoreLittle64(bytes.data()+96+offset,0);refuse(bytes);}
  for(unsigned offset:{52u,60u}) {auto bytes=b;p::StoreLittle32(bytes.data()+96+offset,0);refuse(bytes);}
  for(unsigned bit=1;bit<16;++bit) {auto bytes=b;p::StoreLittle16(bytes.data()+96+48,1u<<bit);refuse(bytes);}
  for(unsigned bit=1;bit<32;++bit) {auto bytes=b;p::StoreLittle32(bytes.data()+p::LoadLittle32(b.data()+88)+12,1u<<bit);refuse(bytes);}
  for(unsigned version:{'1','2','3','5'}) {auto bytes=b;bytes[7]=version;refuse(bytes,false);}
  for(unsigned offset:{16u,88u,96u+56u,96u+144u+4u}) {
    for(p::u32 value:{0u,1u,0xffffffffu}) {auto bytes=b;p::StoreLittle32(bytes.data()+offset,value);refuse(bytes,false);}
  }
  {auto bytes=b;bytes[20]=1;refuse(bytes);}
  {auto bytes=b;bytes[96+144+2]=1;refuse(bytes);}
  for(unsigned mode=0;mode<4;++mode) {
    auto bad=body;
    if(mode==0)bad.rows[0].version_uuid={};
    if(mode==1)bad.rows[0].version_uuid=bad.rows[0].row_uuid.value;
    if(mode==2)bad.rows[0].previous_version_uuid={};
    if(mode==3)bad.rows[0].previous_row_version=0;
    Check(!page::BuildRowDataPageBody(bad,8192).ok(),"malformed version writer accepted");
  }
  auto duplicate=body;duplicate.rows.push_back(row);
  Check(!page::BuildRowDataPageBody(duplicate,8192).ok(),"duplicate version accepted");
  duplicate.rows.back().version_uuid=Id(100);
  Check(!page::BuildRowDataPageBody(duplicate,8192).ok(),"duplicate row/sequence accepted");
  duplicate.rows.back().row_uuid=Typed(p::UuidKind::row,101);
  Good(page::BuildRowDataPageBody(duplicate,8192));
  auto linked=body;linked.rows[0].next_row_version=0;linked.rows[0].next_version_uuid={};
  auto successor=linked.rows.front();successor.version_uuid=Id(100);
  successor.previous_row_version=successor.row_version;successor.previous_version_uuid=row.version_uuid;
  ++successor.row_version;linked.rows.push_back(successor);
  Good(page::BuildRowDataPageBody(linked,8192));
  linked.rows.back().previous_version_uuid=Id(101);
  Check(!page::BuildRowDataPageBody(linked,8192).ok(),"previous sequence redirected to a different UUID");
  linked.rows.back().previous_version_uuid=row.version_uuid;
  --linked.rows.back().previous_row_version;
  Check(!page::BuildRowDataPageBody(linked,8192).ok(),"previous UUID redirected to a different sequence");
  std::cout<<"PASS native row-page identity/extent checks="<<checks<<'\n';
}
constexpr p::u64 RowPage=20000;
db::PhysicalMgaCowReadRequest ReadRequest(const fs::path& root) {
  db::PhysicalMgaCowReadRequest read;read.database_path=(root/"native.sbdb").string();
  read.relation_uuid=Typed(p::UuidKind::object,20);read.page_number=RowPage;
  return read;
}
db::PhysicalMgaCowMutationRequest Mutation(const fs::path& root,db::PhysicalMgaCowMutationKind kind,unsigned n) {
  db::PhysicalMgaCowMutationRequest request;
  request.database_path=(root/"native.sbdb").string();request.kind=kind;
  request.relation_uuid=Typed(p::UuidKind::object,20);request.row_uuid=Typed(p::UuidKind::row,21);
  request.transaction_uuid=Typed(p::UuidKind::transaction,100+n);
  request.page_number=RowPage;request.begin_unix_epoch_millis=Now();request.stable_slot_id=1;
  if(kind!=db::PhysicalMgaCowMutationKind::delete_row)request.cells={Cell(n)};
  return request;
}
void Finish(const fs::path& root,const db::PhysicalMgaCowMutationResult& mutation,bool commit) {
  db::PhysicalMgaCowFinalizeRequest finish;
  finish.database_path=(root/"native.sbdb").string();finish.transaction=mutation.transaction_entry.identity;
  finish.decision=commit?db::PhysicalMgaCowFinalizeDecision::commit:db::PhysicalMgaCowFinalizeDecision::rollback;
  finish.final_unix_epoch_millis=Now();Good(db::FinalizePhysicalMgaCowTransaction(finish));
}
void CheckInventoryState(const fs::path& root,p::u64 transaction_number,
                         mga::TransactionState expected) {
  const auto inventory=db::LoadLocalTransactionInventoryFromDatabase((root/"native.sbdb").string());Good(inventory);
  const auto id=Id(transaction_number);
  const auto found=std::find_if(inventory.inventory.entries.begin(),inventory.inventory.entries.end(),
      [&](const auto& entry){return entry.identity.transaction_uuid.value==id;});
  Check(found!=inventory.inventory.entries.end(),"failed mutation lost transaction identity");
  Check(found->identity.local_id.valid() && found->state==expected,
        "durable transaction state disagrees with mutation ownership");
}
std::string Quote(const std::string& text);
void CheckOwnedFailure(const fs::path& root,const db::PhysicalMgaCowMutationResult& result,
                       p::u64 transaction_number,const std::string& self) {
  Check(!result.ok() && result.row_page.rows.empty() && result.row_version.version_uuid.is_nil() &&
        result.page_uuid.value.is_nil() && result.page_generation==0,
        "failed mutation returned partial row or publication identity");
  Check(std::count(result.evidence.begin(),result.evidence.end(),
        "physical_mga_cow.failed_owned_transaction_rolled_back=true")==1,
        "failed helper-owned mutation omitted rollback evidence");
  CheckInventoryState(root,transaction_number,mga::TransactionState::rolled_back);
  Check(std::system((Quote(self)+" --rolled-back "+Quote(root.string())+" "+
        std::to_string(transaction_number)).c_str())==0,"independent process did not observe rollback");
}
void SaveOracle(const fs::path& root,const page::RowDataRecord& row,p::u32 expected_value) {
  std::ofstream out(root/"expected.bin",std::ios::binary|std::ios::trunc);
  for(const auto& id:{row.row_uuid.value,row.version_uuid,row.previous_version_uuid})
    out.write(reinterpret_cast<const char*>(id.bytes.data()),16);
  p::byte bytes[4];p::StoreLittle32(bytes,expected_value);
  out.write(reinterpret_cast<const char*>(bytes),sizeof(bytes));
  p::byte generation[8];p::StoreLittle64(generation,row.storage_generation);
  out.write(reinterpret_cast<const char*>(generation),sizeof(generation));
  out.close();Check(out.good(),"binary oracle write failed");
}
void Reopen(const fs::path& root) {
  const auto result=db::ReadPhysicalMgaCowRows(ReadRequest(root));Good(result);
  Check(result.version_metadata.size()==result.row_page.rows.size(),"native owner omitted version metadata");
  for(std::size_t i=0;i<result.version_metadata.size();++i)
    Check(result.version_metadata[i].identity.version_uuid==result.row_page.rows[i].version_uuid,
          "native metadata substituted a version identity");
  Check(result.visible_rows.size()==1,"independent reopen did not return one row");
  std::ifstream in(root/"expected.bin",std::ios::binary);
  for(const auto& id:{result.visible_rows[0].row_uuid.value,result.visible_rows[0].version_uuid,
                     result.visible_rows[0].previous_version_uuid}) {
    p::Uuid expected;in.read(reinterpret_cast<char*>(expected.bytes.data()),16);
    Check(in.good() && expected==id,"independent reopen changed binary identity");
  }
  p::byte expected_value[4];in.read(reinterpret_cast<char*>(expected_value),sizeof(expected_value));
  p::byte generation[8];in.read(reinterpret_cast<char*>(generation),sizeof(generation));
  Check(in.good() && p::LoadLittle64(generation)==result.visible_rows[0].storage_generation,
        "independent reopen changed version residency generation");
  const auto& cells=result.visible_rows[0].cells;
  Check(in.good() && cells.size()==1 && cells[0].column_ordinal==1 &&
        cells[0].value.type_id==scratchbird::core::datatypes::CanonicalTypeId::int32 &&
        cells[0].value.payload.size()==4 &&
        std::equal(std::begin(expected_value),std::end(expected_value),cells[0].value.payload.begin()),
        "independent reopen did not preserve requested typed payload");
  std::set<p::Uuid> versions;
  for(const auto& row:result.row_page.rows)
    Check(uuid::IsEngineIdentityUuid(row.version_uuid) && row.version_uuid!=row.row_uuid.value &&
          versions.insert(row.version_uuid).second,"native writer did not issue distinct version identities");
}
std::string Quote(const std::string& text) {
  std::string out="'";for(char c:text)out+=c=='\''?"'\\''":std::string(1,c);return out+"'";
}
void Storage(const fs::path& root,const std::string& self) {
  db::DatabaseCreateConfig create;
  create.path=(root/"native.sbdb").string();create.database_uuid=Typed(p::UuidKind::database,10);
  create.filespace_uuid=Typed(p::UuidKind::filespace,11);create.page_size=8192;
  create.creation_unix_epoch_millis=Now();create.require_resource_seed_pack=true;
  create.resource_seed_pack_root=SB_BOOTSTRAP_SEED_PACK_ROOT;
  Good(db::CreateDatabaseFile(create));
  const auto insert=db::WritePhysicalMgaCowUnpublishedMutation(Mutation(root,db::PhysicalMgaCowMutationKind::insert,1));Good(insert);
  Check(insert.page_generation==1 && insert.row_version.storage_generation==1,
        "single insert did not issue actual residency generation");
  auto read=db::ReadPhysicalMgaCowRows(ReadRequest(root));Good(read);
  Check(read.visible_rows.empty(),"uncommitted insert became visible");Finish(root,insert,true);
  SaveOracle(root,insert.row_version,1);
  const auto independent=[&] { Check(std::system((Quote(self)+" --reopen "+Quote(root.string())).c_str())==0,"independent reopen failed"); };
  independent();
  const auto update=db::WritePhysicalMgaCowUnpublishedMutation(Mutation(root,db::PhysicalMgaCowMutationKind::update,2));Good(update);
  Check(update.page_generation==2 && update.row_version.storage_generation==2 &&
        update.row_page.rows.front().storage_generation==1,
        "single update replaced earlier residency or guessed its own generation");
  Check(update.row_version.previous_version_uuid==insert.row_version.version_uuid &&
        update.row_version.version_uuid!=insert.row_version.version_uuid,"update lost predecessor identity");
  independent();Finish(root,update,true);SaveOracle(root,update.row_version,2);independent();
  const auto undone=db::WritePhysicalMgaCowUnpublishedMutation(Mutation(root,db::PhysicalMgaCowMutationKind::update,3));Good(undone);
  Check(undone.row_version.storage_generation==3,"rolled-back version lacked actual residency");
  Finish(root,undone,false);independent();
  const auto deleted=db::WritePhysicalMgaCowUnpublishedMutation(Mutation(root,db::PhysicalMgaCowMutationKind::delete_row,4));Good(deleted);
  Check(deleted.row_version.storage_generation==4 && deleted.row_page.rows.front().storage_generation==1,
        "delete changed retained residency generation");
  Check(deleted.row_version.previous_version_uuid==update.row_version.version_uuid &&
        deleted.row_version.cells.empty(),"delete did not retain exact predecessor");
  independent();Finish(root,deleted,true);
  read=db::ReadPhysicalMgaCowRows(ReadRequest(root));Good(read);
  Check(read.visible_rows.empty() && read.visible_delete_marker_count==1,"committed delete did not suppress row");
  // A checksum-valid creator mismatch on an older, hidden version must not
  // disappear behind the visible delete marker or become write authority.
  const auto offset=RowPage*8192+disk::kPageHeaderSerializedBytes;
  std::vector<p::byte> original(8192-disk::kPageHeaderSerializedBytes);
  {
    std::ifstream file(create.path,std::ios::binary);file.seekg(offset);
    file.read(reinterpret_cast<char*>(original.data()),original.size());
    Check(file.good(),"native corruption fixture read failed");
  }
  auto changed=original;const auto impostor=Id(999);
  std::copy(impostor.bytes.begin(),impostor.bytes.end(),changed.begin()+96+16);
  RowChecksum(changed);
  const auto write_body=[&](const std::vector<p::byte>& bytes) {
    std::fstream file(create.path,std::ios::binary|std::ios::in|std::ios::out);
    file.seekp(offset);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.close();
    Check(file.good(),"native corruption fixture write failed");
  };
  write_body(changed);
  const auto corrupt_read=db::ReadPhysicalMgaCowRows(ReadRequest(root));
  Check(!corrupt_read.ok() && corrupt_read.visible_rows.empty() &&
        corrupt_read.diagnostic.message_key=="storage.physical_mga_cow.creator_identity_mismatch",
        "hidden creator mismatch became ordinary absence");
  const auto corrupt_write=db::WritePhysicalMgaCowUnpublishedMutation(Mutation(root,db::PhysicalMgaCowMutationKind::insert,8));
  Check(!corrupt_write.ok() && corrupt_write.diagnostic.message_key==
        "storage.physical_mga_cow.creator_identity_mismatch","writer accepted corrupt predecessor inventory identity");
  CheckOwnedFailure(root,corrupt_write,108,self);
  write_body(original);
  disk::SerializedPageHeader original_header;
  {
    std::ifstream file(create.path,std::ios::binary);file.seekg(RowPage*8192);
    file.read(reinterpret_cast<char*>(original_header.data()),original_header.size());
    Check(file.good(),"native outer header fixture read failed");
  }
  const auto parsed_header=disk::ParsePageHeader(original_header);Good(parsed_header);
  const auto write_header=[&](const disk::SerializedPageHeader& header) {
    std::fstream file(create.path,std::ios::binary|std::ios::in|std::ios::out);file.seekp(RowPage*8192);
    file.write(reinterpret_cast<const char*>(header.data()),header.size());file.close();
    Check(file.good(),"native outer header fixture write failed");
  };
  for(unsigned field=0;field<3;++field) {
    auto header=parsed_header.header;
    if(field==0)header.database_uuid=Id(990);
    if(field==1)header.filespace_uuid=Id(991);
    if(field==2)++header.page_generation;
    const auto encoded=disk::SerializePageHeader(header);Good(encoded);write_header(encoded.serialized);
    const auto refused=db::ReadPhysicalMgaCowRows(ReadRequest(root));
    Check(!refused.ok() && refused.visible_rows.empty() && refused.diagnostic.message_key==
          "storage.physical_mga_cow.page_identity_mismatch","cross-node/filespace or stale generation header accepted");
  }
  write_header(original_header);
  auto exhausted_body=original;
  p::StoreLittle64(exhausted_body.data()+56,std::numeric_limits<p::u64>::max());
  p::StoreLittle64(exhausted_body.data()+80,std::numeric_limits<p::u64>::max());
  BodyChecksum(exhausted_body);write_body(exhausted_body);
  auto exhausted_header=parsed_header.header;exhausted_header.page_generation=std::numeric_limits<p::u64>::max();
  const auto encoded_exhausted=disk::SerializePageHeader(exhausted_header);Good(encoded_exhausted);
  write_header(encoded_exhausted.serialized);
  const auto generation_refused=db::WritePhysicalMgaCowUnpublishedMutation(Mutation(root,db::PhysicalMgaCowMutationKind::insert,9));
  Check(!generation_refused.ok() && generation_refused.diagnostic.message_key==
        "storage.physical_mga_cow.page_generation_exhausted","page generation overflow reused an old generation");
  CheckOwnedFailure(root,generation_refused,109,self);
  write_header(original_header);write_body(original);
  auto exhausted_slot=original;p::StoreLittle32(exhausted_slot.data()+96+60,std::numeric_limits<p::u32>::max());
  p::StoreLittle32(exhausted_slot.data()+p::LoadLittle32(exhausted_slot.data()+88),std::numeric_limits<p::u32>::max());
  RowChecksum(exhausted_slot);write_body(exhausted_slot);
  auto new_record=Mutation(root,db::PhysicalMgaCowMutationKind::insert,11);
  new_record.row_uuid=Typed(p::UuidKind::row,501);new_record.stable_slot_id=0;
  const auto slot_refused=db::WritePhysicalMgaCowUnpublishedMutation(new_record);
  Check(!slot_refused.ok() && slot_refused.diagnostic.message_key==
        "storage.physical_mga_cow.slot_identity_exhausted","slot identity overflow reused an occupied identifier");
  CheckOwnedFailure(root,slot_refused,111,self);
  write_body(original);
  // Actual batch path under an inventory-owned transaction, not fabricated proof flags.
  const auto inventory=db::LoadLocalTransactionInventoryFromDatabase(create.path);Good(inventory);
  const auto batch_transaction=Typed(p::UuidKind::transaction,110);
  const auto started=mga::BeginLocalTransaction(inventory.inventory,batch_transaction,Now());Good(started);
  Good(db::PersistLocalTransactionInventoryToDatabase(create.path,started.inventory));
  db::PhysicalMgaCowMutationBatchRequest batch;
  for(unsigned i=0;i<8;++i) {
    auto request=Mutation(root,db::PhysicalMgaCowMutationKind::insert,10);
    request.row_uuid=Typed(p::UuidKind::row,200+i);request.transaction_uuid=batch_transaction;
    request.use_existing_transaction=true;request.existing_local_transaction_id=started.entry.identity.local_id;
    request.page_number=RowPage+1;request.stable_slot_id=i+1;batch.mutations.push_back(request);
  }
  Good(db::WritePhysicalMgaCowUnpublishedMutationBatch(batch));
  auto own=ReadRequest(root);own.page_number=RowPage+1;own.use_latest_committed_snapshot=false;
  own.visibility_snapshot.reader_transaction=started.entry.identity.local_id;
  own.reader_identity=started.entry.identity;
  const auto rows=db::ReadPhysicalMgaCowRows(own);Good(rows);
  Check(rows.visible_rows.size()==8,"native batch did not write eight rows");
  std::set<p::Uuid> ids;
  for(const auto& row:rows.visible_rows)Check(uuid::IsEngineIdentityUuid(row.version_uuid) && ids.insert(row.version_uuid).second,"batch reused version UUID");
  auto duplicate_batch=batch;duplicate_batch.engine_generated_unique_insert_rows=true;
  duplicate_batch.mutations={batch.mutations[0],batch.mutations[0]};
  for(auto& mutation:duplicate_batch.mutations)mutation.page_number=RowPage+2;
  const auto duplicate_result=db::WritePhysicalMgaCowUnpublishedMutationBatch(duplicate_batch);
  Check(!duplicate_result.ok() && duplicate_result.written_rows==0,
        "batch uniqueness declaration bypassed native version identity validation");
  CheckInventoryState(root,110,mga::TransactionState::active);
  const auto caller_failure=db::WritePhysicalMgaCowUnpublishedMutation(batch.mutations[0]);
  Check(!caller_failure.ok() && caller_failure.diagnostic.message_key==
        "storage.physical_mga_cow.duplicate_visible_row","duplicate caller-owned row was accepted");
  Check(caller_failure.evidence.empty(),"helper claimed rollback of caller-owned transaction");
  CheckInventoryState(root,110,mga::TransactionState::active);
  Check(std::system((Quote(self)+" --active "+Quote(root.string())+" 110").c_str())==0,
        "independent process did not retain caller-owned active transaction");
  const auto after_failure=db::ReadPhysicalMgaCowRows(own);Good(after_failure);
  Check(after_failure.visible_rows.size()==8,"caller-owned failure lost prior statement rows");
  for(std::size_t i=0;i<8;++i)
    Check(after_failure.visible_rows[i].version_uuid==rows.visible_rows[i].version_uuid &&
          after_failure.visible_rows[i].cells[0].value.payload==rows.visible_rows[i].cells[0].value.payload,
          "failed duplicate mutation changed an existing version or payload");
  {
    const auto before=db::LoadLocalTransactionInventoryFromDatabase(create.path);Good(before);
    const auto journal_bytes=[&] {
      std::ifstream in(create.path+".sb.txn_publish",std::ios::binary);
      Check(in.good(),"inventory publication oracle cannot open journal");
      return std::string(std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>());
    };
    const auto publication_before=journal_bytes();
    disk::FileDevice read_only;Good(read_only.Open(create.path,disk::FileOpenMode::open_existing_read_only));
    auto request=Mutation(root,db::PhysicalMgaCowMutationKind::insert,12);
    request.page_number=RowPage+3;request.row_uuid=Typed(p::UuidKind::row,512);
    const auto refused=db::WritePhysicalMgaCowUnpublishedMutationToOpenDevice(read_only,request);
    Check(!refused.ok() && refused.diagnostic.diagnostic_code=="STORAGE.READ_ONLY_DEVICE" &&
          refused.diagnostic.message_key=="storage.transaction_inventory.read_only_device",
          "read-only device accepted mutation or lost the device refusal diagnostic");
    Check(journal_bytes()==publication_before,"read-only mutation changed publication authority");
    Check(std::count(refused.evidence.begin(),refused.evidence.end(),
          "physical_mga_cow.failed_owned_transaction_not_published=true")==1,
          "read-only pre-publication failure invented rollback authority");
    const auto after=db::LoadLocalTransactionInventoryFromOpenDevice(&read_only,8192);Good(after);
    Check(after.inventory.entries.size()==before.inventory.entries.size() &&
          after.inventory.next_local_transaction_id==before.inventory.next_local_transaction_id,
          "read-only failure published or consumed transaction identity");
    const auto proposed=mga::BeginLocalTransaction(before.inventory,request.transaction_uuid,Now());Good(proposed);
    const auto direct=db::PersistLocalTransactionInventoryToOpenDevice(&read_only,8192,proposed.inventory);
    Check(!direct.ok() && direct.diagnostic.diagnostic_code=="STORAGE.READ_ONLY_DEVICE" &&
          direct.inventory.entries.empty() && journal_bytes()==publication_before,
          "direct inventory persistence bypassed the read-only publication fence");
  }
  // Real native owner metadata and durable inventory feed the actual MGA
  // decision and page-image sweep. This does not claim durable cleanup writes.
  const auto cleanup_source=db::ReadPhysicalMgaCowRows(ReadRequest(root));Good(cleanup_source);
  Check(cleanup_source.version_metadata.size()==cleanup_source.row_page.rows.size(),
        "cleanup owner metadata is incomplete");
  db::RepairHistoryInspectionRequest history;
  for(const auto& metadata:cleanup_source.version_metadata) {
    db::RepairOrdinaryVersionRecord record;record.metadata=metadata;
    record.version_uuid={p::UuidKind::row,metadata.identity.version_uuid};
    record.page_uuid={p::UuidKind::page,parsed_header.header.page_uuid};
    record.page_number=RowPage;history.ordinary_versions.push_back(record);
  }
  const auto inspected=db::InspectRepairHistory(history);Good(inspected);
  Check(inspected.ordinary_version_count==history.ordinary_versions.size(),"repair inspection omitted native metadata");
  for(unsigned mode=0;mode<2;++mode) {
    auto bad=history;
    if(mode==0)bad.ordinary_versions.back().version_uuid.value=Id(998);
    else bad.ordinary_versions.back().metadata.identity.version_uuid={};
    const auto refused=db::InspectRepairHistory(bad);
    Check(!refused.ok() && refused.rows.empty() && !refused.inspection_ready,
          "repair inspection accepted detached version or published partial metadata");
  }
  mga::LocalGarbageCollectionSweepRequest cleanup;
  cleanup.workset.inventory=cleanup_source.inventory;
  cleanup.workset.inventory_authoritative=true;cleanup.workset.inventory_complete=true;
  cleanup.workset.emit_reclaim_evidence_records=true;cleanup.workset.max_reclaim_evidence_records=16;
  cleanup.workset.retain_row_versions_in_result=false;
  for(const auto& metadata:cleanup_source.version_metadata) {
    if(metadata.state==mga::RowVersionState::rolled_back)cleanup.workset.row_versions.push_back(metadata);
  }
  Check(cleanup.workset.row_versions.size()==1,"actual rolled-back cleanup candidate absent");
  cleanup.family=mga::LocalCleanupSweepFamily::explicit_request;cleanup.engine_mga_authoritative=true;
  cleanup.max_candidate_row_versions=16;
  const auto decision=mga::RunLocalGarbageCollectionSweep(cleanup);Good(decision);
  Check(decision.cleanup.reclaimed_row_version_count==1 &&
        decision.cleanup.reclaim_evidence_records[0].row_version_identity.version_uuid==undone.row_version.version_uuid,
        "MGA cleanup lost owning native version identity");
  page::RowDataPhysicalSweepRequest sweep;sweep.page=cleanup_source.row_page;sweep.sweep=decision;
  sweep.page_size=8192;sweep.engine_mga_authoritative=true;sweep.max_reclaim_rows=16;
  const auto staged=page::ApplyRowDataPhysicalSweep(sweep);Good(staged);
  Check(staged.removed_row_count==1 && staged.page.rows.size()+1==sweep.page.rows.size(),
        "exact native version was not removed from staged page");
  for(const auto& retained_row:staged.page.rows) {
    const auto original_row=std::find_if(sweep.page.rows.begin(),sweep.page.rows.end(),
        [&](const auto& row){return row.version_uuid==retained_row.version_uuid;});
    Check(original_row!=sweep.page.rows.end() && original_row->storage_generation==retained_row.storage_generation,
        "compaction replaced retained residency generation");
  }
  Check(staged.staged_page_changed && staged.diagnostic.diagnostic_code.empty(),
        "page staging claimed a physical mutation or fabricated success diagnostic");
  Check(std::none_of(staged.page.rows.begin(),staged.page.rows.end(),[&](const auto& row){
        return row.version_uuid==undone.row_version.version_uuid;}),"wrong version survived staged cleanup");
  const auto refuses=[&](const page::RowDataPhysicalSweepRequest& bad) {
    const auto failed=page::ApplyRowDataPhysicalSweep(bad);
    Check(!failed.ok() && failed.page.rows.empty() && failed.serialized.empty() &&
          failed.removed_row_count==0 && !failed.staged_page_changed,
          "invalid reclaim evidence published partial cleanup");
  };
  for(unsigned mode=0;mode<6;++mode) {
    auto bad=sweep;auto& evidence=bad.sweep.cleanup.reclaim_evidence_records[0];
    if(mode==0)evidence.row_version_identity.version_uuid=Id(555);
    if(mode==1)evidence.row_version_identity.version_uuid={};
    if(mode==2)++evidence.authoritative_cleanup_horizon_local_transaction_id;
    if(mode==3)++evidence.creator_transaction.value;
    if(mode>=4) {
      auto extra=evidence;
      if(mode==5)extra.row_version_identity.version_uuid=Id(556);
      bad.sweep.cleanup.reclaim_evidence_records.push_back(extra);
      // Both records deliberately share the original display label. A label
      // set must not certify the unused/duplicate entry with a count of one.
      bad.sweep.cleanup.reclaimed_row_version_count=1;
    }
    refuses(bad);
  }
  auto exhausted=sweep;exhausted.page.compaction_generation=std::numeric_limits<p::u64>::max();refuses(exhausted);
  auto noop=sweep;noop.sweep.cleanup.reclaim_evidence_records.clear();noop.sweep.cleanup.reclaimed_row_version_count=0;
  const auto no_change=page::ApplyRowDataPhysicalSweep(noop);Good(no_change);
  const auto original_page=page::BuildRowDataPageBody(noop.page,8192);Good(original_page);
  Check(!no_change.staged_page_changed && no_change.serialized==original_page.serialized,
        "empty cleanup changed the page generation or bytes");
  auto identity=cleanup.workset.row_versions.front().identity;
  for(unsigned version=0;version<16;++version)if(version!=7) {
    auto bad=identity;bad.version_uuid.bytes[6]=static_cast<p::byte>(version<<4);
    Check(!mga::ValidateRowVersionIdentity(bad).ok(),"metadata admitted a non-v7 version identity");
  }
  identity.version_uuid=identity.row.row_uuid.value;
  Check(!mga::ValidateRowVersionIdentity(identity).ok(),"metadata substituted logical row for version identity");
  db::PhysicalMgaCowFinalizeRequest abort_batch;
  abort_batch.database_path=create.path;abort_batch.transaction=started.entry.identity;
  abort_batch.decision=db::PhysicalMgaCowFinalizeDecision::rollback;abort_batch.final_unix_epoch_millis=Now();
  Good(db::FinalizePhysicalMgaCowTransaction(abort_batch));
  auto batch_read=ReadRequest(root);batch_read.page_number=RowPage+1;
  const auto batch_cleanup_source=db::ReadPhysicalMgaCowRows(batch_read);Good(batch_cleanup_source);
  Check(batch_cleanup_source.visible_rows.empty() && batch_cleanup_source.version_metadata.size()==8,
        "batch rollback lost native cleanup metadata or leaked rows");
  cleanup.workset.inventory=batch_cleanup_source.inventory;
  cleanup.workset.row_versions=batch_cleanup_source.version_metadata;
  const auto batch_decision=mga::RunLocalGarbageCollectionSweep(cleanup);Good(batch_decision);
  Check(batch_decision.cleanup.reclaimed_row_version_count==8 &&
        batch_decision.cleanup.reclaim_evidence_records.size()==8,"batch versions were not independently reclaimable");
  std::set<std::string> display_labels;
  for(const auto& evidence:batch_decision.cleanup.reclaim_evidence_records)
    display_labels.insert(evidence.stable_evidence_id);
  Check(display_labels.size()==1,"fixture no longer exercises colliding cleanup display labels");
  sweep.page=batch_cleanup_source.row_page;sweep.sweep=batch_decision;
  const auto batch_staged=page::ApplyRowDataPhysicalSweep(sweep);Good(batch_staged);
  Check(batch_staged.staged_page_changed && batch_staged.removed_row_count==8 && batch_staged.page.rows.empty(),
        "display-label collision prevented exact binary version reclamation");
  const auto still_durable=db::ReadPhysicalMgaCowRows(batch_read);Good(still_durable);
  Check(still_durable.row_page.rows.size()==8,"page staging unexpectedly mutated durable storage");

  // Feed actual native versions and their matching inventory projection into
  // the engine's production snapshot cleanup, not string or mock identities.
  api::MgaRelationPhysicalSweepRequest relation_cleanup;
  relation_cleanup.relation_uuid=batch_cleanup_source.row_page.relation_uuid.value;
  relation_cleanup.engine_mga_authoritative=true;relation_cleanup.cleanup_horizon_authoritative=true;
  relation_cleanup.authoritative_cleanup_horizon_local_transaction_id=
      batch_decision.cleanup.authoritative_cleanup_horizon_local_transaction_id;
  relation_cleanup.max_row_versions_to_scan=16;relation_cleanup.max_index_entries_to_scan=32;
  relation_cleanup.reclaim_evidence_records=batch_decision.cleanup.reclaim_evidence_records;
  for(const auto& native:batch_cleanup_source.row_page.rows) {
    api::CrudRowVersionRecord row;
    row.table_uuid=relation_cleanup.relation_uuid;row.row_uuid=native.row_uuid.value;
    row.version_uuid=native.version_uuid;row.creator_tx=native.local_transaction_id;
    row.creator_transaction_uuid=native.transaction_uuid.value;row.sequence=native.row_version;
    row.previous_version_uuid=native.previous_version_uuid;row.previous_sequence=native.previous_row_version;
    row.deleted=native.deleted;relation_cleanup.state.row_versions.push_back(row);
    api::CrudIndexEntryRecord index;
    index.index_uuid=Id(701);index.table_uuid=row.table_uuid;index.row_uuid=row.row_uuid;
    index.version_uuid=row.version_uuid;index.key_value="fixture index projection";
    relation_cleanup.state.index_entries.push_back(index);
  }
  auto projected=api::ApplyMgaRelationPhysicalSweepToState(relation_cleanup);
  Check(projected.ok && !projected.diagnostic.error && projected.staged_state_changed &&
        projected.removed_row_version_count==8 && projected.removed_index_entry_count==8 &&
        projected.state.row_versions.empty() && projected.state.index_entries.empty() && projected.evidence.empty(),
        "binary engine projection did not remove all exact versions and index targets");
  const auto reject_projection=[&](const api::MgaRelationPhysicalSweepRequest& request) {
    const auto result=api::ApplyMgaRelationPhysicalSweepToState(request);
    Check(!result.ok && result.fail_closed && !result.staged_state_changed && result.diagnostic.error &&
          result.diagnostic.code=="CATALOG.INVALID_INPUT" && result.state.row_versions.empty() &&
          result.state.index_entries.empty() && result.evidence.empty() &&
          result.scanned_row_version_count==0 && result.removed_row_version_count==0 &&
          result.retained_row_version_count==0 && result.scanned_index_entry_count==0 &&
          result.removed_index_entry_count==0 && result.retained_index_entry_count==0,
          "invalid binary projection produced partial cleanup or fake authority");
  };
  for(unsigned mode=0;mode<14;++mode) {
    auto bad=relation_cleanup;
    auto& row=bad.state.row_versions.back();auto& entry=bad.state.index_entries.back();
    auto& evidence=bad.reclaim_evidence_records.back();
    switch(mode) {
      case 0: bad.relation_uuid=Id(800);break;
      case 1: row.version_uuid={};break;
      case 2: row.creator_transaction_uuid=Id(801);break;
      case 3: ++row.creator_tx;break;
      case 4: ++row.sequence;break;
      case 5: row.table_uuid=Id(802);break;
      case 6: row.row_uuid=Id(803);break;
      case 7: ++evidence.authoritative_cleanup_horizon_local_transaction_id;break;
      case 8: bad.reclaim_evidence_records.push_back(evidence);break;
      case 9: evidence.row_version_identity.version_uuid=Id(804);break;
      case 10: entry.table_uuid=Id(805);break;
      case 11: entry.row_uuid=Id(806);break;
      case 12: entry.version_uuid=Id(807);break;
      case 13: bad.state.row_versions.push_back(row);break;
    }
    reject_projection(bad);
  }
  for(auto member:{&api::CrudRowVersionRecord::table_uuid,&api::CrudRowVersionRecord::row_uuid,
                   &api::CrudRowVersionRecord::version_uuid,&api::CrudRowVersionRecord::creator_transaction_uuid}) {
    for(unsigned version=0;version<16;++version)if(version!=7) {
      auto bad=relation_cleanup;(bad.state.row_versions.back().*member).bytes[6]=static_cast<p::byte>(version<<4);
      reject_projection(bad);
    }
    auto bad=relation_cleanup;(bad.state.row_versions.back().*member).bytes[8]=0;reject_projection(bad);
  }
  for(auto member:{&api::CrudIndexEntryRecord::index_uuid,&api::CrudIndexEntryRecord::table_uuid,
                   &api::CrudIndexEntryRecord::row_uuid,&api::CrudIndexEntryRecord::version_uuid}) {
    auto bad=relation_cleanup;(bad.state.index_entries.back().*member)={};reject_projection(bad);
    bad=relation_cleanup;(bad.state.index_entries.back().*member).bytes[6]=0x40;reject_projection(bad);
  }
  // Distinct live projections are retained; a same-label decision cannot
  // certify their reclamation. Empty evidence is an unchanged projection.
  auto partial=relation_cleanup;partial.reclaim_evidence_records.pop_back();
  const auto retained=api::ApplyMgaRelationPhysicalSweepToState(partial);
  Check(retained.ok && retained.removed_row_version_count==7 && retained.removed_index_entry_count==7 &&
        retained.state.row_versions.size()==1 && retained.state.index_entries.size()==1 &&
        retained.state.row_versions[0].version_uuid==relation_cleanup.state.row_versions.back().version_uuid,
        "projection lost the unmatched version or its index entry");
  auto empty=relation_cleanup;empty.reclaim_evidence_records.clear();
  const auto unchanged=api::ApplyMgaRelationPhysicalSweepToState(empty);
  Check(unchanged.ok && !unchanged.staged_state_changed && unchanged.state.row_versions.size()==8 &&
        unchanged.state.index_entries.size()==8,"empty cleanup changed the native projection");
  allocation_attempts=0;allocations_before_failure=std::numeric_limits<std::ptrdiff_t>::max();
  const auto measured=api::ApplyMgaRelationPhysicalSweepToState(relation_cleanup);
  allocations_before_failure=-1;const auto allocation_count=allocation_attempts;
  Check(measured.ok && allocation_count>0,"projection allocation coverage did not execute");
  for(std::size_t failure=0;failure<allocation_count;++failure) {
    bool threw=false;allocations_before_failure=static_cast<std::ptrdiff_t>(failure);
    try { (void)api::ApplyMgaRelationPhysicalSweepToState(relation_cleanup); }
    catch(const std::bad_alloc&) { threw=true; }
    allocations_before_failure=-1;
    Check(threw,"projection allocation failure returned fabricated success");
    Check(relation_cleanup.state.row_versions.size()==8 && relation_cleanup.state.index_entries.size()==8 &&
          relation_cleanup.state.row_versions.back().version_uuid==batch_cleanup_source.row_page.rows.back().version_uuid,
          "projection allocation failure changed its native input snapshot");
  }
  std::cout<<"relation_cleanup allocation_points="<<allocation_count<<'\n';
  const auto after_projection=db::ReadPhysicalMgaCowRows(batch_read);Good(after_projection);
  Check(after_projection.row_page.rows.size()==8,"engine snapshot staging changed durable native rows");
  std::cout<<"PASS native insert/update/rollback/delete/batch and independent-process reopen checks="<<checks<<'\n';
}
int main(int argc,char** argv) {
  fs::path root;
  try {
    auto policy=memory::DefaultLocalEngineMemoryPolicy();policy.policy_name="native_row_version_identity";
    Good(memory::ConfigureDefaultMemoryManagerForFixture(policy,"native_row_version_identity"));
    if(argc==3 && std::string_view(argv[1])=="--reopen") { Reopen(argv[2]);return 0; }
    if(argc==4 && (std::string_view(argv[1])=="--rolled-back" || std::string_view(argv[1])=="--active")) {
      CheckInventoryState(argv[2],std::stoull(argv[3]),std::string_view(argv[1])=="--active"?
          mga::TransactionState::active:mga::TransactionState::rolled_back);return 0;
    }
    Codec();
    if(argc==2 && std::string_view(argv[1])=="--codec")return 0;
    const auto unique=uuid::IssueRuntimeIdentityV7();Check(unique.has_value(),"fixture identity failed");
    root=fs::temp_directory_path()/("sb-native-row-"+uuid::UuidToString(*unique));
    Check(fs::create_directory(root),"isolated fixture directory failed");
    Storage(root,fs::absolute(argv[0]).string());fs::remove_all(root);return 0;
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<'\n';if(!root.empty())fs::remove_all(root);return 1;
  }
}
