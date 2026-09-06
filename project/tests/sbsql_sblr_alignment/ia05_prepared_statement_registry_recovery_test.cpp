// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_prepared_statement_registry.hpp"
#include "sblr_prepared_coordination_registry.hpp"
#include "database_lifecycle.hpp"
#include "scratchbird/engine/engine.h"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using scratchbird::engine::internal_api::EngineRequestContext;
using scratchbird::engine::internal_api::AcquireSblrPreparedCoordination;
using scratchbird::engine::internal_api::BeginSblrPreparedCoordination;
using scratchbird::engine::internal_api::
    BeginSblrPreparedExecutionCoordination;
using scratchbird::engine::internal_api::FreeSblrPreparedStatementV1;
using scratchbird::engine::internal_api::LoadSblrPreparedStatementRegistryV1;
using scratchbird::engine::internal_api::PublishSblrPreparedStatementV1;
using scratchbird::engine::internal_api::RevokeSblrPreparedStatementSessionV1;
using scratchbird::engine::internal_api::
    ResolveActiveSblrPreparedStatementCapabilityV1;
using scratchbird::engine::internal_api::SealSblrPreparedCoordination;
using scratchbird::engine::internal_api::SblrPreparedStatementRegistryHashV1;
using scratchbird::engine::internal_api::SblrPreparedStatementRegistryRecordV1;
using scratchbird::engine::internal_api::SblrPreparedStatementRegistryStateV1;
using scratchbird::engine::internal_api::SblrPreparedStatementRegistryUuidV1;

namespace database = scratchbird::storage::database;

