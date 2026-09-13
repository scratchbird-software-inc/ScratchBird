// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Native storage and independent-process reopen; not SQL/IPC qualification.
#include "row_data_page.hpp"
#include "page_header.hpp"
#include "physical_mga_cow_store.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "memory.hpp"
#include "uuid.hpp"
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
  row.row_uuid=Typed(p::UuidKind::row,2);row.version_uuid=Id(3);
  row.transaction_uuid=Typed(p::UuidKind::transaction,4);row.local_transaction_id=5;
  row.row_version=(p::u64{1}<<40)+7;row.stable_slot_id=8;
  row.previous_row_version=9;row.previous_version_uuid=Id(5);
  row.next_row_version=row.row_version+1;row.next_version_uuid=Id(6);
  row.cells={Cell(42)};body.rows={row};
  const auto built=page::BuildRowDataPageBody(body,8192);Good(built);
  Check(std::string(built.serialized.begin(),built.serialized.begin()+8)=="SBROW003","wrong row-page version");
  const auto& b=built.serialized;
  Check(p::LoadLittle64(b.data()+96+40)==row.row_version,"64-bit sequence truncated");
  for(const auto [offset,id]:{std::pair<unsigned,p::Uuid>{0,row.row_uuid.value},
      {16,row.transaction_uuid.value},{72,row.version_uuid},
      {104,row.previous_version_uuid},{120,row.next_version_uuid}})
    Check(std::equal(id.bytes.begin(),id.bytes.end(),b.begin()+96+offset),"native identity not raw16 at specified offset");
  const auto parsed=page::ParseRowDataPageBody(b,200);Good(parsed);
  Check(parsed.body.rows.size()==1 && parsed.body.rows[0].row_version==row.row_version &&
      parsed.body.rows[0].version_uuid==row.version_uuid &&
      parsed.body.rows[0].previous_version_uuid==row.previous_version_uuid &&
      parsed.body.rows[0].next_version_uuid==row.next_version_uuid,"native version roundtrip lost identity");
  auto locator=page::MakeDenseRowOrdinalLocator(page::MakeDenseRowOrdinalScope(parsed.body),parsed.body.rows[0],true,true);
  Check(page::ValidateDenseRowOrdinalLocator(parsed.body,locator).accepted,"exact ordinal locator refused");
  locator.version_uuid=Id(7);
  Check(!page::ValidateDenseRowOrdinalLocator(parsed.body,locator).accepted,"ordinal locator accepted another version");
  auto refuse=[&](std::vector<p::byte> bytes,bool refresh_row=true) {
    if(refresh_row)RowChecksum(bytes);else BodyChecksum(bytes);
    const auto result=page::ParseRowDataPageBody(bytes,200);
    Check(!result.ok() && result.body.rows.empty() && result.serialized.empty(),"malformed native page published partial rows");
  };
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
  for(unsigned version:{'1','2','4'}) {auto bytes=b;bytes[7]=version;refuse(bytes,false);}
  for(unsigned offset:{16u,88u,96u+56u,96u+136u+4u}) {
    for(p::u32 value:{0u,1u,0xffffffffu}) {auto bytes=b;p::StoreLittle32(bytes.data()+offset,value);refuse(bytes,false);}
  }
  {auto bytes=b;bytes[20]=1;refuse(bytes);}
  {auto bytes=b;bytes[96+136+2]=1;refuse(bytes);}
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
  finish.database_path=(root/"native.sbdb").string();finish.local_transaction_id=mutation.transaction_entry.identity.local_id;
  finish.decision=commit?db::PhysicalMgaCowFinalizeDecision::commit:db::PhysicalMgaCowFinalizeDecision::rollback;
  finish.final_unix_epoch_millis=Now();Good(db::FinalizePhysicalMgaCowTransaction(finish));
}
void SaveOracle(const fs::path& root,const page::RowDataRecord& row,p::u32 expected_value) {
  std::ofstream out(root/"expected.bin",std::ios::binary|std::ios::trunc);
  for(const auto& id:{row.row_uuid.value,row.version_uuid,row.previous_version_uuid})
    out.write(reinterpret_cast<const char*>(id.bytes.data()),16);
  p::byte bytes[4];p::StoreLittle32(bytes,expected_value);
  out.write(reinterpret_cast<const char*>(bytes),sizeof(bytes));
  out.close();Check(out.good(),"binary oracle write failed");
}
void Reopen(const fs::path& root) {
  const auto result=db::ReadPhysicalMgaCowRows(ReadRequest(root));Good(result);
  Check(result.visible_rows.size()==1,"independent reopen did not return one row");
  std::ifstream in(root/"expected.bin",std::ios::binary);
  for(const auto& id:{result.visible_rows[0].row_uuid.value,result.visible_rows[0].version_uuid,
                     result.visible_rows[0].previous_version_uuid}) {
    p::Uuid expected;in.read(reinterpret_cast<char*>(expected.bytes.data()),16);
    Check(in.good() && expected==id,"independent reopen changed binary identity");
  }
  p::byte expected_value[4];in.read(reinterpret_cast<char*>(expected_value),sizeof(expected_value));
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
  auto read=db::ReadPhysicalMgaCowRows(ReadRequest(root));Good(read);
  Check(read.visible_rows.empty(),"uncommitted insert became visible");Finish(root,insert,true);
  SaveOracle(root,insert.row_version,1);
  const auto independent=[&] { Check(std::system((Quote(self)+" --reopen "+Quote(root.string())).c_str())==0,"independent reopen failed"); };
  independent();
  const auto update=db::WritePhysicalMgaCowUnpublishedMutation(Mutation(root,db::PhysicalMgaCowMutationKind::update,2));Good(update);
  Check(update.row_version.previous_version_uuid==insert.row_version.version_uuid &&
        update.row_version.version_uuid!=insert.row_version.version_uuid,"update lost predecessor identity");
  independent();Finish(root,update,true);SaveOracle(root,update.row_version,2);independent();
  const auto undone=db::WritePhysicalMgaCowUnpublishedMutation(Mutation(root,db::PhysicalMgaCowMutationKind::update,3));Good(undone);
  Finish(root,undone,false);independent();
  const auto deleted=db::WritePhysicalMgaCowUnpublishedMutation(Mutation(root,db::PhysicalMgaCowMutationKind::delete_row,4));Good(deleted);
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
  write_header(original_header);write_body(original);
  auto exhausted_slot=original;p::StoreLittle32(exhausted_slot.data()+96+60,std::numeric_limits<p::u32>::max());
  p::StoreLittle32(exhausted_slot.data()+p::LoadLittle32(exhausted_slot.data()+88),std::numeric_limits<p::u32>::max());
  RowChecksum(exhausted_slot);write_body(exhausted_slot);
  auto new_record=Mutation(root,db::PhysicalMgaCowMutationKind::insert,11);
  new_record.row_uuid=Typed(p::UuidKind::row,501);new_record.stable_slot_id=0;
  const auto slot_refused=db::WritePhysicalMgaCowUnpublishedMutation(new_record);
  Check(!slot_refused.ok() && slot_refused.diagnostic.message_key==
        "storage.physical_mga_cow.slot_identity_exhausted","slot identity overflow reused an occupied identifier");
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
  std::cout<<"PASS native insert/update/rollback/delete/batch and independent-process reopen checks="<<checks<<'\n';
}
int main(int argc,char** argv) {
  fs::path root;
  try {
    auto policy=memory::DefaultLocalEngineMemoryPolicy();policy.policy_name="native_row_version_identity";
    Good(memory::ConfigureDefaultMemoryManagerForFixture(policy,"native_row_version_identity"));
    if(argc==3 && std::string_view(argv[1])=="--reopen") { Reopen(argv[2]);return 0; }
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
