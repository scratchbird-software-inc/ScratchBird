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
  std::cerr << "CSC-TEST-003631: " << message << '\n';
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
            ".sb.sblr_executor_availability_registry.v1.catalog_epoch_check",
        ignored);
  }
};

Fixture MakeFixture() {
  Fixture fixture;
  fixture.database_path =
      (std::filesystem::temp_directory_path() /
       ("sb_catalog_epoch_check_availability_" +
        std::to_string(std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count())))
          .string();
  return fixture;
}

api::EngineRequestContext Context(const Fixture& fixture,
                                  const char* statement_uuid) {
  api::EngineRequestContext context;
  context.database_path = fixture.database_path;
  context.database_uuid.canonical =
      "019d0000-0000-7000-8000-000000003631";
  context.statement_uuid.canonical = statement_uuid;
  context.security_context_present = true;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};
  return context;
}

api::SblrExecutorAvailabilityRowIdentity Identity() {
  return {api::kSblrCatalogEpochCheckExecutorId,
          api::kSblrCatalogEpochCheckOpcodeCode,
          api::kSblrCatalogEpochCheckOpcodeVersion,
          api::kSblrCatalogEpochCheckOperandDescriptorId,
          api::kSblrCatalogEpochCheckResultDescriptorId,
          api::kSblrCatalogEpochCheckResultDescriptorVersion};
}

}  // namespace

int main() {
  auto fixture = MakeFixture();
  auto statement = Context(
      fixture, "019d0000-0000-7000-8000-000000003632");
  const auto identity = Identity();
  const std::vector identities{identity};

  const auto admitted = api::LoadSblrExecutorAvailabilitySnapshots(
      statement,
      std::span<const api::SblrExecutorAvailabilityRowIdentity>(identities));
  Require(admitted.ok && admitted.cohort != nullptr &&
              admitted.rows.size() == 1 && admitted.rows.front().ok &&
              admitted.rows.front().snapshot.installed,
          "catalog-epoch-check admission cohort was not installed");
  statement.statement_executor_availability_cohort = admitted.cohort;

  const auto pinned = api::LoadSblrExecutorAvailabilitySnapshot(
      statement, identity);
  Require(pinned.ok && pinned.snapshot.installed &&
              pinned.snapshot.generation ==
                  admitted.rows.front().snapshot.generation,
          "statement cohort did not retain its immutable exact row");

  api::SblrExecutorAvailabilitySetRequest revoke;
  revoke.database_uuid = statement.database_uuid.canonical;
  revoke.expected_snapshot_uuid = admitted.rows.front().snapshot.snapshot_uuid;
  revoke.expected_generation = admitted.rows.front().snapshot.generation;
  revoke.exact_row_identity = identity;
  revoke.requested_state = api::SblrExecutorAvailabilityState::revoked;
  revoke.reason_code = "CSC-TEST-003631";
  const auto revoked = api::SetSblrExecutorAvailability(statement, revoke);
  Require(revoked.ok && !revoked.snapshot.installed &&
              revoked.snapshot.availability_state ==
                  api::SblrExecutorAvailabilityState::revoked,
          "catalog-epoch-check executor revocation was not published");

  api::SblrExecutorAvailabilitySnapshot current;
  const auto diagnostic = api::RevalidateSblrExecutorAvailability(
      statement, identity, admitted.rows.front().snapshot, &current);
  Require(diagnostic.code == "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
              current.snapshot_uuid == revoked.snapshot.snapshot_uuid &&
              current.generation == revoked.snapshot.generation &&
              !current.installed,
          "pinned cohort hid current catalog-epoch-check revocation");

  auto next_statement = Context(
      fixture, "019d0000-0000-7000-8000-000000003633");
  const auto observed = api::LoadSblrExecutorAvailabilitySnapshots(
      next_statement,
      std::span<const api::SblrExecutorAvailabilityRowIdentity>(identities));
  Require(observed.ok && observed.cohort != nullptr &&
              observed.rows.size() == 1 && observed.rows.front().ok &&
              !observed.rows.front().snapshot.installed &&
              observed.rows.front().snapshot.availability_state ==
                  api::SblrExecutorAvailabilityState::revoked,
          "next statement did not observe revoked catalog-epoch authority");
  return EXIT_SUCCESS;
}
