#pragma once

#include "engine/internal_api/sblr_executor_availability_registry.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace scratchbird::tests::ia01 {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] inline void FailAvailabilityTest(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

inline void RequireAvailabilityTest(bool condition, std::string_view message) {
  if (!condition) {
    FailAvailabilityTest(message);
  }
}

class AvailabilityFixture final {
 public:
  AvailabilityFixture(std::string_view tag,
                      std::string_view database_uuid,
                      std::string_view store_suffix)
      : database_uuid_(database_uuid),
        store_suffix_(store_suffix) {
    database_path_ =
        (std::filesystem::temp_directory_path() /
         ("sb_ia01_" + std::string(tag) + "_" +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count())))
            .string();
  }

  AvailabilityFixture(const AvailabilityFixture&) = delete;
  AvailabilityFixture& operator=(const AvailabilityFixture&) = delete;

  ~AvailabilityFixture() {
    std::error_code ignored;
    std::filesystem::remove(StorePath(), ignored);
  }

  [[nodiscard]] std::string StorePath() const {
    return database_path_ +
           ".sb.sblr_executor_availability_registry.v1" + store_suffix_;
  }

  [[nodiscard]] api::EngineRequestContext Context(bool admin) const {
    api::EngineRequestContext context;
    context.database_path = database_path_;
    context.database_uuid.canonical = database_uuid_;
    context.security_context_present = true;
    if (admin) {
      context.trace_tags.push_back(
          "right:SBLR_EXECUTOR_AVAILABILITY_ADMIN");
    }
    return context;
  }

 private:
  std::string database_path_;
  std::string database_uuid_;
  std::string store_suffix_;
};

inline void RequireMissingExecutorEvidence(
    std::string_view tag,
    std::string_view database_uuid,
    std::string_view store_suffix,
    const api::SblrExecutorAvailabilityRowIdentity& identity) {
  RequireAvailabilityTest(
      api::IsAdmittedExecutorAvailabilityIdentity(identity),
      "exact executor availability tuple is not admitted");

  AvailabilityFixture fixture(tag, database_uuid, store_suffix);
  const auto read_context = fixture.Context(false);
  const auto admin_context = fixture.Context(true);

  const auto absent = api::LoadCurrentSblrExecutorAvailabilitySnapshot(
      read_context, identity);
  RequireAvailabilityTest(
      !absent.ok &&
          absent.diagnostic.code ==
              "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
          !std::filesystem::exists(fixture.StorePath()),
      "read-only lookup did not fail closed for absent executor evidence");

  const auto admitted = api::LoadSblrExecutorAvailabilitySnapshot(
      admin_context, identity);
  RequireAvailabilityTest(
      admitted.ok && admitted.snapshot.installed &&
          admitted.snapshot.generation == 1 &&
          admitted.snapshot.row_identity_sha256 ==
              api::ComputeSblrExecutorAvailabilityRowIdentitySha256(identity) &&
          std::filesystem::exists(fixture.StorePath()),
      "explicit executor evidence bootstrap failed");

  api::SblrExecutorAvailabilitySetRequest revoke;
  revoke.database_uuid = database_uuid;
  revoke.expected_snapshot_uuid = admitted.snapshot.snapshot_uuid;
  revoke.expected_generation = admitted.snapshot.generation;
  revoke.exact_row_identity = identity;
  revoke.requested_state = api::SblrExecutorAvailabilityState::revoked;
  revoke.reason_code = "test.ia01.missing_executor_evidence";
  const auto revoked =
      api::SetSblrExecutorAvailability(admin_context, revoke);
  RequireAvailabilityTest(
      revoked.ok && !revoked.snapshot.installed &&
          revoked.snapshot.availability_state ==
              api::SblrExecutorAvailabilityState::revoked &&
          revoked.snapshot.generation == 2,
      "executor evidence revocation was not durably published");

  api::SblrExecutorAvailabilitySnapshot observed;
  const auto diagnostic = api::RevalidateSblrExecutorAvailability(
      read_context, identity, admitted.snapshot, &observed);
  RequireAvailabilityTest(
      diagnostic.code == "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
          diagnostic.message_key ==
              "sblr.opcode.executor_evidence_missing" &&
          observed.snapshot_uuid == revoked.snapshot.snapshot_uuid &&
          observed.generation == revoked.snapshot.generation &&
          observed.row_identity_sha256 ==
              admitted.snapshot.row_identity_sha256 &&
          !observed.installed,
      "dispatch-time revalidation did not observe exact revoked evidence");

  const auto current = api::LoadCurrentSblrExecutorAvailabilitySnapshot(
      read_context, identity);
  RequireAvailabilityTest(
      current.ok && current.snapshot.snapshot_uuid == observed.snapshot_uuid &&
          current.snapshot.generation == observed.generation &&
          current.snapshot.availability_state ==
              api::SblrExecutorAvailabilityState::revoked,
      "read-only current lookup did not retain the revoked authority row");
}

}  // namespace scratchbird::tests::ia01
