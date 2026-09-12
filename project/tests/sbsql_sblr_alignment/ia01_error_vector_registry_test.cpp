#include "sblr_error_vector_descriptor_registry.hpp"
#include "uuid.hpp"
#include "hash_digest.hpp"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <new>
#include <set>
#include <sstream>
#include <thread>
#if defined(SB_EV_LINUX_IO_FAULTS)
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>
namespace {
unsigned fault_kind=0, fault_at=0, fault_seen=0;
bool fault_fired=false, short_pending=false, crash_after=false;
const char* fault_directory=nullptr;
bool JournalFd(int fd,unsigned kind=0) {
  char name[64], path[4096];
  const auto count=std::snprintf(name,sizeof(name),"/proc/self/fd/%d",fd);
  if(count<=0 || static_cast<std::size_t>(count)>=sizeof(name))return false;
  const auto bytes=::readlink(name,path,sizeof(path));
  constexpr std::string_view suffix=".sb.sblr_error_vector_registry.v1";
  if(bytes<=0)return false;
  const auto actual=std::string_view(path,bytes);
  return kind==5 ? fault_directory && actual==fault_directory : actual.ends_with(suffix);
}
bool Trigger(unsigned kind,int fd) {
  if(fault_kind!=kind || !JournalFd(fd,kind))return false;
  if(++fault_seen!=fault_at)return false;
  fault_fired=true;fault_kind=0;return true;
}
}
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_pread(int fd,void* data,size_t size,off_t at) {
  if(Trigger(1,fd)){errno=EIO;return -1;}
  return __real_pread(fd,data,size,at);
}
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t size,off_t at) {
  if(short_pending && JournalFd(fd)){short_pending=false;errno=EIO;return -1;}
  if(Trigger(2,fd)) {
    if(crash_after){const auto n=__real_pwrite(fd,data,size,at);(void)n;::_exit(82);}
    errno=EIO;return -1;
  }
  if(Trigger(4,fd)) {
    short_pending=true;return __real_pwrite(fd,data,size/2,at);
  }
  return __real_pwrite(fd,data,size,at);
}
extern "C" int __wrap_fsync(int fd) {
  if(Trigger(3,fd) || Trigger(5,fd)) {
    if(crash_after){const auto n=__real_fsync(fd);(void)n;::_exit(83);}
    errno=EIO;return -1;
  }
  return __real_fsync(fd);
}
#endif

using namespace scratchbird::engine::internal_api;

namespace allocation_fault {
thread_local long remaining=-1;
thread_local bool injected=false;
}
void* operator new(std::size_t size) {
  if(allocation_fault::remaining==0) {
    allocation_fault::remaining=-1;allocation_fault::injected=true;throw std::bad_alloc();
  }
  if(allocation_fault::remaining>0)--allocation_fault::remaining;
  if(void* p=std::malloc(size?size:1))return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

static EngineUuid Identity(scratchbird::core::platform::UuidKind kind) {
  const std::uint64_t tick = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  auto value = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, tick);
  assert(value.ok());
  return value.value.value;
}

