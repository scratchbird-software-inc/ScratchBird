// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "create_and_connect.hpp"
#include "../../../src/server/startup_notification.hpp"
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "memory.hpp"
#include "uuid.hpp"
#include <array>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace {
namespace cli=scratchbird::cli;
namespace db=scratchbird::storage::database;
namespace disk=scratchbird::storage::disk;
namespace uuid=scratchbird::core::uuid;
namespace fs=std::filesystem;
using scratchbird::core::platform::UuidKind;
unsigned checks=0;
void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
cli::CreateAndConnectParseResult Parse(std::vector<std::string> words){std::vector<char*> args;for(auto& word:words)args.push_back(word.data());return cli::ParseCreateAndConnectArgs(args.size(),args.data());}
struct Fixture{fs::path root;Fixture(){std::string name=(fs::temp_directory_path()/"sb_create_connect.XXXXXX").string();auto* path=::mkdtemp(name.data());Check(path,"fixture directory");root=path;}~Fixture(){std::error_code ignored;fs::remove_all(root,ignored);}};
void Stop(std::uint64_t pid){::kill(static_cast<pid_t>(pid),SIGTERM);int status=0;while(::waitpid(static_cast<pid_t>(pid),&status,0)<0&&errno==EINTR){}}
int ServerProcessFixture(int argc,char** argv){
    std::string path,config;int notify=-1;
    for(int i=1;i<argc;++i){std::string_view word=argv[i];if((word=="--database"||word=="--config"||word=="--startup-notify-fd")&&i+1<argc){const std::string value=argv[++i];if(word=="--database")path=value;else if(word=="--config")config=value;else notify=std::stoi(value);}}
    std::ifstream input(config);std::string mode;input>>mode;
    std::ofstream(config+".started")<<"started";
    disk::FileDevice owner;if(!owner.Open(path,disk::FileOpenMode::open_existing).ok())return 2;
    disk::SerializedDatabaseHeader bytes{};if(!owner.ReadAt(0,bytes.data(),bytes.size()).ok())return 3;
    auto header=disk::ParseDatabaseHeader(bytes);if(!header.ok())return 4;
    // This subprocess qualifies physical ownership and notification transport,
    // not the production server's recovery, activation or client admission.
    if(mode=="exit")return 5;
    if(mode=="partial"){const char first='S';const auto written=::write(notify,&first,1);return written==1?6:8;}
    if(mode=="timeout"){for(;;)::pause();}
    if(mode=="wrong")header.header.database_uuid.bytes.back()^=1;
    scratchbird::server::StartupNotification notification;
    if(!notification.Adopt(notify)||!notification.Send(header.header.database_uuid))return 7;
    for(;;)::pause();
}
}
int main(int argc,char** argv)try{
    if(argc>1&&std::string_view(argv[1])=="--foreground")return ServerProcessFixture(argc,argv);
    const std::vector<std::string> base={"architect","/tmp/create-node","--mode=embedded","--platform-profile=/tmp/platform","--resource-seed-pack-root=/tmp/resources","--policy-seed-pack-root=/tmp/policies","--start-server=/tmp/SBsrv","--server-config=/tmp/server.conf","--ipc-path=/tmp/server.sock"};
    auto args=base;args.insert(args.end(),{"--password-stdin","--connect-password-stdin","--startup-timeout-ms=1234","--","-c","SELECT 1","--quiet"});
    auto parsed=Parse(args);Check(parsed.ok&&parsed.request.connect_password_stdin&&parsed.request.startup_timeout_ms==1234,"separate creation and connection argument stages");
    Check(parsed.request.creation.ownership==cli::EmbeddedCreationOwnership::kServiceIdentity,"explicit platform profile selects service account handoff");
    auto local_args=base;local_args.erase(local_args.begin()+3);
    const auto local=Parse(local_args);
    Check(local.ok&&local.request.creation.ownership==cli::EmbeddedCreationOwnership::kCurrentProcess&&local.request.creation.platform_profile_path.empty(),"local create requires no service account or administrator profile");
    Check(parsed.request.client_arguments==std::vector<std::string>({"-c","SELECT 1","--quiet"}),"client setup retained for post-authentication execution");
    for(const char* option:{"--connection=remote","--mode=embedded","--ipc-path=foreign","-U","--password=secret","foreign-database"}){auto invalid=base;invalid.insert(invalid.end(),{"--",option});Check(!Parse(invalid).ok,"client handoff cannot change database or transport or supply argv secret");}
    for(const char* option:{"--startup-timeout-ms=0","--startup-timeout-ms=600001","--startup-timeout-ms=bad","--start-server=duplicate"}){auto invalid=base;invalid.emplace_back(option);Check(!Parse(invalid).ok,"invalid launch input refused before creation");}
    Check(Parse({"--help"}).help_requested,"create help is side-effect free");
    Fixture fixture;
    cli::FirstPrincipalBootstrapRequest create;
    create.explicit_embedded_mode=true;create.ownership=cli::EmbeddedCreationOwnership::kCurrentProcess;
    create.database_path=(fixture.root/"node.sbdb").string();create.principal_name="architect";
    create.resource_seed_pack_root=SB_BOOTSTRAP_SEED_PACK_ROOT;create.policy_seed_pack_root=SB_BOOTSTRAP_POLICY_PACK_ROOT;
    create.password_source=cli::FirstPrincipalBootstrapPasswordSource::kProtectedStdin;
    const auto user_before=::geteuid();const auto group_before=::getegid();
    const int group_count=::getgroups(0,nullptr);Check(group_count>=0,"read caller groups");
    std::vector<gid_t> groups_before(group_count),groups_after(group_count);
    Check(::getgroups(group_count,groups_before.data())==group_count,"retain caller groups");
    std::istringstream password("Local-create-test-secret-2026\n");std::ostringstream output;
    const auto receipt=cli::RunFirstPrincipalBootstrap(create,password,output);
    if(!receipt.ok)std::cerr<<"embedded creation diagnostic="<<receipt.diagnostic_code<<'\n';
    Check(receipt.ok&&receipt.creation_handoff_ready,"actual current-user embedded creation completes and closes");
    const auto memory=scratchbird::core::memory::DefaultMemoryManagerState();
    Check(memory.initialized&&memory.explicitly_configured&&!memory.fixture_mode,"CLI owns actual production memory admission");
    Check(::geteuid()==user_before&&::getegid()==group_before&&::getgroups(group_count,groups_after.data())==group_count&&groups_after==groups_before,"embedded creation retains the caller's OS credentials and groups");
    struct stat created_stat{};
    Check(::stat(create.database_path.c_str(),&created_stat)==0&&created_stat.st_uid==user_before&&(created_stat.st_mode&077)==0,"created database belongs to caller with no group or other access");
    const auto committed=db::ReadDatabaseBootstrapSecurityCatalog(create.database_path);
    Check(committed.ok()&&committed.state.committed_by_inventory&&committed.state.principal_name==create.principal_name&&!committed.state.credential_fingerprint.empty(),"first principal and verifier are committed before handoff");
    Check(output.str().find("Local-create-test-secret")==std::string::npos,"creation never prints password");
    Check(cli::RunFirstPrincipalBootstrap(create,password,output).diagnostic_code=="BOOTSTRAP.AUTH_DB_ALREADY_OWNED","embedded creation never overwrites an existing database");
    auto service=create;service.database_path=(fixture.root/"refused.sbdb").string();service.platform_profile_path=(fixture.root/"missing.profile").string();
    Check(!cli::RunFirstPrincipalBootstrap(service,password,output).ok&&!fs::exists(service.database_path),"current-user request cannot silently discard an explicit service profile");
    service.ownership=cli::EmbeddedCreationOwnership::kServiceIdentity;
    Check(cli::RunFirstPrincipalBootstrap(service,password,output).diagnostic_code=="SECURITY.BOOTSTRAP_DENIED"&&!fs::exists(service.database_path),"rejected service profile never falls back to current-user creation");
    const auto database_uuid=uuid::ParseTypedUuid(UuidKind::database,receipt.database_uuid);
    Check(database_uuid.ok(),"creation receipt contains actual database identity");
    cli::CreateAndConnectRequest request;request.creation=create;request.server_program=fs::canonical(argv[0]).string();request.server_config=(fixture.root/"server.conf").string();request.ipc_path=(fixture.root/"server.sock").string();request.startup_timeout_ms=1000;
    {auto incomplete=receipt;incomplete.creation_handoff_ready=false;const auto launch=cli::LaunchCreatedDatabaseServer(request,incomplete);Check(!launch.ok&&launch.diagnostic_code=="BOOTSTRAP.CREATE_VERIFICATION_REQUIRED"&&!fs::exists(request.server_config+".started"),"incomplete creation cannot launch a process");}
    for(const auto* mode:{"ready","wrong","partial","exit","timeout"}){
        std::ofstream(request.server_config)<<mode;
        request.startup_timeout_ms=std::string_view(mode)=="timeout"?100:2000;
        const auto launched=cli::LaunchCreatedDatabaseServer(request,receipt);
        if(std::string_view(mode)=="ready"){
            Check(launched.ok&&launched.server_pid!=0,"fresh process acquires created database and notifies exact binary identity");
            const bool separate_group=::getpgid(static_cast<pid_t>(launched.server_pid))==static_cast<pid_t>(launched.server_pid);
            disk::FileDevice contender;const auto refused=contender.Open(create.database_path,disk::FileOpenMode::open_existing);
            const bool owned=!refused.ok();if(contender.is_open())contender.Close();Stop(launched.server_pid);
            Check(owned,"parent cannot reopen file while dedicated process owns it");
            Check(separate_group,"client terminal cancellation does not target the independent server process group");
        }else{
            const std::string expected=std::string_view(mode)=="wrong"?"BOOTSTRAP.SERVER_DATABASE_IDENTITY_MISMATCH":std::string_view(mode)=="timeout"?"BOOTSTRAP.SERVER_STARTUP_TIMEOUT":"BOOTSTRAP.SERVER_EXITED_BEFORE_READY";
            Check(!launched.ok&&launched.diagnostic_code==expected&&launched.creation.creation_handoff_ready,"startup failure is distinct from successful creation");
        }
        Check(fs::exists(create.database_path),"startup outcome never deletes committed database");
        disk::FileDevice next;Check(next.Open(create.database_path,disk::FileOpenMode::open_existing).ok()&&next.Close().ok(),"failed startup or stopped server releases ownership");
        const auto catalog=db::ReadDatabaseBootstrapSecurityCatalog(create.database_path);Check(catalog.ok()&&catalog.state.committed_by_inventory,"committed creation survives every handoff failure");
    }
    {request.server_program=(fixture.root/"missing-server").string();const auto launched=cli::LaunchCreatedDatabaseServer(request,receipt);Check(!launched.ok&&launched.diagnostic_code=="BOOTSTRAP.SERVER_LAUNCH_FAILED"&&fs::exists(create.database_path),"spawn failure preserves created database");}
    {int pair[2];Check(::socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0,"private readiness socket pair");scratchbird::server::StartupNotification notification;Check(notification.Adopt(pair[1]),"server adopts private channel");::close(pair[0]);Check(!notification.Send(database_uuid.value.value),"disconnected launcher refuses notification without SIGPIPE termination");}
    std::cout<<"create/connect process-boundary checks="<<checks<<" failures=0 production_runtime_activation=false\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
