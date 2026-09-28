// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sbps.hpp"
#include "parser_package_registry.hpp"
#include "ipc_server.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {
using Id=std::array<std::uint8_t,16>;
unsigned failures=0;
void Check(bool value,const char* message) {
  if(!value) {++failures;std::cerr<<message<<'\n';}
}
bool Transfer(int fd,void* bytes,std::size_t count,bool write) {
  auto* data=static_cast<std::uint8_t*>(bytes);
  while(count) {
    const auto n=write ? ::write(fd,data,count) : ::read(fd,data,count);
    if(n<0 && errno==EINTR) continue;
    if(n<=0) return false;
    count-=std::size_t(n);data+=n;
  }
  return true;
}
bool Valid(const Id& id) {return (id[6]&0xf0)==0x70 && (id[8]&0xc0)==0x80;}
std::vector<Id> Generate(std::size_t count) {
  std::vector<Id> ids;ids.reserve(count);
  for(std::size_t i=0;i<count;++i) ids.push_back(scratchbird::server::sbps::MakeUuidV7Bytes());
  return ids;
}

void HelloCodecChecks() {
  namespace sbps = scratchbird::server::sbps;
  const auto original = sbps::EncodeHelloRequestForTest();
  const auto decoded = sbps::DecodeHelloRequest(original);
  Check(decoded.has_value(), "canonical HELLO did not decode");
  if (!decoded) return;
  // The final fields are launch UUID, listener UUID, generation and bitmap.
  const std::array<std::size_t, 6> offsets{
      0, 16, 32, 48, original.size() - 72, original.size() - 56};
  for (std::size_t field = 0; field < offsets.size(); ++field) {
    for (std::size_t octet = 0; octet < 16; ++octet) {
      for (unsigned value = 0; value < 256; ++value) {
        auto payload = original;
        payload[offsets[field] + octet] = static_cast<std::uint8_t>(value);
        const bool valid = (octet != 6 || (value & 0xf0) == 0x70) &&
                           (octet != 8 || (value & 0xc0) == 0x80);
        const auto result = sbps::DecodeHelloRequest(payload);
        Check(result.has_value() == valid, "HELLO system identity byte validation mismatch");
        if (!result) continue;
        const std::array<const Id*, 6> fields{
            &result->parser_instance_uuid, &result->parser_package_uuid,
            &result->parser_family_uuid, &result->dialect_profile_uuid,
            &result->launch_uuid, &result->listener_uuid};
        for (std::size_t selected = 0; selected < offsets.size(); ++selected) {
          Check(std::equal(fields[selected]->begin(), fields[selected]->end(),
                           payload.begin() + offsets[selected]),
                "HELLO decoding changed a native identity octet");
        }
      }
    }
    auto nil = original;
    std::fill_n(nil.begin() + offsets[field], 16, 0);
    Check(!sbps::DecodeHelloRequest(nil), "HELLO accepted a nil system identity");
  }
  for (std::size_t size = 0; size < original.size(); ++size) {
    Check(!sbps::DecodeHelloRequest({original.begin(), original.begin() + size}),
          "HELLO accepted a truncated payload");
  }
  for (const auto count : {1, 16, 36}) {
    auto extra = original;
    extra.insert(extra.end(), count, 0);
    Check(!sbps::DecodeHelloRequest(extra), "HELLO accepted trailing payload bytes");
  }
}