namespace {

void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

SblrPreparedStatementRegistryUuidV1 NewUuid(
    scratchbird::core::platform::UuidKind kind) {
  static std::uint64_t next = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto timestamp = ++next;
  scratchbird::core::platform::TypedUuid typed;
  if (scratchbird::core::uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto generated =
        scratchbird::core::uuid::GenerateEngineIdentityV7(kind, timestamp);
    Require(generated.ok(), "durable UUID generation failed");
    typed = generated.value;
  } else {
    const auto generated =
        scratchbird::core::uuid::GenerateCompatibilityUnixTimeV7(timestamp);
    Require(generated.ok(), "compatibility UUID generation failed");
    const auto classified =
        scratchbird::core::uuid::MakeTypedUuid(kind, generated.value);
    Require(classified.ok(), "typed UUID classification failed");
    typed = classified.value;
  }
  SblrPreparedStatementRegistryUuidV1 bytes{};
  std::copy(typed.value.bytes.begin(), typed.value.bytes.end(), bytes.begin());
  return bytes;
}

std::string UuidText(const SblrPreparedStatementRegistryUuidV1& bytes) {
  scratchbird::core::platform::Uuid uuid{};
  std::copy(bytes.begin(), bytes.end(), uuid.bytes.begin());
  return scratchbird::core::uuid::UuidToString(uuid);
}

sb_engine_uuid_t PublicUuid(
    const SblrPreparedStatementRegistryUuidV1& bytes) {
  sb_engine_uuid_t value{};
  std::memcpy(value.bytes, bytes.data(), bytes.size());
  return value;
}

SblrPreparedStatementRegistryHashV1 Hash(std::uint8_t seed) {
  SblrPreparedStatementRegistryHashV1 value{};
  for (std::size_t index = 0; index != value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(seed + index);
  }
  return value;
}

EngineRequestContext Context(const std::filesystem::path& database_path,
                             const SblrPreparedStatementRegistryUuidV1& database,
                             const SblrPreparedStatementRegistryUuidV1& session,
                             const SblrPreparedStatementRegistryUuidV1& principal) {
  EngineRequestContext context;
  context.database_path = database_path.string();
  context.database_uuid.canonical = UuidText(database);
  context.principal_uuid.canonical = UuidText(principal);
  context.session_uuid.canonical = UuidText(session);
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags.push_back("private_prepared_statement_registry");
  return context;
}

SblrPreparedStatementRegistryRecordV1 Record(std::string name,
                                             std::uint8_t seed) {
  SblrPreparedStatementRegistryRecordV1 record;
  record.canonical_name = std::move(name);
  record.body_operation_id = "query.evaluate_projection";
  record.body_operation_family = "sblr.query.relational.v3";
  record.body_result_shape = "query_projection_result";
  record.source_free_parameterless_query_template = true;
  record.statement_uuid =
      NewUuid(scratchbird::core::platform::UuidKind::object);
  record.statement_name_uuid =
      NewUuid(scratchbird::core::platform::UuidKind::object);
  record.preparing_receipt_uuid =
      NewUuid(scratchbird::core::platform::UuidKind::object);
  record.prepared_generation = 1;
  record.descriptor_sha256 = Hash(seed);
  record.canonical_descriptor_bytes = {seed, 1, 2, 3};
  record.canonical_container_bytes = {seed, 4, 5, 6};
  record.canonical_execution_envelope_bytes = {seed, 7, 8, 9};
  record.canonical_prepare_result_bytes.assign(160, seed);
  record.canonical_prepare_result_bytes[0] = 'S';
  record.canonical_prepare_result_bytes[1] = 'B';
  record.canonical_prepare_result_bytes[2] = 'P';
  record.canonical_prepare_result_bytes[3] = 'R';
  return record;
}

SblrPreparedStatementRegistryRecordV1 ParameterizedRecord(
    std::string name, std::uint8_t seed) {
  auto record = Record(std::move(name), seed);
  record.source_free_parameterless_query_template = false;
  record.source_free_parameterized_query_template = true;
  record.parameter_set_uuid = UuidText(
      NewUuid(scratchbird::core::platform::UuidKind::object));
  record.parameter_prepared_statement_uuid = UuidText(
      NewUuid(scratchbird::core::platform::UuidKind::object));
  record.parameter_set_generation = 1;
  record.parameter_set_snapshot_uuid = UuidText(
      NewUuid(scratchbird::core::platform::UuidKind::object));
  record.parameter_set_snapshot_generation = 1;
  record.ordered_slot_table_sha256 = "sha256:" + std::string(64, 'a');
  return record;
}

std::string PathFor(const EngineRequestContext& context) {
  return context.database_path +
         ".sb.sblr_prepared_statement_registry.v1." +
         context.session_uuid.canonical;
}

void Remove(const EngineRequestContext& context) {
  std::error_code error;
  std::filesystem::remove(PathFor(context), error);
}

void RemoveCoordinationRegistry(const EngineRequestContext& context) {
  std::error_code error;
  std::filesystem::remove(
      context.database_path + ".sb.sblr_prepared_coordination.v1", error);
}

void RequirePublicSessionLifecycle(
    const std::filesystem::path& directory,
    const SblrPreparedStatementRegistryUuidV1& database_uuid) {
  const auto database_path = directory / "prepared_recovery.sbdb";
  const auto filespace_uuid =
      NewUuid(scratchbird::core::platform::UuidKind::filespace);
  database::DatabaseCreateConfig create;
  create.path = database_path.string();
  create.database_uuid = scratchbird::core::uuid::MakeTypedUuid(
                             scratchbird::core::platform::UuidKind::database,
                             scratchbird::core::uuid::ParseUuid(
                                 UuidText(database_uuid))
                                 .value)
                             .value;
  create.filespace_uuid = scratchbird::core::uuid::MakeTypedUuid(
                              scratchbird::core::platform::UuidKind::filespace,
                              scratchbird::core::uuid::ParseUuid(
                                  UuidText(filespace_uuid))
                                  .value)
                              .value;
  create.creation_unix_epoch_millis = 1'950'000'000'000ULL;
  create.page_size = 16384;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  Require(database::CreateDatabaseFile(create).ok(),
          "public lifecycle database creation failed");

  const auto session_uuid =
      NewUuid(scratchbird::core::platform::UuidKind::session);
  const auto principal_uuid =
      NewUuid(scratchbird::core::platform::UuidKind::principal);
  const auto context =
      Context(database_path, database_uuid, session_uuid, principal_uuid);
  Require(PublishSblrPreparedStatementV1(
              context, Record("recovered_public_statement", 73))
              .ok,
          "public lifecycle PREPARE fixture publication failed");

  const auto path = database_path.string();
  sb_engine_open_params_v1_t open{};
  open.struct_size = sizeof(open);
  open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  open.database_path_utf8 = path.data();
  open.database_path_size = path.size();
  open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
  sb_engine_handle_t engine = nullptr;
  Require(sb_engine_open(&open, &engine, nullptr) == SB_ENGINE_STATUS_OK &&
              engine != nullptr,
          "public lifecycle engine open failed");

  sb_engine_session_params_v1_t begin{};
  begin.struct_size = sizeof(begin);
  begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  begin.effective_user_uuid = PublicUuid(principal_uuid);
  begin.session_uuid = PublicUuid(session_uuid);
  begin.default_language_utf8 = "en";
  begin.default_language_size = 2;
  begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
  sb_engine_session_t session = nullptr;
  Require(sb_engine_session_begin(engine, &begin, &session, nullptr) ==
                  SB_ENGINE_STATUS_OK &&
              session != nullptr,
          "public ABI did not recover the unterminated prepared session");

  sb_engine_session_end_params_v1_t end{};
  end.struct_size = sizeof(end);
  end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  end.rollback_active_transactions = 1;
  end.cancel_open_results = 1;
  Require(sb_engine_session_end(session, &end, nullptr) ==
              SB_ENGINE_STATUS_OK,
          "public ABI session termination failed");
  const auto terminated = LoadSblrPreparedStatementRegistryV1(context);
  Require(terminated.ok && terminated.found &&
              terminated.snapshot.session_revoked,
          "public ABI session termination was not durably recorded");

  session = nullptr;
  Require(sb_engine_session_begin(engine, &begin, &session, nullptr) ==
                  SB_ENGINE_STATUS_SECURITY_DENIED &&
              session == nullptr,
          "public ABI resurrected an explicitly terminated prepared session");
  Require(sb_engine_close(engine, nullptr) == SB_ENGINE_STATUS_OK,
          "public lifecycle engine close failed");
}

}  // namespace

