// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "create_and_connect.hpp"
#include "database_startup_notification.hpp"
#include "uuid.hpp"
#include <array>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;
#endif

namespace scratchbird::cli {
namespace {
CreateAndConnectParseResult Invalid(std::string code) {
    CreateAndConnectParseResult out; out.diagnostic_code=std::move(code); return out;
}
bool PositiveTimeout(std::string_view text, std::uint32_t& out) {
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),out);
    return parsed.ec==std::errc{}&&parsed.ptr==text.data()+text.size()&&out>0&&out<=600000;
}
#ifndef _WIN32
struct Descriptor {
    int value=-1;
    ~Descriptor(){if(value>=0)::close(value);}
};
// Until readiness is received this caller owns the child. An unsuccessful
// launch terminates and reaps that exact process; no unrelated PID or endpoint
// is used for cleanup, and the committed database is never removed.
struct StartingProcess {
    pid_t pid=-1;
    ~StartingProcess(){
        if(pid<=0)return;
        int status=0;
        const auto already=::waitpid(pid,&status,WNOHANG);
        if(already==pid||(already<0&&errno==ECHILD))return;
        ::kill(pid,SIGTERM);
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(std::chrono::steady_clock::now()<end){
            const auto got=::waitpid(pid,&status,WNOHANG);
            if(got==pid||(got<0&&errno==ECHILD))return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ::kill(pid,SIGKILL);
        while(::waitpid(pid,&status,0)<0&&errno==EINTR){}
    }
};
#endif
}

void PrintCreateAndConnectUsage(const char* program,std::ostream& out){
    out<<"Usage: "<<program<<" create <first-principal> <database-path> --mode=embedded\n"
       <<"  --resource-seed-pack-root DIR --policy-seed-pack-root DIR [--platform-profile FILE]\n"
       <<"  --metric-policy-defaults FILE --metric-policy-families FILE\n"
       <<"  --start-server PROGRAM --server-config FILE --ipc-path PATH\n"
       <<"  [--password-stdin] [--connect-password-stdin] [--startup-timeout-ms N]\n"
       <<"  [-- [-c SQL | -f FILE] [--quiet] [--verbose]]\n\n"
       <<"Creates and closes the database, starts its dedicated server, waits for readiness,\n"
       <<"then authenticates through local IPC. Creation and connection passwords are read\n"
       <<"separately. A failed startup preserves the created database.\n";
    out<<"Creation uses the current OS account. An explicit platform profile selects\n"
       <<"the administrator-authorized service-account handoff.\n";
}

CreateAndConnectParseResult ParseCreateAndConnectArgs(int argc,char* const argv[]){
    if(argc==1&&argv&&argv[0]&&std::string_view(argv[0])=="--help"){
        CreateAndConnectParseResult out;out.ok=true;out.help_requested=true;return out;
    }
    CreateAndConnectParseResult out;
    std::vector<char*> bootstrap;
    bool server=false,config=false,ipc=false,timeout=false,login=false,client=false;
    for(int i=0;i<argc;++i){
        if(!argv||!argv[i])return Invalid("BOOTSTRAP.AUTH_DB_INPUT_INVALID");
        const std::string_view token=argv[i];
        if(client){
            // Routes and credentials are fixed for this handoff. Ordinary SQL
            // execution/output flags are parsed by SBsql before creation.
            const bool value=token=="-c"||token=="--command"||token=="-f"||token=="--file"||token=="-i"||token=="--input"||token=="-F"||token=="--field-separator";
            const bool flag=token=="-q"||token=="--quiet"||token=="-v"||token=="--verbose"||token=="-t"||token=="--tuples-only"||token=="-A"||token=="--no-align"||token=="-b"||token=="--bail"||token=="-e"||token=="--echo";
            if(!value&&!flag)return Invalid("BOOTSTRAP.CONNECT_OPTION_NOT_ALLOWED");
            out.request.client_arguments.emplace_back(token);
            if(value){if(++i>=argc||!argv[i])return Invalid("BOOTSTRAP.AUTH_DB_INPUT_INVALID");out.request.client_arguments.emplace_back(argv[i]);}
            continue;
        }
        if(token=="--"){client=true;continue;}
        if(token=="--connect-password-stdin"){
            if(login)return Invalid("BOOTSTRAP.AUTH_DB_INPUT_INVALID");login=true;out.request.connect_password_stdin=true;continue;
        }
        const auto named=[&](std::string_view name){return token==name||token.starts_with(std::string(name)+"=");};
        std::string* destination=nullptr;bool* seen=nullptr;std::string_view name;
        if(named("--start-server")){destination=&out.request.server_program;seen=&server;name="--start-server";}
        else if(named("--server-config")){destination=&out.request.server_config;seen=&config;name="--server-config";}
        else if(named("--ipc-path")){destination=&out.request.ipc_path;seen=&ipc;name="--ipc-path";}
        else if(named("--startup-timeout-ms")){seen=&timeout;name="--startup-timeout-ms";}
        if(seen){
            if(*seen)return Invalid("BOOTSTRAP.AUTH_DB_INPUT_INVALID");*seen=true;
            std::string value;
            if(token==name){if(++i>=argc||!argv[i])return Invalid("BOOTSTRAP.AUTH_DB_INPUT_INVALID");value=argv[i];}
            else value=token.substr(name.size()+1);
            if(value.empty()||value.front()=='-')return Invalid("BOOTSTRAP.AUTH_DB_INPUT_INVALID");
            if(destination)*destination=std::move(value);
            else if(!PositiveTimeout(value,out.request.startup_timeout_ms))return Invalid("BOOTSTRAP.STARTUP_TIMEOUT_INVALID");
            continue;
        }
        bootstrap.push_back(argv[i]);
    }
    if(!server||!config||!ipc)return Invalid("BOOTSTRAP.HANDOFF_PARAMETERS_REQUIRED");
    auto parsed=ParseEmbeddedDatabaseCreateArgs(static_cast<int>(bootstrap.size()),bootstrap.data());
    if(!parsed.ok||parsed.help_requested)return Invalid(parsed.diagnostic_code.empty()?"BOOTSTRAP.AUTH_DB_INPUT_INVALID":parsed.diagnostic_code);
    out.request.creation=std::move(parsed.request);
    // Resolve relative paths before the bootstrap changes OS credentials.
    std::error_code error;
    for(auto* path:{&out.request.server_program,&out.request.server_config,&out.request.ipc_path,&out.request.creation.database_path}){
        *path=std::filesystem::absolute(*path,error).lexically_normal().string();
        if(error)return Invalid("BOOTSTRAP.HANDOFF_PATH_INVALID");
    }
    out.ok=true;return out;
}

