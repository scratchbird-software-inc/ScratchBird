// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_prepared_coordination_registry.hpp"
#include "uuid.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::string Id(scratchbird::core::platform::UuidKind kind) {
  static auto next = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  scratchbird::core::platform::TypedUuid typed;
  if (scratchbird::core::uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto value =
        scratchbird::core::uuid::GenerateEngineIdentityV7(kind, ++next);
    Require(value.ok(), "durable coordination UUID generation failed");
    typed = value.value;
  } else {
    const auto value =
        scratchbird::core::uuid::GenerateCompatibilityUnixTimeV7(++next);
    Require(value.ok(), "compatibility coordination UUID generation failed");
    const auto classified =
        scratchbird::core::uuid::MakeTypedUuid(kind, value.value);
    Require(classified.ok(), "coordination UUID classification failed");
    typed = classified.value;
  }
  return scratchbird::core::uuid::UuidToString(typed.value);
}

EngineRequestContext Context(const std::filesystem::path& path) {
  EngineRequestContext context;
  context.database_path = path.string();
  context.database_uuid.canonical =
      Id(scratchbird::core::platform::UuidKind::database);
  context.session_uuid.canonical =
      Id(scratchbird::core::platform::UuidKind::session);
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags.push_back("private_prepared_coordination");
  return context;
}

}  // namespace

int main() {
  try {
    const auto ordinal = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto base = std::filesystem::temp_directory_path() /
                      ("sb_prepared_coordination_registry_test_" +
                       std::to_string(ordinal));
    std::error_code error;
    std::filesystem::remove(
        base.string() + ".sb.sblr_prepared_coordination.v1", error);
    auto context = Context(base);
    const auto operation = Id(scratchbird::core::platform::UuidKind::object);
    const auto begin = BeginSblrPreparedCoordination(context, operation);
    Require(begin.ok &&
                begin.snapshot.kind ==
                    SblrPreparedCoordinationKind::preparation,
            "preparation coordination begin failed");
    Require(begin.snapshot.provisional_prepared_generation ==
                begin.snapshot.coordinator_generation,
            "provisional generation did not match begin generation");
    Require(begin.snapshot.private_handle != 0,
            "preparation coordination handle was not issued");

    const auto acquire = AcquireSblrPreparedCoordination(
        context, begin.snapshot.coordination_uuid, operation,
        begin.snapshot.coordinator_generation);
    Require(acquire.ok &&
                acquire.snapshot.kind ==
                    SblrPreparedCoordinationKind::preparation &&
                acquire.snapshot.coordinator_generation >
                    begin.snapshot.coordinator_generation,
            "preparation coordination acquire failed");
    const auto stale = SealSblrPreparedCoordination(
        context, begin.snapshot.coordination_uuid, operation,
        begin.snapshot.coordinator_generation,
        begin.snapshot.provisional_prepared_uuid,
        begin.snapshot.provisional_prepared_generation,
        "sha256:" + std::string(64, 'a'));
    Require(!stale.ok && stale.diagnostic.code == "SBLR.PARAMETER.STALE",
            "stale preparation coordination generation was admitted");
    const auto seal = SealSblrPreparedCoordination(
        context, begin.snapshot.coordination_uuid, operation,
        acquire.snapshot.coordinator_generation,
        begin.snapshot.provisional_prepared_uuid,
        begin.snapshot.provisional_prepared_generation,
        "sha256:" + std::string(64, 'b'));
    Require(seal.ok &&
                seal.snapshot.kind ==
                    SblrPreparedCoordinationKind::preparation,
            "preparation coordination seal failed");

    const auto second = BeginSblrPreparedCoordination(
        context, Id(scratchbird::core::platform::UuidKind::object));
    Require(second.ok &&
                second.snapshot.kind ==
                    SblrPreparedCoordinationKind::preparation &&
                second.snapshot.coordinator_generation >
                    seal.snapshot.coordinator_generation,
            "second preparation coordination begin failed");
    auto admin = context;
    admin.trace_tags = {"right:SBLR_PREPARED_COORDINATION_ADMIN"};
    Require(RecoverSblrPreparedCoordinationRegistry(admin).code == "OK",
            "coordination registry recovery failed");
    const auto hidden = AcquireSblrPreparedCoordination(
        context, second.snapshot.coordination_uuid,
        second.snapshot.operation_uuid,
        second.snapshot.coordinator_generation);
    Require(!hidden.ok && hidden.diagnostic.code == "SECURITY.ACCESS_DENIED",
            "recovery did not revoke unfinished coordination");
    const auto third = BeginSblrPreparedCoordination(
        context, Id(scratchbird::core::platform::UuidKind::object));
    Require(third.ok &&
                third.snapshot.kind ==
                    SblrPreparedCoordinationKind::preparation &&
                third.snapshot.coordinator_generation >
                    second.snapshot.coordinator_generation,
            "post-recovery coordination generation did not advance");

    std::filesystem::remove(
        base.string() + ".sb.sblr_prepared_coordination.v1", error);
    return 0;
  } catch (const std::exception& exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
