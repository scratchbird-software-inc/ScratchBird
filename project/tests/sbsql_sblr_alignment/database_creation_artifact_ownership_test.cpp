// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>

namespace {
namespace db=scratchbird::storage::database;
namespace disk=scratchbird::storage::disk;
namespace fs=std::filesystem;
constexpr std::array<const char*,4> suffixes{{".sb.txn_publish.tmp",".sb.txn_publish",".sb.security_principal_events",".sb.local_password_auth"}};
const std::string sentinel("preserve\0private\xff" "data",21);
std::string tracked_main,late_artifact;
unsigned inspections=0,inspection_fault=0,inject_on_inspection=0,removals=0;
bool inject_on_write=false,late_created=false,remove_fault=false,removal_owned=true;
unsigned checks=0;
void Check(bool value,std::string_view why,std::source_location at=std::source_location::current()){
  ++checks;if(!value)throw std::runtime_error(std::string(why)+" line="+std::to_string(at.line()));
}
void Put(const fs::path& path,const std::string& bytes=sentinel){
  std::ofstream stream(path,std::ios::binary|std::ios::trunc);stream.write(bytes.data(),bytes.size());stream.close();
  if(!stream)throw std::runtime_error("fixture write failed");
}
std::string Get(const fs::path& path){std::ifstream stream(path,std::ios::binary);Check(bool(stream),"open preserved fixture bytes");return {std::istreambuf_iterator<char>(stream),{}};}
bool IsArtifact(const fs::path& path){for(const auto* suffix:suffixes)if(path==tracked_main+suffix)return true;return false;}
void Inject(){Put(late_artifact);late_created=true;}
struct Fixture{
  fs::path root;
  Fixture(){std::string path=(fs::temp_directory_path()/"sb_create_ownership.XXXXXX").string();
    char* actual=::mkdtemp(path.data());Check(actual,"create isolated fixture directory");root=actual;}
  ~Fixture(){tracked_main.clear();late_artifact.clear();std::error_code error;fs::remove_all(root,error);}
};
db::DatabaseCreateConfig Config(const fs::path& path,std::string fault={}){
  using scratchbird::core::platform::UuidKind;
  static std::uint64_t tick=1790000000000ULL;
  const auto database=scratchbird::core::uuid::GenerateDurableEngineIdentityV7(UuidKind::database,++tick);
  const auto filespace=scratchbird::core::uuid::GenerateDurableEngineIdentityV7(UuidKind::filespace,tick);
  Check(database.ok()&&filespace.ok(),"fixture identities from real UUID source");
  db::DatabaseCreateConfig config;config.path=path.string();config.database_uuid=database.value;config.filespace_uuid=filespace.value;
  config.creation_unix_epoch_millis=tick;config.resource_seed_pack_root=SB_BOOTSTRAP_SEED_PACK_ROOT;
  config.require_resource_seed_pack=true;config.bootstrap_principal_name="ROOT";config.require_bootstrap_principal=true;
  config.bootstrap_credential_fingerprint="local-password-pbkdf2-sha256:v1:iterations=600000:salt=89abcdef0123456789abcdef01234567:verifier=abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
  config.create_fault_injection_point=std::move(fault);return config;
}
void Track(const fs::path& path){tracked_main=path.string();late_artifact.clear();inspections=inspection_fault=inject_on_inspection=removals=0;
  inject_on_write=late_created=remove_fault=false;removal_owned=true;}
void Unpublished(const db::DatabaseLifecycleResult& result){Check(!result.ok()&&result.create_finality==db::DatabaseCreateFinalityClass::not_published,"refusal has no creation publication");}
}

extern "C" fs::file_status __real__ZNSt10filesystem14symlink_statusERKNS_7__cxx114pathERSt10error_code(const fs::path&,std::error_code&);
extern "C" fs::file_status __wrap__ZNSt10filesystem14symlink_statusERKNS_7__cxx114pathERSt10error_code(const fs::path& path,std::error_code& error){
  if(!tracked_main.empty()&&IsArtifact(path)){
    ++inspections;
    if(inject_on_inspection&&inspections==inject_on_inspection){inject_on_inspection=0;Inject();}
    if(inspection_fault&&inspections==inspection_fault){inspection_fault=0;error=std::make_error_code(std::errc::permission_denied);return fs::file_status(fs::file_type::none);}
  }
  return __real__ZNSt10filesystem14symlink_statusERKNS_7__cxx114pathERSt10error_code(path,error);
}
extern "C" bool __real__ZNSt10filesystem6removeERKNS_7__cxx114pathERSt10error_code(const fs::path&,std::error_code&);
extern "C" bool __wrap__ZNSt10filesystem6removeERKNS_7__cxx114pathERSt10error_code(const fs::path& path,std::error_code& error){
  if(!tracked_main.empty()&&path==tracked_main){++removals;disk::FileDevice other;
    const auto probe=other.Open(path.string(),disk::FileOpenMode::open_existing);
    const auto& code=probe.diagnostic.diagnostic_code;
    removal_owned&=!probe.ok()&&(code=="SB-STORAGE-DISK-OWNER-LOCK-HELD"||code=="SB-STORAGE-DISK-DATA-OWNER-LOCK-HELD"||code=="SB-STORAGE-DISK-ROUTE-OWNER-LOCK-HELD");
    if(other.is_open())(void)other.Close();
    if(remove_fault){remove_fault=false;error=std::make_error_code(std::errc::permission_denied);return false;}
  }
  return __real__ZNSt10filesystem6removeERKNS_7__cxx114pathERSt10error_code(path,error);
}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t count,off_t offset){
  if(inject_on_write){inject_on_write=false;Inject();}
  return __real_pwrite(fd,data,count,offset);
}