CreateAndConnectResult LaunchCreatedDatabaseServer(const CreateAndConnectRequest& request,FirstPrincipalBootstrapResult creation){
    CreateAndConnectResult out;out.creation=std::move(creation);
    const auto fail=[&](const char* code){out.diagnostic_code=code;return out;};
    if(!out.creation.ok||!out.creation.creation_handoff_ready||out.creation.committed_with_warning)
        return fail("BOOTSTRAP.CREATE_VERIFICATION_REQUIRED");
    const auto identity=core::uuid::ParseTypedUuid(core::platform::UuidKind::database,out.creation.database_uuid);
    if(!identity.ok()||request.server_program.empty()||request.server_config.empty()||request.creation.database_path.empty()||request.ipc_path.empty()||!request.startup_timeout_ms||request.startup_timeout_ms>600000)
        return fail("BOOTSTRAP.HANDOFF_PARAMETERS_REQUIRED");
#ifdef _WIN32
    return fail("BOOTSTRAP.LOCAL_IPC_HANDOFF_UNSUPPORTED");
#else
    int sockets[2];
    if(::socketpair(AF_UNIX,SOCK_STREAM,0,sockets)!=0)return fail("BOOTSTRAP.SERVER_LAUNCH_FAILED");
    Descriptor reader{sockets[0]},writer{sockets[1]};
    // Child descriptor 3 is explicit; every other socket copy is close-on-exec.
    if(::fcntl(reader.value,F_SETFD,FD_CLOEXEC)<0||::fcntl(writer.value,F_SETFD,FD_CLOEXEC)<0)
        return fail("BOOTSTRAP.SERVER_LAUNCH_FAILED");
    posix_spawn_file_actions_t actions;
    if(::posix_spawn_file_actions_init(&actions)!=0)return fail("BOOTSTRAP.SERVER_LAUNCH_FAILED");
    struct ActionsCleanup{posix_spawn_file_actions_t* actions;~ActionsCleanup(){::posix_spawn_file_actions_destroy(actions);}}cleanup{&actions};
    // Do not close reader after dup2 when its descriptor happens to be 3.
    int setup=::posix_spawn_file_actions_addclose(&actions,reader.value);
    if(!setup)setup=::posix_spawn_file_actions_adddup2(&actions,writer.value,3);
    if(!setup&&writer.value!=3)setup=::posix_spawn_file_actions_addclose(&actions,writer.value);
    if(!setup)setup=::posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    if(setup)return fail("BOOTSTRAP.SERVER_LAUNCH_FAILED");
    std::vector<std::string> words={request.server_program,"--foreground","--config",request.server_config,
        "--database",request.creation.database_path,"--sbps-endpoint",request.ipc_path,"--no-listeners","--startup-notify-fd","3"};
    std::vector<char*> args;for(auto& word:words)args.push_back(word.data());args.push_back(nullptr);
    posix_spawnattr_t attributes;
    if(::posix_spawnattr_init(&attributes)!=0)return fail("BOOTSTRAP.SERVER_LAUNCH_FAILED");
    struct AttributesCleanup{posix_spawnattr_t* attributes;~AttributesCleanup(){::posix_spawnattr_destroy(attributes);}}attributes_cleanup{&attributes};
    sigset_t signal_mask;::sigemptyset(&signal_mask);
    if(::posix_spawnattr_setpgroup(&attributes,0)!=0 ||
       ::posix_spawnattr_setsigmask(&attributes,&signal_mask)!=0 ||
       ::posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETPGROUP|POSIX_SPAWN_SETSIGMASK)!=0)
        return fail("BOOTSTRAP.SERVER_LAUNCH_FAILED");
    StartingProcess child;
    pid_t spawned=-1;
    if(::posix_spawn(&spawned,request.server_program.c_str(),&actions,&attributes,args.data(),environ)!=0)
        return fail("BOOTSTRAP.SERVER_LAUNCH_FAILED");
    child.pid=spawned;
    ::close(writer.value);writer.value=-1;
    std::array<std::uint8_t,core::platform::kDatabaseStartupNotificationBytes> packet{};
    std::size_t used=0;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(request.startup_timeout_ms);
    while(used<packet.size()){
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(remaining<=0)return fail("BOOTSTRAP.SERVER_STARTUP_TIMEOUT");
        pollfd fd{reader.value,POLLIN,0};const auto polled=::poll(&fd,1,static_cast<int>(remaining));
        if(polled<0&&errno==EINTR)continue;
        if(polled<0)return fail("BOOTSTRAP.SERVER_READINESS_FAILED");
        if(!polled)return fail("BOOTSTRAP.SERVER_STARTUP_TIMEOUT");
        const auto got=::read(reader.value,packet.data()+used,packet.size()-used);
        if(got<0&&errno==EINTR)continue;
        if(got<=0)return fail("BOOTSTRAP.SERVER_EXITED_BEFORE_READY");
        used+=static_cast<std::size_t>(got);
    }
    if(!core::platform::DatabaseStartupNotificationMatches(packet,identity.value.value.bytes))
        return fail("BOOTSTRAP.SERVER_DATABASE_IDENTITY_MISMATCH");
    int status=0;
    const auto exited=::waitpid(child.pid,&status,WNOHANG);
    if(exited==child.pid){child.pid=-1;return fail("BOOTSTRAP.SERVER_EXITED_BEFORE_READY");}
    if(exited<0)return fail("BOOTSTRAP.SERVER_READINESS_FAILED");
    out.server_pid=static_cast<std::uint64_t>(child.pid);child.pid=-1;out.ok=true;return out;