int main() {
  unsigned checks = 0, failures = 0;
  const auto check = [&](bool value, const char* requirement) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL " << requirement << '\n'; }
  };
  const auto base = std::filesystem::temp_directory_path() /
                    ("sb_error_vector_registry_" + std::to_string(
                        std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto journal = base.string() + ".sb.sblr_error_vector_registry.v1";
#if defined(SB_EV_LINUX_IO_FAULTS)
  const auto directory=base.parent_path().string();fault_directory=directory.c_str();
#endif
  { std::ofstream primary(base, std::ios::binary); primary << "isolated registry component fixture"; }
  std::error_code ec;
  std::filesystem::remove(journal, ec);

  EngineRequestContext context;
  context.database_path = base.string();
  context.database_uuid = Identity(scratchbird::core::platform::UuidKind::database);
  context.session_uuid = Identity(scratchbird::core::platform::UuidKind::object);
  context.principal_uuid = Identity(scratchbird::core::platform::UuidKind::object);
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags = {"private_error_vector_registry"};

  scratchbird::engine::sblr::SblrErrorVectorEntryV1 entry;
  entry.occurrence_ordinal = 1;
  const auto diagnostic = Identity(scratchbird::core::platform::UuidKind::object);
  std::copy(diagnostic.bytes.begin(), diagnostic.bytes.end(),
            entry.diagnostic_uuid.begin());
  entry.diagnostic_generation = 1;
  entry.precedence_ordinal = 1;
  entry.severity_code = 1;
  entry.redaction_class = 1;
  entry.safe_fields_sha256=scratchbird::engine::sblr::SblrErrorVectorEmptySafeFieldsHashV1();

  const auto receipt = Identity(scratchbird::core::platform::UuidKind::object);
  const auto registry = Identity(scratchbird::core::platform::UuidKind::object);
  const auto diagnostics = Identity(scratchbird::core::platform::UuidKind::object);
  context.statement_receipt_uuid = receipt;
  const auto receipt_id = receipt.bytes;
  const auto registry_id = registry.bytes;
  const auto diagnostics_id = diagnostics.bytes;
  auto issued = IssueSblrErrorVectorDescriptorV1(
      context, receipt_id, registry_id, 41, diagnostics_id, 7, {entry});
  assert(issued.ok && !issued.snapshot.canonical_ervd.empty());
  check(issued.snapshot.registry_generation == 41,
        "issued snapshot preserves catalog generation, not journal sequence");
  scratchbird::engine::sblr::SblrErrorVectorDescriptorV1 first;
  std::string first_detail;
  check(scratchbird::engine::sblr::DecodeSblrErrorVectorDescriptorV1(
      issued.snapshot.canonical_ervd.data(), issued.snapshot.canonical_ervd.size(),
      &first, &first_detail) && first.registry_generation == 41 &&
      first.diagnostic_registry_generation == 7,
      "canonical ERVD preserves both supplied immutable registry generations");
  assert(LookupSblrErrorVectorDescriptorV1(
      context, receipt_id, issued.snapshot.descriptor_uuid, 1).ok);

  auto foreign = context;
  foreign.database_uuid = Identity(scratchbird::core::platform::UuidKind::database);
  auto looked = LookupSblrErrorVectorDescriptorV1(
      foreign, receipt_id, issued.snapshot.descriptor_uuid, 1);
  check(!looked.ok && looked.diagnostic.code == "SECURITY.ACCESS_DENIED",
        "database ownership is checked before descriptor disclosure");
  foreign = context;
  foreign.principal_uuid = Identity(scratchbird::core::platform::UuidKind::object);
  looked = LookupSblrErrorVectorDescriptorV1(
      foreign, receipt_id, issued.snapshot.descriptor_uuid, 1);
  check(!looked.ok && looked.diagnostic.code == "SECURITY.ACCESS_DENIED",
        "principal ownership is checked before descriptor disclosure");
  foreign = context;
  foreign.session_uuid = Identity(scratchbird::core::platform::UuidKind::object);
  const auto rejected_revoke = RevokeSblrErrorVectorDescriptorsV1(
      foreign, receipt_id);
  check(rejected_revoke.error && rejected_revoke.code == "SECURITY.ACCESS_DENIED",
        "foreign session cannot revoke another receipt's descriptors");
  check(LookupSblrErrorVectorDescriptorV1(
      context, receipt_id, issued.snapshot.descriptor_uuid, 1).ok,
      "rejected foreign revocation leaves owner descriptor executable");

  const auto repeated = IssueSblrErrorVectorDescriptorV1(
      context, receipt_id, registry_id, 41, diagnostics_id, 7, {entry});
  check(repeated.ok && repeated.snapshot.registry_generation == 41,
        "repeated issue does not manufacture a new catalog generation");

  auto admin = context;
  admin.trace_tags = {"right:SBLR_ERROR_VECTOR_REGISTRY_ADMIN"};
  assert(!RecoverSblrErrorVectorDescriptorRegistryV1(admin).error);
  assert(!LookupSblrErrorVectorDescriptorV1(
      context, receipt_id, issued.snapshot.descriptor_uuid, 1).ok);

  // Exercise the actual private descriptor issuer/revoker for every registered
  // canonical severity. This is metadata codec/registry proof, not permission
  // to project private-only diagnostic identities to a parser.
  for (std::uint8_t severity = 1; severity <= 13; ++severity) {
    entry.severity_code = severity;
    auto exact = IssueSblrErrorVectorDescriptorV1(
        context, receipt_id, registry_id, 1, diagnostics_id, 1, {entry});
    assert(exact.ok);
    scratchbird::engine::sblr::SblrErrorVectorDescriptorV1 decoded;
    std::string detail;
    assert(scratchbird::engine::sblr::DecodeSblrErrorVectorDescriptorV1(
        exact.snapshot.canonical_ervd.data(), exact.snapshot.canonical_ervd.size(),
        &decoded, &detail));
    assert(decoded.entries.size() == 1 &&
           decoded.entries.front().severity_code == severity);
    assert(!RevokeSblrErrorVectorDescriptorsV1(context, receipt_id).error);
  }

  std::ifstream input(journal, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto valid = contents.str();
  assert(!valid.empty());
  const auto get = [](std::string_view bytes, std::size_t at, unsigned width) {
    std::uint64_t n=0;
    for(unsigned i=0;i<width;++i)n|=std::uint64_t(static_cast<unsigned char>(bytes[at+i]))<<(8*i);
    return n;
  };
  const auto digest = [](std::string_view frame) {
    std::string material="ScratchBird.SblrErrorVectorRegistryFrame.V1";
    material.append(frame.substr(0,248));material.append(frame.substr(280));
    const auto hash=scratchbird::core::hash::ComputeSha256Digest(
        reinterpret_cast<const scratchbird::core::platform::byte*>(material.data()),material.size());
    assert(hash.ok());return std::string(reinterpret_cast<const char*>(hash.digest.data()),32);
  };
  std::size_t offset=0, last_offset=0;std::uint64_t sequence=0;
  std::string previous(32,'\0');
  while(offset<valid.size()) {
    assert(valid.size()-offset>=344);
    const auto extent=get(valid,offset+8,8);assert(extent>=280 && extent<=524720);
    assert(extent+64<=valid.size()-offset);
    const auto frame=std::string_view(valid).substr(offset,extent);
    const auto footer=std::string_view(valid).substr(offset+extent,64);
    check(frame.substr(0,4)=="EVDE" && get(frame,4,2)==1 && get(frame,6,2)==280,
          "independent exact binary evidence header");
    check(get(frame,16,8)==++sequence && frame.substr(216,32)==previous,
          "independent contiguous sequence and previous evidence chain");
    check(frame.substr(248,32)==digest(frame),"independent evidence SHA256 preimage");
    check(footer.substr(0,4)=="EVDC" && get(footer,4,2)==1 && get(footer,6,2)==64 &&
          get(footer,8,8)==sequence && footer.substr(16,16)==frame.substr(96,16) &&
          footer.substr(32,32)==frame.substr(248,32),"independent exact commit footer");
    check(get(frame,128,8)==1,"descriptor generation remains immutable after revocation");
    if(sequence<=4)check(get(frame,136,8)==41 && get(frame,144,8)==7,
                        "issue and recovery preserve catalog and diagnostic generations");
    previous=frame.substr(248,32);last_offset=offset;offset+=extent+64;
  }
  check(sequence==30,"two issues and recovery plus thirteen issue/revoke pairs");
  // Formatting is only a negative test oracle; execution uses binary UUIDs.
  check(valid.find(scratchbird::core::uuid::UuidToString(context.database_uuid)) == std::string::npos &&
        valid.find(scratchbird::core::uuid::UuidToString(context.session_uuid)) == std::string::npos &&
        valid.find(scratchbird::core::uuid::UuidToString(receipt)) == std::string::npos &&
        valid.find(scratchbird::core::uuid::UuidToString(registry)) == std::string::npos &&
        valid.find(scratchbird::core::uuid::UuidToString(diagnostics)) == std::string::npos,
        "durable system identity is binary16, never text UUID authority");

  {
    std::ofstream output(journal, std::ios::binary | std::ios::app);
    output << "E\ttruncated\n";
  }
  assert(RecoverSblrErrorVectorDescriptorRegistryV1(admin).code ==
         "SBLR.ERROR_VECTOR.STALE");

  {
    auto corrupt = valid;
    assert(corrupt.size() > 280);
    corrupt[248] ^= 1;
    std::ofstream output(journal, std::ios::binary | std::ios::trunc);
    output << corrupt;
  }
  assert(RecoverSblrErrorVectorDescriptorRegistryV1(admin).code ==
         "SBLR.ERROR_VECTOR.STALE");

  const auto replace = [&](const std::string& bytes) {
    std::ofstream output(journal,std::ios::binary|std::ios::trunc);
    output.write(bytes.data(),bytes.size());output.close();assert(output);
  };
  const auto refused_unchanged = [&](const std::string& bytes) {
    replace(bytes);
    const auto refusal=RecoverSblrErrorVectorDescriptorRegistryV1(admin);
    check(refusal.error,"corrupt evidence cannot be accepted by recovery");
    std::ifstream input_bytes(journal,std::ios::binary);
    const std::string after((std::istreambuf_iterator<char>(input_bytes)),{});
    check(after==bytes,"failed recovery preserves every existing evidence byte");
  };
  const auto first_extent=get(valid,8,8);
  for(std::size_t byte=0;byte<first_extent+64;++byte) {
    auto corrupt=valid;corrupt[byte]^=1;refused_unchanged(corrupt);
  }
  for(std::size_t length=0;length<first_extent+64;++length)
    refused_unchanged(valid.substr(0,length));
  // Rehash valid-looking contradictions independently; a hash check alone is insufficient.
  const auto reseal = [&](std::string* bytes,std::size_t at) {
    const auto extent=get(*bytes,at+8,8);
    const auto hash=digest(std::string_view(*bytes).substr(at,extent));
    bytes->replace(at+248,32,hash);bytes->replace(at+extent+32,32,hash);
  };
  auto conflicting=valid;
  conflicting[last_offset+64]^=1;reseal(&conflicting,last_offset);
  refused_unchanged(conflicting); // Same descriptor but changed principal on revoke.
  conflicting=valid.substr(0,first_extent+64);
  conflicting[136]^=1;reseal(&conflicting,0);
  refused_unchanged(conflicting); // Outer catalog generation differs from canonical ERVD.
  refused_unchanged("E\tlegacy-text-registry\nS\tlegacy-text-registry\n");
  replace(valid);
  check(!RecoverSblrErrorVectorDescriptorRegistryV1(admin).error,
        "valid immutable evidence recovers after isolated corruption fixtures");
#if defined(SB_EV_LINUX_IO_FAULTS)
  const auto current_bytes = [&] {
    std::ifstream input_bytes(journal,std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input_bytes)),{});
  };
  // Actual OS errors during replay, evidence, commit and sync. No mocked registry.
  for(const auto& [kind,at]:{std::pair{1u,1u}, {2u,1u}, {2u,2u},
                           {3u,1u}, {3u,2u}, {3u,3u}, {4u,1u}, {4u,2u},
                           {5u,1u}, {5u,2u}}) {
    replace(valid);fault_kind=kind;fault_at=at;fault_seen=0;fault_fired=false;
    const auto failed=IssueSblrErrorVectorDescriptorV1(
        context,receipt_id,registry_id,41,diagnostics_id,7,{entry});
    fault_kind=0;short_pending=false;
    check(fault_fired && !failed.ok && failed.diagnostic.error,
          "real read/write/short-write/sync failure cannot publish issue success");
    const auto before=current_bytes();
    const auto recovered=RecoverSblrErrorVectorDescriptorRegistryV1(admin);
    if(recovered.error)check(current_bytes()==before,"failed I/O recovery preserves torn evidence");
    else check(current_bytes().substr(0,valid.size())==valid,
               "recoverable complete write retains every prior evidence byte");
  }
  // Abrupt process death at evidence/commit write and sync boundaries.
  for(const auto& [kind,at]:{std::pair{2u,1u},{2u,2u},{3u,1u},{3u,2u},{3u,3u}}) {
    replace(valid);const auto child=::fork();assert(child>=0);
    if(child==0) {
      fault_kind=kind;fault_at=at;fault_seen=0;fault_fired=false;crash_after=true;
      (void)IssueSblrErrorVectorDescriptorV1(context,receipt_id,registry_id,41,diagnostics_id,7,{entry});
      ::_exit(99);
    }
    int status=0;assert(::waitpid(child,&status,0)==child);
    check(WIFEXITED(status) && WEXITSTATUS(status)==int(80+kind),
          "child actually dies at the selected durable boundary");
    const auto before=current_bytes();
    const auto recovered=RecoverSblrErrorVectorDescriptorRegistryV1(admin);
    const bool complete=(kind==2 && at==2) || (kind==3 && (at==1 || at==3));
    check(recovered.error!=complete,"crash recovery distinguishes complete commits from torn evidence");
    if(!complete)check(current_bytes()==before,"crash-torn journal is never erased or repaired by invention");
  }
  replace(valid);
#endif
  unsigned allocation_failures=0;
  for(bool issue_operation:{false,true}) {
    replace(valid);
    const auto active=IssueSblrErrorVectorDescriptorV1(
        context,receipt_id,registry_id,41,diagnostics_id,7,{entry});
    assert(active.ok);
    std::ifstream active_input(journal,std::ios::binary);
    const std::string active_bytes((std::istreambuf_iterator<char>(active_input)),{});
    bool complete=false;
    for(long index=0;index<10000;++index) {
      if(index%250==0)std::cerr<<"allocation sweep issue="<<issue_operation<<" index="<<index<<'\n';
      if(issue_operation)replace(valid);
      std::vector<scratchbird::engine::sblr::SblrErrorVectorEntryV1> arguments{entry};
      SblrErrorVectorRegistryResultV1 outcome;
      allocation_fault::remaining=index;allocation_fault::injected=false;
      try {
        if(issue_operation)outcome=IssueSblrErrorVectorDescriptorV1(
            context,receipt_id,registry_id,41,diagnostics_id,7,std::move(arguments));
        else outcome=LookupSblrErrorVectorDescriptorV1(
            context,receipt_id,active.snapshot.descriptor_uuid,1);
      } catch(...) {
        allocation_fault::remaining=-1;
        std::cerr<<"allocation exception escaped index="<<index<<" issue="<<issue_operation<<'\n';
        check(false,"registry must translate allocation failure including refusal construction");break;
      }
      allocation_fault::remaining=-1;
      if(!allocation_fault::injected) {
        check(outcome.ok,"allocation sweep reaches a complete successful operation");complete=true;break;
      }
      ++allocation_failures;
      check(!outcome.ok && outcome.diagnostic.error && outcome.snapshot.canonical_ervd.empty(),
            "allocation failure returns no partial descriptor or fake success");
      std::ifstream after_input(journal,std::ios::binary);
      const std::string after((std::istreambuf_iterator<char>(after_input)),{});
      if(!issue_operation) {
        check(after==active_bytes,"failed read never mutates durable descriptor evidence");
        check(LookupSblrErrorVectorDescriptorV1(context,receipt_id,active.snapshot.descriptor_uuid,1).ok,
              "failed read releases every provisional lock and permits immediate valid lookup");
      } else {
        check(after.substr(0,valid.size())==valid,"failed issue preserves all prior immutable evidence");
      }
    }
    check(complete,"allocation sweep covers every reached standard allocation point");
  }
  std::cout<<"ERROR_VECTOR allocation failures="<<allocation_failures<<'\n';
  replace(valid);
  std::vector<scratchbird::engine::sblr::SblrErrorVectorEntryV1> maximum(4096,entry);
  for(std::size_t i=0;i<maximum.size();++i)maximum[i].occurrence_ordinal=i+1;
  const auto large=IssueSblrErrorVectorDescriptorV1(
      context,receipt_id,registry_id,41,diagnostics_id,7,maximum);
  check(large.ok && large.snapshot.canonical_ervd.size()==524440,
        "full 4096-entry descriptor fits exact maximum frame, not a bounded subset");
  const auto reloaded=LookupSblrErrorVectorDescriptorV1(
      context,receipt_id,large.snapshot.descriptor_uuid,1);
  check(reloaded.ok && reloaded.snapshot.canonical_ervd==large.snapshot.canonical_ervd,
        "large canonical vector survives all read-ahead buffer boundaries exactly");
  maximum.push_back(entry);maximum.back().occurrence_ordinal=4097;
  const auto too_many=IssueSblrErrorVectorDescriptorV1(
      context,receipt_id,registry_id,41,diagnostics_id,7,std::move(maximum));
  check(!too_many.ok && too_many.diagnostic.code=="SBLR.OPERAND_INVALID",
        "4097 entries cannot mutate the admitted descriptor journal");
  replace(valid);
  std::array<SblrErrorVectorRegistryResultV1,8> concurrent;
  std::vector<std::thread> workers;
  for(std::size_t i=0;i<concurrent.size();++i)workers.emplace_back([&,i] {
    concurrent[i]=IssueSblrErrorVectorDescriptorV1(
        context,receipt_id,registry_id,41,diagnostics_id,7,{entry});
  });
  for(auto& worker:workers)worker.join();
  std::set<SblrErrorVectorUuidV1> issued_ids;
  std::set<std::uint64_t> issued_sequences;
  for(const auto& value:concurrent) {
    check(value.ok && value.snapshot.registry_generation==41 &&
          issued_ids.insert(value.snapshot.descriptor_uuid).second &&
          issued_sequences.insert(value.snapshot.journal_sequence).second,
          "concurrent issue preserves unique binary identity and immutable catalog generation");
    check(LookupSblrErrorVectorDescriptorV1(context,receipt_id,value.snapshot.descriptor_uuid,1).ok,
          "every concurrent issue is independently recoverable from the full journal");
  }
  check(issued_sequences.size()==8 && *issued_sequences.begin()==31 &&
        *issued_sequences.rbegin()==38,"concurrent journal sequence has no holes or duplicates");
  std::filesystem::remove(journal, ec);
  std::filesystem::remove(base, ec);
  std::cout << "ERROR_VECTOR owner/lifecycle checks=" << checks
            << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
