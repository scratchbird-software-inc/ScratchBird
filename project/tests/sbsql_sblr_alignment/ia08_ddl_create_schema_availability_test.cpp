// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/internal_api/sblr_executor_availability_registry.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace api = scratchbird::engine::internal_api;

namespace {

[[noreturn]] void Fail(const char* message) {
  std::cerr << "CSC-TEST-005782: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const char* message) {
  if (!condition) Fail(message);
}

struct Fixture {
  std::string database_path;

  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove(
        database_path +
            ".sb.sblr_executor_availability_registry.v1.ddl_create_schema",
        ignored);
  }
};

Fixture MakeFixture() {
  Fixture fixture;
  fixture.database_path =
      (std::filesystem::temp_directory_path() /
       ("sb_ddl_create_schema_availability_" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count())))
          .string();
  return fixture;
}

api::SblrExecutorAvailabilityRowIdentity Identity() {
  return {api::kSblrDdlCreateSchemaExecutorId,
          api::kSblrDdlCreateSchemaOpcodeCode,
          api::kSblrDdlCreateSchemaOpcodeVersion,
          api::kSblrDdlCreateSchemaOperandDescriptorId,
          api::kSblrDdlCreateSchemaResultDescriptorId,
          api::kSblrDdlCreateSchemaResultDescriptorVersion};
}

}  // namespace

int main() {
  const auto fixture = MakeFixture();
  api::EngineRequestContext context;
  context.database_path = fixture.database_path;
  context.database_uuid.canonical =
      "019d0000-0000-7000-8000-000000002571";
  context.statement_uuid.canonical =
      "019d0000-0000-7000-8000-000000002595";
  context.security_context_present = true;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};

  const std::vector identities{Identity()};
  const auto batch = api::LoadSblrExecutorAvailabilitySnapshots(
      context,
      std::span<const api::SblrExecutorAvailabilityRowIdentity>(identities));
  Require(batch.ok && batch.cohort != nullptr && batch.rows.size() == 1 &&
              batch.rows.front().ok && batch.rows.front().snapshot.installed,
          "exact CREATE SCHEMA availability cohort was not installed");

  context.statement_executor_availability_cohort = batch.cohort;
  const auto pinned =
      api::LoadSblrExecutorAvailabilitySnapshot(context, identities.front());
  Require(pinned.ok && pinned.snapshot.installed &&
              pinned.snapshot.snapshot_uuid ==
                  batch.rows.front().snapshot.snapshot_uuid &&
              pinned.snapshot.generation ==
                  batch.rows.front().snapshot.generation,
          "statement-scoped CREATE SCHEMA row changed after exact batch load");

  auto mismatched_identity = identities.front();
  ++mismatched_identity.opcode_code;
  const std::vector mismatched_identities{mismatched_identity};
  const auto refused = api::LoadSblrExecutorAvailabilitySnapshots(
      context,
      std::span<const api::SblrExecutorAvailabilityRowIdentity>(
          mismatched_identities));
  Require(!refused.ok && refused.rows.empty() &&
              refused.diagnostic.code == "SBLR.OPERAND_INVALID",
          "mismatched CREATE SCHEMA executor tuple was not refused");
  return EXIT_SUCCESS;
}
