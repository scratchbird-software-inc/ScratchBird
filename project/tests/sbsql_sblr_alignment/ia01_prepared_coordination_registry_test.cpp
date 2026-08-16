// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_prepared_coordination_registry.hpp"
#include "uuid.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>

using namespace scratchbird::engine::internal_api;
namespace {
std::string Id(scratchbird::core::platform::UuidKind kind, std::uint64_t n) {
  static auto next=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  (void)n;
  const auto v=scratchbird::core::uuid::GenerateEngineIdentityV7(kind,++next);
  assert(v.ok()); return scratchbird::core::uuid::UuidToString(v.value.value);
}
EngineRequestContext Context(const std::filesystem::path& path,bool recovery=false) {
  EngineRequestContext c; c.database_path=path.string();
  c.database_uuid.canonical=Id(scratchbird::core::platform::UuidKind::database,1);
  c.session_uuid.canonical=Id(scratchbird::core::platform::UuidKind::object,2);
  c.security_context_present=true;c.statement_metadata_snapshot_engine_owned=true;
  c.trace_tags.push_back(recovery?"right:SBLR_PREPARED_COORDINATION_ADMIN":"private_prepared_coordination"); return c;
}
}
int main() {
  const auto base=std::filesystem::temp_directory_path()/"sb_prepared_coordination_registry_test";
  std::error_code ec; std::filesystem::remove(base.string()+".sb.sblr_prepared_coordination.v1",ec);
  auto c=Context(base); const auto operation=Id(scratchbird::core::platform::UuidKind::object,3);
  const auto begin=BeginSblrPreparedCoordination(c,operation); assert(begin.ok);
  assert(begin.snapshot.provisional_prepared_generation==begin.snapshot.coordinator_generation);
  assert(begin.snapshot.private_handle!=0);
  const auto acquire=AcquireSblrPreparedCoordination(c,begin.snapshot.coordination_uuid,operation,begin.snapshot.coordinator_generation); assert(acquire.ok);
  assert(acquire.snapshot.coordinator_generation>begin.snapshot.coordinator_generation);
  const auto stale=SealSblrPreparedCoordination(c,begin.snapshot.coordination_uuid,operation,begin.snapshot.coordinator_generation,begin.snapshot.provisional_prepared_uuid,begin.snapshot.provisional_prepared_generation,"sha256:"+std::string(64,'a')); assert(!stale.ok&&stale.diagnostic.code=="SBLR.PARAMETER.STALE");
  const auto seal=SealSblrPreparedCoordination(c,begin.snapshot.coordination_uuid,operation,acquire.snapshot.coordinator_generation,begin.snapshot.provisional_prepared_uuid,begin.snapshot.provisional_prepared_generation,"sha256:"+std::string(64,'b')); assert(seal.ok);
  const auto second=BeginSblrPreparedCoordination(c,Id(scratchbird::core::platform::UuidKind::object,4)); assert(second.ok&&second.snapshot.coordinator_generation>seal.snapshot.coordinator_generation);
  auto admin=c; admin.trace_tags={"right:SBLR_PREPARED_COORDINATION_ADMIN"};
  assert(RecoverSblrPreparedCoordinationRegistry(admin).code=="OK");
  const auto hidden=AcquireSblrPreparedCoordination(c,second.snapshot.coordination_uuid,second.snapshot.operation_uuid,second.snapshot.coordinator_generation); assert(!hidden.ok&&hidden.diagnostic.code=="SECURITY.ACCESS_DENIED");
  const auto third=BeginSblrPreparedCoordination(c,Id(scratchbird::core::platform::UuidKind::object,5)); assert(third.ok&&third.snapshot.coordinator_generation>second.snapshot.coordinator_generation);
  std::filesystem::remove(base.string()+".sb.sblr_prepared_coordination.v1",ec);
}