void RetainedOwnerChecks() {
  namespace server = scratchbird::server;
  namespace uuid = scratchbird::core::uuid;
  server::ServerBootstrapConfig config;
  const auto first = server::LoadParserPackageRegistry(config);
  const auto second = server::LoadParserPackageRegistry(config);
  const auto copy = first;
  Check(first.diagnostics.empty() && second.diagnostics.empty() &&
            uuid::IsEngineIdentityUuid(first.snapshot_uuid) &&
            uuid::IsEngineIdentityUuid(second.snapshot_uuid),
        "registry load did not retain a native snapshot owner");
  Check(first.snapshot_uuid != second.snapshot_uuid &&
            first.snapshot_uuid == copy.snapshot_uuid,
        "registry copies/reloads did not preserve/distinguish their actual owner");

  char path[] = "/tmp/sb_hello_registry_XXXXXX";
  const auto fd = ::mkstemp(path);
  Check(fd >= 0, "failed registry fixture creation");
  if (fd >= 0) {
    ::close(fd);
    config.parser_registry_path = path;
    const auto failed = server::LoadParserPackageRegistry(config);
    Check(!failed.diagnostics.empty() && failed.snapshot_uuid.is_nil(),
          "failed registry load published a successful snapshot identity");
    ::unlink(path);
  }
  server::HostedEngineState no_engine;
  for (unsigned kind = 0; kind != 3; ++kind) {
    server::ServerLifecycleArtifacts invalid;
    if (kind) {
      invalid.server_uuid.bytes = server::sbps::MakeUuidV7Bytes();
      if (kind == 1) invalid.server_uuid.bytes[6] = 0x40;
      else invalid.server_uuid.bytes[8] = 0xc0;
    }
    const auto denied = server::RunParserServerIpcEndpoint(config, invalid, no_engine);
    Check(!denied.ok() && denied.exit_code != 0 &&
              denied.diagnostics.size() == 1 &&
              denied.diagnostics.front().code == "PARSER_SERVER_IPC.ENDPOINT_CREATE_FAILED",
          "endpoint accepted a missing or malformed retained native server owner");
  }
}
}
int main() {
  namespace sbps=scratchbird::server::sbps;
  HelloCodecChecks();
  RetainedOwnerChecks();
  constexpr unsigned count=4096,threads=8;
  const auto initial=sbps::MakeUuidV7Bytes();Check(Valid(initial),"initial server identity not binary UUIDv7");
  int descriptors[2];
  if(::pipe(descriptors)!=0) {std::cerr<<"pipe failed\n";return 2;}
  const auto child=::fork();
  if(child<0) {::close(descriptors[0]);::close(descriptors[1]);std::cerr<<"fork failed\n";return 2;}
  if(child==0) {
    ::close(descriptors[0]);
    auto ids=Generate(count);
    const bool written=Transfer(descriptors[1],ids.data(),ids.size()*sizeof(Id),true);
    ::close(descriptors[1]);::_exit(written?0:2);
  }
  ::close(descriptors[1]);
  const auto parent_ids=Generate(count);
  std::vector<Id> child_ids(count);
  const bool received=Transfer(descriptors[0],child_ids.data(),child_ids.size()*sizeof(Id),false);
  ::close(descriptors[0]);int status=0;
  const bool waited=::waitpid(child,&status,0)==child;
  Check(received && waited && WIFEXITED(status) && WEXITSTATUS(status)==0,"child generation/read failed");
  Check(std::is_sorted(parent_ids.begin(),parent_ids.end()),
        "server runtime issuer did not preserve parent allocation order");
  Check(std::is_sorted(child_ids.begin(),child_ids.end()),
        "server runtime issuer did not preserve child allocation order");
  std::set<Id> parent_set(parent_ids.begin(),parent_ids.end());
  unsigned duplicates=0,cloned_suffixes=0;
  for(unsigned i=0;i<count;++i) {
    duplicates+=parent_set.contains(child_ids[i]);
    cloned_suffixes+=std::equal(parent_ids[i].begin()+6,parent_ids[i].end(),child_ids[i].begin()+6);
    Check(Valid(parent_ids[i]) && Valid(child_ids[i]),"fork emitted non-v7 identity");
  }
  Check(duplicates==0,"forked server reused an actual binary UUID");
  // This additional state-cloning check isolates fork safety from scheduler
  // timing. A shifted clock can mask actual collisions in one particular run.
  Check(cloned_suffixes==0,"fork cloned the server identity generator stream");
  std::barrier ready(threads);
  std::array<std::vector<Id>,threads> batches;
  std::vector<std::thread> workers;
  for(unsigned i=0;i<threads;++i) workers.emplace_back([&,i] {
    ready.arrive_and_wait();batches[i]=Generate(count);
  });
  for(auto& worker:workers) worker.join();
  std::set<Id> all;
  for(const auto& batch:batches) {
    Check(std::is_sorted(batch.begin(),batch.end()),
          "server runtime issuer did not preserve thread-local allocation order");
    for(const auto& id:batch) {
      Check(Valid(id),"concurrent server emission not binary UUIDv7");all.insert(id);
    }
  }
  const auto concurrent_duplicates=threads*count-all.size();
  Check(concurrent_duplicates==0,"concurrent server reused binary UUID identities");
  std::cout<<"server_uuid_emission fork_duplicates="<<duplicates<<" cloned_suffixes="<<cloned_suffixes
           <<" concurrent_duplicates="<<concurrent_duplicates<<" samples="<<(threads+2)*count
           <<" failures="<<failures<<'\n';
  return failures?1:0;
}