int main() {
  try {
    const auto ordinal = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto base = std::filesystem::temp_directory_path() /
                      ("sb_prepared_statement_registry_" +
                       std::to_string(ordinal));
    const auto database =
        NewUuid(scratchbird::core::platform::UuidKind::database);
    const auto session =
        NewUuid(scratchbird::core::platform::UuidKind::session);
    const auto principal =
        NewUuid(scratchbird::core::platform::UuidKind::principal);
    const auto context = Context(base, database, session, principal);
    Remove(context);
    RemoveCoordinationRegistry(context);

    const auto prepared = Record("prepared_one", 11);
    const auto published = PublishSblrPreparedStatementV1(context, prepared);
    Require(published.ok && published.found && !published.exact_replay,
            "first durable PREPARE publication failed");
    Require(published.record.canonical_prepare_result_bytes ==
                prepared.canonical_prepare_result_bytes,
            "published PREPARE result was not byte exact");

    const auto reopened = LoadSblrPreparedStatementRegistryV1(context);
    Require(reopened.ok && reopened.found &&
                reopened.snapshot.records.size() == 1 &&
                reopened.snapshot.records.front().canonical_descriptor_bytes ==
                    prepared.canonical_descriptor_bytes,
            "process-style registry reopen did not reconstruct PREPARE");

    const auto replay = PublishSblrPreparedStatementV1(context, prepared);
    Require(replay.ok && replay.exact_replay &&
                replay.record.statement_uuid == prepared.statement_uuid,
            "exact PREPARE replay did not reuse durable identity");
    auto collision = prepared;
    collision.descriptor_sha256 = Hash(99);
    const auto collision_result =
        PublishSblrPreparedStatementV1(context, collision);
    Require(!collision_result.ok &&
                collision_result.diagnostic.code == "MGA.TRANSACTION.STALE",
            "changed PREPARE descriptor replaced durable identity");

    const auto other_session =
        NewUuid(scratchbird::core::platform::UuidKind::session);
    const auto foreign_context =
        Context(base, database, other_session, principal);
    const auto hidden = LoadSblrPreparedStatementRegistryV1(foreign_context);
    Require(hidden.ok && !hidden.found && hidden.snapshot.records.empty(),
            "one session observed another session's prepared registry");
    const auto other_principal =
        NewUuid(scratchbird::core::platform::UuidKind::principal);
    const auto impersonated_context =
        Context(base, database, session, other_principal);
    const auto impersonated =
        LoadSblrPreparedStatementRegistryV1(impersonated_context);
    Require(!impersonated.ok &&
                impersonated.diagnostic.code == "SECURITY.ACCESS_DENIED",
            "another principal recovered a prepared session by UUID");

    const auto parameterized = ParameterizedRecord("prepared_two", 29);
    Require(PublishSblrPreparedStatementV1(context, parameterized).ok,
            "parameterized PREPARE publication failed");
    auto capability_context = context;
    capability_context.trace_tags.push_back(
        "private_prepared_statement_capability_check");
    const auto active_capability =
        ResolveActiveSblrPreparedStatementCapabilityV1(
            capability_context,
            parameterized.parameter_prepared_statement_uuid,
            parameterized.prepared_generation);
    Require(active_capability.ok && active_capability.found &&
                active_capability.record.canonical_name == "prepared_two",
            "active private preparation capability was not resolved");

    auto free_result = std::vector<std::uint8_t>(128, 17);
    free_result[0] = 'S';
    free_result[1] = 'B';
    free_result[2] = 'F';
    free_result[3] = 'R';
    const auto free_hash = Hash(41);
    const auto freed = FreeSblrPreparedStatementV1(
        context, prepared.canonical_name, prepared.statement_uuid,
        prepared.prepared_generation, prepared.descriptor_sha256, free_hash,
        free_result);
    Require(freed.ok && !freed.exact_replay &&
                freed.record.state ==
                    SblrPreparedStatementRegistryStateV1::freed &&
                freed.record.canonical_free_result_bytes == free_result,
            "durable FREE publication failed");
    const auto free_replay = FreeSblrPreparedStatementV1(
        context, prepared.canonical_name, prepared.statement_uuid,
        prepared.prepared_generation, prepared.descriptor_sha256, free_hash,
        free_result);
    Require(free_replay.ok && free_replay.exact_replay &&
                free_replay.record.canonical_free_result_bytes == free_result,
            "exact FREE replay was not byte identical");
    auto conflicting_free = free_result;
    conflicting_free.back() ^= 0x01;
    const auto refused_free = FreeSblrPreparedStatementV1(
        context, prepared.canonical_name, prepared.statement_uuid,
        prepared.prepared_generation, prepared.descriptor_sha256, free_hash,
        conflicting_free);
    Require(!refused_free.ok &&
                refused_free.diagnostic.code == "MGA.TRANSACTION.STALE",
            "conflicting FREE replay was admitted");

    auto parameterized_free = std::vector<std::uint8_t>(128, 31);
    parameterized_free[0] = 'S';
    parameterized_free[1] = 'B';
    parameterized_free[2] = 'F';
    parameterized_free[3] = 'R';
    Require(FreeSblrPreparedStatementV1(
                context, parameterized.canonical_name,
                parameterized.statement_uuid,
                parameterized.prepared_generation,
                parameterized.descriptor_sha256, Hash(47), parameterized_free)
                .ok,
            "parameterized FREE publication failed");
    const auto revoked_capability =
        ResolveActiveSblrPreparedStatementCapabilityV1(
            capability_context,
            parameterized.parameter_prepared_statement_uuid,
            parameterized.prepared_generation);
    Require(!revoked_capability.ok &&
                revoked_capability.diagnostic.code ==
                    "SECURITY.ACCESS_DENIED",
            "durably freed private preparation capability remained usable");

    auto coordination_context = context;
    coordination_context.trace_tags.push_back("private_prepared_coordination");
    const auto prepare_operation = UuidText(
        NewUuid(scratchbird::core::platform::UuidKind::object));
    const auto coordinated_begin =
        BeginSblrPreparedCoordination(coordination_context, prepare_operation);
    Require(coordinated_begin.ok,
            "private preparation coordination begin failed");
    auto coordinated = ParameterizedRecord("prepared_three", 37);
    coordinated.parameter_prepared_statement_uuid =
        coordinated_begin.snapshot.provisional_prepared_uuid;
    coordinated.prepared_generation =
        coordinated_begin.snapshot.provisional_prepared_generation;
    Require(PublishSblrPreparedStatementV1(context, coordinated).ok,
            "coordinated durable PREPARE publication failed");
    const auto coordinated_acquire = AcquireSblrPreparedCoordination(
        coordination_context,
        coordinated_begin.snapshot.coordination_uuid, prepare_operation,
        coordinated_begin.snapshot.coordinator_generation);
    Require(coordinated_acquire.ok,
            "private preparation coordination acquire failed");
    const auto coordinated_seal = SealSblrPreparedCoordination(
        coordination_context,
        coordinated_begin.snapshot.coordination_uuid, prepare_operation,
        coordinated_acquire.snapshot.coordinator_generation,
        coordinated_begin.snapshot.provisional_prepared_uuid,
        coordinated_begin.snapshot.provisional_prepared_generation,
        "sha256:" + std::string(64, 'b'));
    Require(coordinated_seal.ok,
            "private preparation coordination seal failed");
    const auto first_execution = BeginSblrPreparedExecutionCoordination(
        coordination_context,
        UuidText(NewUuid(scratchbird::core::platform::UuidKind::object)),
        coordinated.parameter_prepared_statement_uuid);
    Require(first_execution.ok,
            "active durable capability did not authorize execution");

    auto coordinated_free = std::vector<std::uint8_t>(128, 43);
    coordinated_free[0] = 'S';
    coordinated_free[1] = 'B';
    coordinated_free[2] = 'F';
    coordinated_free[3] = 'R';
    Require(FreeSblrPreparedStatementV1(
                context, coordinated.canonical_name,
                coordinated.statement_uuid, coordinated.prepared_generation,
                coordinated.descriptor_sha256, Hash(53), coordinated_free)
                .ok,
            "coordinated durable FREE publication failed");
    const auto post_free_execution = BeginSblrPreparedExecutionCoordination(
        coordination_context,
        UuidText(NewUuid(scratchbird::core::platform::UuidKind::object)),
        coordinated.parameter_prepared_statement_uuid);
    Require(!post_free_execution.ok &&
                post_free_execution.diagnostic.code ==
                    "SECURITY.ACCESS_DENIED",
            "stale coordination journal revived a freed prepared capability");

    const auto revoked = RevokeSblrPreparedStatementSessionV1(context);
    Require(revoked.ok && revoked.snapshot.session_revoked &&
                revoked.snapshot.records.front().state ==
                    SblrPreparedStatementRegistryStateV1::session_revoked,
            "explicit session termination was not durably terminal");
    const auto revoked_replay = RevokeSblrPreparedStatementSessionV1(context);
    Require(revoked_replay.ok && revoked_replay.exact_replay,
            "session revocation was not idempotent");
    const auto resurrect = PublishSblrPreparedStatementV1(
        context, Record("must_not_resurrect", 55));
    Require(!resurrect.ok &&
                resurrect.diagnostic.code == "SECURITY.ACCESS_DENIED",
            "terminated session resurrected a prepared statement");

    const auto corrupt_session =
        NewUuid(scratchbird::core::platform::UuidKind::session);
    const auto corrupt_context =
        Context(base, database, corrupt_session, principal);
    Remove(corrupt_context);
    RemoveCoordinationRegistry(context);
    Require(PublishSblrPreparedStatementV1(
                corrupt_context, Record("corrupt_me", 61))
                .ok,
            "corruption fixture publication failed");
    {
      std::fstream file(PathFor(corrupt_context),
                        std::ios::binary | std::ios::in | std::ios::out);
      Require(static_cast<bool>(file), "corruption fixture open failed");
      file.seekp(72);
      const char changed = static_cast<char>(0xa5);
      file.write(&changed, 1);
      file.flush();
      Require(static_cast<bool>(file), "corruption fixture write failed");
    }
    const auto corrupt =
        LoadSblrPreparedStatementRegistryV1(corrupt_context);
    Require(!corrupt.ok &&
                corrupt.diagnostic.code == "CATALOG.SNAPSHOT_STALE",
            "corrupt durable registry did not fail closed");

    const auto public_directory =
        std::filesystem::temp_directory_path() /
        ("sb_prepared_statement_public_lifecycle_" +
         std::to_string(ordinal));
    std::filesystem::create_directories(public_directory);
    RequirePublicSessionLifecycle(
        public_directory,
        NewUuid(scratchbird::core::platform::UuidKind::database));

    Remove(context);
    Remove(foreign_context);
    Remove(corrupt_context);
    std::error_code cleanup_error;
    std::filesystem::remove_all(public_directory, cleanup_error);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