int main(){try{
  auto policy=scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();policy.policy_name="creation_artifact_ownership_test";
  Check(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(policy,"creation_artifact_ownership_test").ok(),"fixture memory policy");
  Fixture fixture;unsigned serial=0;
  const auto next=[&](){const auto path=fixture.root/("node-"+std::to_string(serial++));Track(path);return path;};
  for(const auto* suffix:suffixes)for(unsigned kind=0;kind<5;++kind){const auto path=next(),artifact=fs::path(path.string()+suffix),target=fixture.root/("target-"+std::to_string(serial));
    if(kind==0)Put(artifact);if(kind==1)Put(artifact,{});if(kind==2){fs::create_directory(artifact);Put(artifact/"child");}
    if(kind>=3){if(kind==3)Put(target);fs::create_symlink(target,artifact);}
    const auto status=fs::symlink_status(artifact);const auto config=Config(path);
    const auto result=db::CreateDatabaseFile(config);Unpublished(result);
    Check(result.diagnostic.diagnostic_code=="STORAGE.CREATE_ARTIFACT_CONFLICT"&&removals==0&&!fs::exists(path),"early conflict never creates or cleans main file");
    Check(fs::symlink_status(artifact).type()==status.type(),"preexisting directory entry type preserved");
    if(kind<2)Check(Get(artifact)==(kind?std::string{}:sentinel),"regular and empty file bytes preserved");
    if(kind==2)Check(Get(artifact/"child")==sentinel,"directory and descendants preserved");
    if(kind>=3){Check(fs::read_symlink(artifact)==target,"symlink itself preserved without following");if(kind==3)Check(Get(target)==sentinel,"symlink target unchanged");else Check(!fs::exists(target),"dangling target not created");}
    for(const auto& argument:result.diagnostic.arguments)Check(argument.value.find(sentinel)==std::string::npos,"artifact contents not diagnostic material");
  }
  for(unsigned fault=1;fault<=8;++fault){const auto path=next();const auto config=Config(path);inspection_fault=fault;
    const auto result=db::CreateDatabaseFile(config);Unpublished(result);
    Check(!inspection_fault&&result.diagnostic.diagnostic_code=="STORAGE.CREATE_ARTIFACT_INSPECTION_FAILED"&&!fs::exists(path),"every early and retained-owner inspection failure refuses");
    Check(removals==(fault>4?1u:0u)&&removal_owned,"only owned main file cleaned on late inspection failure");
  }
  for(const auto* suffix:suffixes){const auto path=next();const auto config=Config(path);late_artifact=path.string()+suffix;inject_on_inspection=5;
    const auto result=db::CreateDatabaseFile(config);Unpublished(result);
    Check(late_created&&!inject_on_inspection&&result.diagnostic.diagnostic_code=="STORAGE.CREATE_ARTIFACT_CONFLICT","recheck detects entry created after first admission");
    Check(!fs::exists(path)&&Get(late_artifact)==sentinel&&removals==1&&removal_owned,"recheck conflict preserves external bytes and cleans only still-owned main file");
  }
  for(const auto* suffix:suffixes){const auto path=next();const auto config=Config(path,"before_catalog");late_artifact=path.string()+suffix;inject_on_write=true;
    const auto result=db::CreateDatabaseFile(config);Unpublished(result);
    Check(late_created&&!inject_on_write&&Get(late_artifact)==sentinel,"prepublication cleanup cannot erase a later unowned auxiliary artifact");
    Check(!fs::exists(path)&&removals==1&&removal_owned,"prepublication main cleanup retains ownership");
  }
  {const auto parent=fixture.root/"not-a-directory";Put(parent);const auto path=parent/"node";Track(path);
    const auto result=db::CreateDatabaseFile(Config(path));Unpublished(result);
    Check(result.diagnostic.diagnostic_code=="STORAGE.CREATE_ARTIFACT_INSPECTION_FAILED"&&Get(parent)==sentinel,"actual filesystem inspection failure is not absence");}
  {const auto path=next();const auto config=Config(path,"before_catalog");remove_fault=true;
    const auto result=db::CreateDatabaseFile(config);Unpublished(result);
    Check(!remove_fault&&fs::is_regular_file(path)&&removals==1&&removal_owned,"failed removal preserves partial bytes without post-unlock rename");
    disk::FileDevice probe;Check(probe.Open(path.string(),disk::FileOpenMode::open_existing_read_only).ok()&&probe.Close().ok(),"failed cleanup releases device without deleting bytes");}
  {const auto path=next();auto config=Config(path);const auto created=db::CreateDatabaseFile(config);
    Check(created.ok()&&created.create_finality==db::DatabaseCreateFinalityClass::committed&&fs::exists(path)&&removals==0,"ordinary credentialed creation still commits real storage");
    const auto actual=db::ReadDatabaseBootstrapSecurityCatalog(path.string());Check(actual.ok()&&actual.state.present&&actual.state.committed_by_inventory,"independent bootstrap reader sees actual committed catalog");
    const auto bytes=Get(path);config.allow_overwrite=true;const auto replay=db::CreateDatabaseFile(config);Unpublished(replay);
    Check(Get(path)==bytes&&removals==0,"replay cannot erase or overwrite committed storage");}
  std::cout<<"creation artifact ownership checks="<<checks<<" failures=0\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<" checks="<<checks<<'\n';return 1;}}