#endif
}

CreateAndConnectResult RunCreateAndConnect(const CreateAndConnectRequest& request,std::istream& input,std::ostream& output){
#ifdef _WIN32
    CreateAndConnectResult out;out.diagnostic_code="BOOTSTRAP.LOCAL_IPC_HANDOFF_UNSUPPORTED";return out;
#else
    if(request.server_program.empty()||request.server_config.empty()||request.ipc_path.empty()||
       request.creation.database_path.empty()||!request.startup_timeout_ms||request.startup_timeout_ms>600000){
        CreateAndConnectResult out;out.diagnostic_code="BOOTSTRAP.HANDOFF_PARAMETERS_REQUIRED";return out;
    }
    // Fail before creating a database if its launch target is unavailable.
    std::error_code error;
    const bool program_regular=std::filesystem::is_regular_file(request.server_program,error);
    const bool config_regular=std::filesystem::is_regular_file(request.server_config,error);
    if(!program_regular||!config_regular||::access(request.server_program.c_str(),X_OK)!=0||::access(request.server_config.c_str(),R_OK)!=0){
        CreateAndConnectResult out;out.diagnostic_code="BOOTSTRAP.SERVER_LAUNCH_INPUT_UNAVAILABLE";return out;
    }
    auto creation=RunFirstPrincipalBootstrap(request.creation,input,output);
    if(!creation.ok){CreateAndConnectResult out;out.diagnostic_code=creation.diagnostic_code;out.creation=std::move(creation);return out;}
    output<<"database_state=created\ncreation_handoff=verified_and_closed\n";output.flush();
    auto out=LaunchCreatedDatabaseServer(request,std::move(creation));
    if(out.ok){output<<"server_ready=true\nserver_pid="<<out.server_pid<<"\n";output.flush();}
    return out;
#endif
}
} // namespace scratchbird::cli
