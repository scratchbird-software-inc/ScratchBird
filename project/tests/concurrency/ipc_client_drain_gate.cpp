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
#include <exception>
namespace srv = scratchbird::server;
namespace core = scratchbird::core;
using namespace std::chrono_literals;
std::binary_semaphore ready{0}, consumed{0}, done{0};
std::binary_semaphore failure_claimed{0}, release_failure_capture{0}, joining_selected_writer{0};
std::binary_semaphore release_retained_client{0};
std::atomic<int> first_fd{-1};
std::atomic<int> listening_fd{-1};
std::atomic<int> attempted_bind_fd{-1};
std::atomic<unsigned> created_threads{0};
std::atomic<unsigned> failure_record_lock_faults{0};
std::atomic<bool> observe_read{true}, first_joined{false};
bool launch_failure = false;
bool allocation_failure = false;
bool cohort_allocation_failure = false;
bool endpoint_allocation_failure = false;
bool bind_failure = false;
bool delayed_failure_publication = false;
bool native_join_failure = false;
std::atomic<unsigned> native_join_failures{0};
int competing_fd = -1;
thread_local bool fail_allocation = false;
thread_local unsigned accepted_count = 0;
thread_local bool client_launch = false;
thread_local bool fail_record_mutex = false;
thread_local bool delay_failure_capture = false;
pthread_t first_thread{};
extern "C" int __real_listen(int, int);
extern "C" int __real_bind(int, const sockaddr*, socklen_t);
extern "C" int __wrap_bind(int fd, const sockaddr* address, socklen_t length) {
  attempted_bind_fd = fd;
  if (bind_failure) {
    // Deterministic competing ownership after the endpoint's preflight lookup.
    // The caller's bind itself fails in the real kernel, not via a fake result.
    competing_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (competing_fd < 0 || __real_bind(competing_fd, address, length) != 0 ||
        __real_listen(competing_fd, 1) != 0) std::abort();
  }
  return __real_bind(fd, address, length);
}
extern "C" int __wrap_listen(int fd, int backlog) {
  const int rc = __real_listen(fd, backlog);
  if (!rc) {
    listening_fd = fd;
    if (endpoint_allocation_failure) fail_allocation = true;
  }
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
  if (!rc) ++created_threads;
  if (client && accepted_count == 1 && !rc) first_thread = *id;
  return rc;
}
extern "C" int __real_pthread_join(pthread_t, void**);
extern "C" int __wrap_pthread_join(pthread_t id, void** result) {
  // Fault window is the reporting boundary before cohort join, not later agent
  // cleanup. A failure sink with no native mutex reaches join without a fault.
  fail_record_mutex = false;
  const bool retained_client = native_join_failure && ::pthread_equal(id, first_thread);
  if (retained_client && native_join_failures.load() < 2) {
    ++native_join_failures;
    return EAGAIN;
  }
  if (delayed_failure_publication && accepted_count && ::pthread_equal(id, first_thread))
    joining_selected_writer.release();
  const int rc = __real_pthread_join(id, result);
  if ((accepted_count || retained_client) && !rc && ::pthread_equal(id, first_thread)) first_joined = true;
  return rc;
}
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (std::exchange(fail_record_mutex, false)) {
    ++failure_record_lock_faults;
    constexpr char message[] = "injected pre-join failure-record mutex error\n";
    (void)::write(STDERR_FILENO, message, sizeof(message)-1);
    return EAGAIN;
  }
  return __real_pthread_mutex_lock(mutex);
}
extern "C" ssize_t __real_recv(int, void*, size_t, int);
extern "C" ssize_t __wrap_recv(int fd, void* data, size_t size, int flags) {
  const auto rc = __real_recv(fd, data, size, flags);
  if (fd == first_fd && rc > 0 && observe_read.exchange(false)) {
    fail_allocation = allocation_failure;
    consumed.release();
    if (native_join_failure) release_retained_client.acquire();
  }
  return rc;
}
extern "C" void* RealNew(std::size_t) asm("__real__Znwm");
extern "C" void* WrapNew(std::size_t size) asm("__wrap__Znwm");
extern "C" void* WrapNew(std::size_t size) {
  if (std::exchange(fail_allocation, false)) {
    delay_failure_capture = delayed_failure_publication;
    throw std::bad_alloc();
  }
  return RealNew(size);
}
extern "C" std::exception_ptr RealCurrentException() noexcept asm("__real__ZSt17current_exceptionv");
extern "C" std::exception_ptr WrapCurrentException() noexcept asm("__wrap__ZSt17current_exceptionv");
extern "C" std::exception_ptr WrapCurrentException() noexcept {
  if (std::exchange(delay_failure_capture, false)) {
    // The actual endpoint has selected its one writer, but exception_ptr has
    // not yet been assigned. Keep that real catch alive until the parent has
    // reported a second fault and entered the selected client's native join.
    failure_claimed.release();
    release_failure_capture.acquire();
  }
  return RealCurrentException();
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
  delayed_failure_publication = mode == "delayed-failure-publication";
  allocation_failure = mode == "allocation-failure" || delayed_failure_publication;
  cohort_allocation_failure = mode == "cohort-allocation-failure";
  endpoint_allocation_failure = mode == "endpoint-allocation-failure";
  bind_failure = mode == "bind-failure";
  native_join_failure = mode == "native-join-failure";
  const bool ready_failure = mode == "ready-failure" || mode == "ready-stop-failure";
  const bool record_lock_failure = mode == "failure-record-lock-failure";
  const bool callback_failure = mode == "callback-failure" || mode == "ready-stop-failure" || record_lock_failure || delayed_failure_publication;
  const bool complete_frame = mode == "complete-frame";
  const bool expected_exception = launch_failure || allocation_failure || callback_failure || cohort_allocation_failure || ready_failure || endpoint_allocation_failure;
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
      fail_record_mutex = record_lock_failure;
      if (callback_failure) throw std::runtime_error("stop callback fault");
    };
    bool exception = false, bind_diagnostic = false; int exit = -1;
    const auto threads_before_endpoint = created_threads.load();
    srv::ServerIpcEndpointOwner endpoint_owner(engine);
    std::thread endpoint([&] {
      try {
        // Deliberately do not keep argument storage alive after endpoint exit.
        const auto endpoint_config = config;
        const auto endpoint_artifacts = artifacts;
        const auto result = srv::RunParserServerIpcEndpoint(endpoint_config, endpoint_artifacts, endpoint_owner, callbacks);
        exit = result.exit_code;
        for (const auto& diagnostic : result.diagnostics)
          if (diagnostic.code == "PARSER_SERVER_IPC.ENDPOINT_BIND_FAILED") bind_diagnostic = true;
      }
      catch (const std::system_error& error) { exception = (launch_failure || native_join_failure) && error.code().value() == EAGAIN; }
      catch (const std::bad_alloc&) { exception = allocation_failure || cohort_allocation_failure || endpoint_allocation_failure; }
      catch (const std::runtime_error& error) {
        exception = !delayed_failure_publication &&
            (ready_failure ? std::string_view(error.what()) == "ready callback fault"
                           : callback_failure && std::string_view(error.what()) == "stop callback fault");
      }
      catch (...) { exception = false; }
      done.release();
    });
    if (endpoint_allocation_failure || bind_failure) {
      if (!done.try_acquire_for(5s)) std::abort();
      endpoint.join();
      const int fd = attempted_bind_fd.load();
      errno = 0;
      const bool closed = fd >= 0 && ::fcntl(fd, F_GETFD) == -1 && errno == EBADF;
      const bool removed = !std::filesystem::exists(config.sbps_endpoint);
      const bool no_cohort = created_threads == threads_before_endpoint + 1;
      const bool not_ready = !ready.try_acquire();
      if (bind_failure) {
        const bool foreign_live = competing_fd >= 0 && ::fcntl(competing_fd, F_GETFD) >= 0 && !removed;
        Require(foreign_live, "failed bind preserves competing socket and path");
        const int peer = Connect(config.sbps_endpoint);
        ::close(peer);
        ::close(competing_fd);
        competing_fd = -1;
        std::cout << "listener_closed=" << closed << " foreign_endpoint_live=" << foreign_live
                  << " bind_diagnostic=" << bind_diagnostic << " no_cohort=" << no_cohort << '\n';
        Require(!exception && exit == 2 && bind_diagnostic && closed && no_cohort && not_ready &&
                    !stopping_called && first_fd < 0, "actual bind failure cleanup");
        std::filesystem::remove_all(root);
        return 0;
      }
      std::cout << "listener_closed=" << closed << " endpoint_removed=" << removed
                << " no_cohort=" << no_cohort << " not_ready=" << not_ready
                << " retained_allocation_failure=" << exception << '\n';
      Require(exception && closed && removed && no_cohort && not_ready && !stopping_called && first_fd < 0,
              "pre-cohort endpoint allocation cleanup");
      std::filesystem::remove_all(root);
      return 0;
    }
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
    bool delayed_join_observed = false;
    if (delayed_failure_publication) {
      if (!failure_claimed.try_acquire_for(5s)) std::abort();
      srv::RequestParserServerStop();
      if (!joining_selected_writer.try_acquire_for(5s)) std::abort();
      delayed_join_observed = true;
      release_failure_capture.release();
    }
    else if (launch_failure || cohort_allocation_failure) second = Connect(config.sbps_endpoint);
    else if (!allocation_failure) srv::RequestParserServerStop();
    const bool bounded = done.try_acquire_for(5s);
    if (native_join_failure) {
      if (!bounded) std::abort();
      endpoint.join();
      Require(exception && !first_joined && native_join_failures == 1 &&
              endpoint_owner.first_failure && !endpoint_owner.endpoint_active,
              "embedded return must retain client after failed native join");
      const auto pending = srv::DrainServerIpcEndpoint(endpoint_owner);
      Require(!pending.complete && !pending.clients_complete && !pending.agents_complete &&
              !pending.listeners_complete && !pending.sessions_complete &&
              endpoint_owner.agents.Snapshot().started && native_join_failures == 2,
              "pending client join must fence dependent cleanup");
      release_retained_client.release();
      const auto drained = srv::DrainServerIpcEndpoint(endpoint_owner);
      Require(drained.complete && drained.clients_complete && first_joined &&
              !endpoint_owner.agents.Snapshot().started && endpoint_owner.first_failure,
              "retry must actually join retained client before service cleanup");
      ::close(peer);
      peer = -1;
      Require(!std::filesystem::exists(config.sbps_endpoint), "listening endpoint withdrawn");
      std::ifstream lifecycle(config.lifecycle_state_file);
      const std::string state{std::istreambuf_iterator<char>(lifecycle), {}};
      Require(state.find("state=stopped") == std::string::npos,
              "resource retry must not invent a clean lifecycle receipt");
      std::filesystem::remove_all(root);
      std::cout << "retained_native_client_join_retry=PASS\n";
      return 0;
    }
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
        (!record_lock_failure || failure_record_lock_faults == 0) &&
        (!delayed_failure_publication || delayed_join_observed) &&
        (expected_exception ? state.find("state=failed") != std::string::npos : exit == 0);
    std::cout << "bounded=" << bounded << " actual_join=" << first_joined
              << " retained_exception=" << exception << " exit=" << exit
              << " delayed_writer_joined=" << delayed_join_observed
              << " peer_read=" << closed_read << " peer_errno=" << closed_error << '\n';
    if (!passed) { std::cerr << "FAIL actual client cohort drain; fixture=" << root << '\n'; return 1; }
    std::filesystem::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << " fixture=" << root << '\n'; return 1;
  }
}
