// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_relation_store.hpp"
#include "secondary_index_delta_ledger.hpp"
#include "uuid.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#if defined(__linux__)
#include <sys/stat.h>
#endif
namespace api=scratchbird::engine::internal_api;
namespace idx=scratchbird::core::index;
namespace p=scratchbird::core::platform;
namespace uuid=scratchbird::core::uuid;
namespace fs=std::filesystem;
unsigned checks=0;
void Check(bool good,const char* detail) {++checks;if(!good) throw std::runtime_error(detail);}
void Write(const fs::path& path,const std::vector<p::byte>& bytes,std::size_t count) {
  std::ofstream output(path,std::ios::binary|std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(count));
  output.close();Check(!output.fail(),"fixture write failed");
}
int main() {
  const auto stamp=std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const auto root=fs::temp_directory_path()/("sb_secondary_ledger_read_"+std::to_string(stamp));
  try {
    Check(fs::create_directory(root),"unique fixture creation failed");
    api::EngineRequestContext context;
    context.database_path=(root/"ledger-host").string();
    const fs::path path=context.database_path+".sb.mga_secondary_index_delta_ledger";
    const auto load=[&] {return api::LoadMgaSecondaryIndexDeltaLedger(context);};
    Check(load().ok && !fs::exists(path),"unpublished absent ledger refused or created");
    idx::PersistentSecondaryIndexDeltaLedger ledger;
    idx::SecondaryIndexDeltaLedgerRecord record;
    const auto id=[&](p::UuidKind kind,p::u64 salt) {
      auto generated=uuid::GenerateEngineIdentityV7(kind,1700000000000ULL+salt);
      Check(generated.ok(),"fixture binary UUID generation failed");return generated.value;
    };
    record.delta.delta_id=id(p::UuidKind::object,1);
    record.delta.index_uuid=id(p::UuidKind::object,2);
    record.delta.table_uuid=id(p::UuidKind::object,3);
    record.delta.row_uuid=id(p::UuidKind::row,4);
    record.delta.version_uuid=id(p::UuidKind::row,5);
    record.delta.transaction_uuid=id(p::UuidKind::transaction,6);
    record.delta.local_transaction_id=7;
    record.delta.delta_kind=idx::SecondaryIndexDeltaKind::insert;
    record.delta.key_payload="exact-key";
    record.delta.cleanup_horizon_token="retained-by-mga";
    record.source_evidence_reference="ledger-read-admission-fixture";
    ledger.records.push_back(record);
    const auto encoded=idx::EncodePersistentSecondaryIndexDeltaLedger(ledger,{});
    Check(encoded.ok(),"fixture ledger encoding failed");
    const auto empty=idx::EncodePersistentSecondaryIndexDeltaLedger({},{});
    Check(empty.ok() && !empty.bytes.empty(),"zero-record ledger requires a format envelope");
    Write(path,empty.bytes,empty.bytes.size());
    const auto empty_loaded=load();
    Check(empty_loaded.ok && empty_loaded.ledger.records.empty(),"valid zero-record ledger refused");
    const auto restored=[&] {
      Write(path,encoded.bytes,encoded.bytes.size());
      const auto value=load();Check(value.ok && value.ledger.records.size()==1,"valid ledger did not recover");
      const auto& actual=value.ledger.records[0].delta;
      Check(actual.delta_id.value==record.delta.delta_id.value &&
            actual.index_uuid.value==record.delta.index_uuid.value &&
            actual.table_uuid.value==record.delta.table_uuid.value &&
            actual.row_uuid.value==record.delta.row_uuid.value &&
            actual.version_uuid.value==record.delta.version_uuid.value &&
            actual.transaction_uuid.value==record.delta.transaction_uuid.value &&
            actual.local_transaction_id==7 && actual.key_payload=="exact-key",
            "complete ledger identity or value changed");
    };
    const auto refused=[&] {
      const auto value=load();
      Check(!value.ok && value.diagnostic.error && value.ledger.records.empty(),
            "unreadable or corrupt existing ledger returned empty successful authority");
    };
    restored();
    for(std::size_t size=0;size<encoded.bytes.size();++size) {
      Write(path,encoded.bytes,size);refused();Check(fs::file_size(path)==size,"refusal changed ledger length");
      std::ifstream truncated(path,std::ios::binary);
      const std::vector<p::byte> retained{std::istreambuf_iterator<char>(truncated),{}};
      Check(retained==std::vector<p::byte>(encoded.bytes.begin(),encoded.bytes.begin()+size),
            "refusal modified retained ledger prefix bytes");
    }
    restored();
    fs::resize_file(path,idx::SecondaryIndexDeltaLedgerLimits{}.max_encoded_bytes+1);
    refused();Check(fs::file_size(path)==idx::SecondaryIndexDeltaLedgerLimits{}.max_encoded_bytes+1,
                    "oversized ledger refusal changed the file");
    restored();
    fs::permissions(path,fs::perms::none);refused();fs::permissions(path,fs::perms::owner_all);
    restored();fs::remove(path);fs::create_directory(path);refused();fs::remove(path);
    fs::create_symlink(root/"missing",path);refused();Check(fs::is_symlink(fs::symlink_status(path)),"dangling link changed");fs::remove(path);
    fs::create_symlink(path.filename(),path);refused();fs::remove(path);
#if defined(__linux__)
    Check(::mkfifo(path.c_str(),0600)==0,"fixture FIFO creation failed");refused();fs::remove(path);
    fs::create_symlink("/proc/self/mem",path);refused();fs::remove(path);
#endif
    restored();
    std::ifstream input(path,std::ios::binary);
    const std::vector<p::byte> before{std::istreambuf_iterator<char>(input),{}};
    Check(before==encoded.bytes,"admission changed persisted ledger bytes");
    input.close();fs::remove_all(root);
    std::cout << "secondary_index_ledger_read checks="<<checks<<" complete_read_authority=passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr<<error.what()<<" fixture="<<root<<'\n';return 1;
  }
}
