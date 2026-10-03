// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "ipc_server.hpp"
#include "database_lifecycle.hpp"
#include "time.hpp"
#include "uuid.hpp"
#include "sbps.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <semaphore>
#include <thread>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
namespace srv = scratchbird::server;
namespace core = scratchbird::core;
using namespace std::chrono_literals;
std::binary_semaphore ready{0}, consumed{0}, done{0};
std::atomic<int> first_fd{-1};
std::atomic<int> listening_fd{-1};
std::atomic<bool> observe_read{true}, first_joined{false};
bool launch_failure = false;
bool allocation_failure = false;
bool cohort_allocation_failure = false;
thread_local bool fail_allocation = false;
thread_local unsigned accepted_count = 0;
thread_local bool client_launch = false;
pthread_t first_thread{};
extern "C" int __real_listen(int, int);
extern "C" int __wrap_listen(int fd, int backlog) {
  const int rc = __real_listen(fd, backlog);
  if (!rc) listening_fd = fd;
  return rc;
}
extern "C" int __real_accept(int, sockaddr*, socklen_t*);
extern "C" int __wrap_accept(int fd, sockaddr* address, socklen_t* length) {
  const int accepted = __real_accept(fd, address, length);
  if (accepted >= 0) {
    ++accepted_count; client_launch = true;
    if (accepted_count == 1) first_fd = accepted;
    if (accepted_count == 2 && cohort_allocation_failure) fail_allocation = true;
  }
  return accepted;
}
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void*(*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* id, const pthread_attr_t* attr, void*(*call)(void*), void* data) {
  const bool client = std::exchange(client_launch, false);
  if (client && accepted_count == 2 && launch_failure) return EAGAIN;
  const int rc = __real_pthread_create(id, attr, call, data);
  if (client && accepted_count == 1 && !rc) first_thread = *id;
  return rc;
}
extern "C" int __real_pthread_join(pthread_t, void**);
extern "C" int __wrap_pthread_join(pthread_t id, void** result) {
  const int rc = __real_pthread_join(id, result);
  if (accepted_count && !rc && ::pthread_equal(id, first_thread)) first_joined = true;
  return rc;
}
extern "C" ssize_t __real_recv(int, void*, size_t, int);
extern "C" ssize_t __wrap_recv(int fd, void* data, size_t size, int flags) {
  const auto rc = __real_recv(fd, data, size, flags);
  if (fd == first_fd && rc > 0 && observe_read.exchange(false)) {
    fail_allocation = allocation_failure;
    consumed.release();
  }
  return rc;
}
extern "C" void* RealNew(std::size_t) asm("__real__Znwm");
extern "C" void* WrapNew(std::size_t size) asm("__wrap__Znwm");
extern "C" void* WrapNew(std::size_t size) {
  if (std::exchange(fail_allocation, false)) throw std::bad_alloc();
  return RealNew(size);
}
void Require(bool value, const char* label) {
  if (!value) throw std::runtime_error(label);
}
int Connect(const std::filesystem::path& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un address{}; address.sun_family = AF_UNIX;
  const auto name = path.string();
  Require(fd >= 0 && name.size() < sizeof(address.sun_path), "socket path");
  std::memcpy(address.sun_path, name.c_str(), name.size()+1);
  Require(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "actual connect");
  return fd;
}
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string mode = argv[1];
  launch_failure = mode == "launch-failure";
  allocation_failure = mode == "allocation-failure";
  cohort_allocation_failure = mode == "cohort-allocation-failure";
  const bool ready_failure = mode == "ready-failure" || mode == "ready-stop-failure";
  const bool callback_failure = mode == "callback-failure" || mode == "ready-stop-failure";
  const bool complete_frame = mode == "complete-frame";
  const bool expected_exception = launch_failure || allocation_failure || callback_failure || cohort_allocation_failure || ready_failure;
  std::string name = (std::filesystem::temp_directory_path()/"sb-client-drain-XXXXXX").string();
  if (!::mkdtemp(name.data())) return 2;
  const std::filesystem::path root(name);
  try {
    const auto clock = core::time::ReadLocalNodeClockSnapshot();
    Require(clock.ok(), "clock");
    const auto millis = core::time::WallClockToUuidV7Millis(clock.value.wall_clock);
    const auto database_id = core::uuid::GenerateEngineIdentityV7(core::platform::UuidKind::database, millis.unix_epoch_millis);
    const auto filespace_id = core::uuid::GenerateEngineIdentityV7(core::platform::UuidKind::filespace, millis.unix_epoch_millis);
    const auto server_id = core::uuid::GenerateEngineIdentityV7(core::platform::UuidKind::object, millis.unix_epoch_millis);
    Require(millis.ok() && database_id.ok() && filespace_id.ok() && server_id.ok(), "native identities");
    scratchbird::storage::database::DatabaseCreateConfig create;
    create.path = (root/"runtime.sbdb").string(); create.database_uuid = database_id.value;
    create.filespace_uuid = filespace_id.value; create.creation_unix_epoch_millis = millis.unix_epoch_millis;
    create.allow_minimal_resource_bootstrap = true; create.require_resource_seed_pack = false;
    Require(scratchbird::storage::database::CreateDatabaseFile(create).ok(), "actual database");
    srv::HostedDatabaseSnapshot database;
    database.state = srv::HostedDatabaseState::kOpen; database.database_path = create.path;
    database.database_uuid = database_id.value.value; database.filespace_uuid = filespace_id.value.value;
    database.database_created = database.database_open = database.config_policy_security_lifecycle_present = true;
    database.write_admission_fenced = false;
    database.selected_agent_type_ids = {"page_allocation_manager", "filespace_capacity_manager"};
    srv::HostedEngineState engine; engine.engine_context_active = true; engine.databases.push_back(database);
    srv::ServerBootstrapConfig config;
    config.control_dir = root/"control"; config.data_dir = root; config.database_default_path = create.path;
    config.sbps_endpoint = root/"ipc.sock"; config.lifecycle_state_file = root/"state";
    config.lifecycle_journal_file = root/"journal"; config.log_file = (root/"server.log").string();
    std::filesystem::create_directories(config.control_dir);
    srv::ServerLifecycleArtifacts artifacts; artifacts.server_uuid = server_id.value.value; artifacts.generation = 1;
    srv::ResetParserServerStopRequest();
    bool stopping_called = false;
    srv::ParserServerIpcLifecycleCallbacks callbacks;
    callbacks.on_ready = [&] {
      ready.release();
      if (ready_failure) throw std::runtime_error("ready callback fault");
    };
    callbacks.on_stopping = [&] {
      stopping_called = true;
      if (callback_failure) throw std::runtime_error("stop callback fault");
    };
    bool exception = false; int exit = -1;
    std::thread endpoint([&] {
      try { exit = srv::RunParserServerIpcEndpoint(config, artifacts, engine, callbacks).exit_code; }
      catch (const std::system_error& error) { exception = error.code().value() == EAGAIN; }
      catch (const std::bad_alloc&) { exception = allocation_failure || cohort_allocation_failure; }
      catch (const std::runtime_error& error) {
        exception = ready_failure ? std::string_view(error.what()) == "ready callback fault"
                                  : callback_failure && std::string_view(error.what()) == "stop callback fault";
      }
      catch (...) { exception = false; }
      done.release();
    });
    if (!ready.try_acquire_for(10s)) std::abort();
    if (ready_failure) {
      if (!done.try_acquire_for(5s)) std::abort();
      endpoint.join();
      const int fd = listening_fd.load();
      errno = 0;
      const bool closed = fd >= 0 && ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
      std::ifstream file(config.lifecycle_state_file);
      const std::string state{std::istreambuf_iterator<char>(file), {}};
      const bool removed = !std::filesystem::exists(config.sbps_endpoint);
      const bool failed = state.find("state=failed") != std::string::npos;
      std::cout << "listener_closed=" << closed << " endpoint_removed=" << removed
                << " stopping_called=" << stopping_called << " failed_state=" << failed
                << " retained_primary=" << exception << '\n';
      // No client was ever accepted. Readiness failure still owns the listener
      // and agent cohort; a later stop callback must not replace the first fault.
      Require(exception && closed && removed && stopping_called && failed && first_fd < 0,
              "readiness failure cleanup");
      std::filesystem::remove_all(root);
      return 0;
    }
    int peer = Connect(config.sbps_endpoint);
    std::vector<std::uint8_t> input{0x53};
    if (complete_frame) {
      srv::sbps::FrameHeader header;
      header.message_type = static_cast<std::uint16_t>(srv::sbps::MessageType::kHello);
      input = srv::sbps::EncodeFrame(header, {}); // Complete but inadmissible hello.
    }
    if (mode == "partial-payload" || allocation_failure) {
      srv::sbps::FrameHeader header;
      input = srv::sbps::EncodeFrame(header, std::vector<std::uint8_t>(32, 0x5a));
      input.resize(input.size()-31);
    }
    Require(::send(peer, input.data(), input.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(input.size()), "actual partial frame");
    if (!consumed.try_acquire_for(10s)) std::abort();
    bool response_received = !complete_frame;
    if (complete_frame) {
      timeval timeout{5,0};
      Require(::setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0,"peer deadline");
      const auto receive = [&](std::vector<std::uint8_t>& bytes) {
        std::size_t at=0;
        while (at<bytes.size()) {
          const auto count=::recv(peer,bytes.data()+at,bytes.size()-at,0);
          if (count<=0) return false;
          at+=static_cast<std::size_t>(count);
        }
        return true;
      };
      std::vector<std::uint8_t> response(srv::sbps::kHeaderBytes);
      if (receive(response)) {
        const auto length=srv::sbps::PayloadLengthFromHeader(response);
        if (length && *length<=config.sbps_max_frame_bytes) {
          std::vector<std::uint8_t> payload(*length);
          if (receive(payload)) {
            response.insert(response.end(),payload.begin(),payload.end());
            const auto decoded=srv::sbps::DecodeFrameBytes(response,config.sbps_max_frame_bytes);
            response_received=decoded.ok() && decoded.frame &&
                (decoded.frame->header.flags & srv::sbps::kFlagError)!=0;
          }
        }
      }
    }
    int second = -1;
    if (launch_failure || cohort_allocation_failure) second = Connect(config.sbps_endpoint);
    else if (!allocation_failure) srv::RequestParserServerStop();
    const bool bounded = done.try_acquire_for(5s);
    std::uint8_t byte{};
    const auto closed_read = bounded ? ::recv(peer, &byte, 1, MSG_DONTWAIT) : -1;
    const int closed_error = closed_read < 0 ? errno : 0;
    // Stop or a fault can leave payload bytes unread after the header. A real
    // close may therefore report ECONNRESET rather than orderly EOF on Linux.
    const bool socket_closed = bounded && (closed_read == 0 ||
        (closed_read < 0 && closed_error == ECONNRESET));
    const bool rejected_closed = second < 0 || (bounded && ::recv(second, &byte, 1, MSG_DONTWAIT) == 0);
    // Baseline can now unwind after its failed bounded-shutdown observation.
    ::close(peer); peer = -1;
    if (second >= 0) ::close(second);
    if (!bounded && !done.try_acquire_for(10s)) std::abort();
    endpoint.join();
    std::ifstream file(config.lifecycle_state_file);
    const std::string state{std::istreambuf_iterator<char>(file), {}};
    const bool passed = bounded && first_joined && response_received && socket_closed && rejected_closed && exception == expected_exception &&
        (expected_exception ? state.find("state=failed") != std::string::npos : exit == 0);
    std::cout << "bounded=" << bounded << " actual_join=" << first_joined
              << " retained_exception=" << exception << " exit=" << exit
              << " peer_read=" << closed_read << " peer_errno=" << closed_error << '\n';
    if (!passed) { std::cerr << "FAIL actual client cohort drain; fixture=" << root << '\n'; return 1; }
    std::filesystem::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << " fixture=" << root << '\n'; return 1;
  }
}
