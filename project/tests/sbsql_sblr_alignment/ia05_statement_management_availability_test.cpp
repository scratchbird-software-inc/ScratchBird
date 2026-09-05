// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "engine/internal_api/sblr_executor_availability_registry.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace api = scratchbird::engine::internal_api;

namespace {

struct Profile {
  std::string_view mode;
  std::string_view suffix;
  std::string_view csc_id;
  api::SblrExecutorAvailabilityRowIdentity identity;
};

std::array<Profile, 5> Profiles() {
  return {{
      {"stmt-prepare", ".stmt_prepare", "CSC-TEST-003575",
       {api::kSblrStmtPrepareExecutorId,
        api::kSblrStmtPrepareOpcodeCode,
        api::kSblrStmtPrepareOpcodeVersion,
        api::kSblrStmtPrepareOperandDescriptorId,
        api::kSblrStmtPrepareResultDescriptorId,
        api::kSblrStmtPrepareResultDescriptorVersion}},
      {"stmt-execute", ".stmt_execute", "CSC-TEST-003579",
       {api::kSblrStmtExecuteExecutorId,
        api::kSblrStmtExecuteOpcodeCode,
        api::kSblrStmtExecuteOpcodeVersion,
        api::kSblrStmtExecuteOperandDescriptorId,
        api::kSblrStmtExecuteResultDescriptorId,
        api::kSblrStmtExecuteResultDescriptorVersion}},
      {"stmt-execute-direct", ".stmt_execute_direct", "CSC-TEST-003583",
       {api::kSblrStmtExecuteDirectExecutorId,
        api::kSblrStmtExecuteDirectOpcodeCode,
        api::kSblrStmtExecuteDirectOpcodeVersion,
        api::kSblrStmtExecuteDirectOperandDescriptorId,
        api::kSblrStmtExecuteDirectResultDescriptorId,
        api::kSblrStmtExecuteDirectResultDescriptorVersion}},
      {"stmt-free", ".stmt_free", "CSC-TEST-003587",
       {api::kSblrStmtFreeExecutorId,
        api::kSblrStmtFreeOpcodeCode,
        api::kSblrStmtFreeOpcodeVersion,
        api::kSblrStmtFreeOperandDescriptorId,
        api::kSblrStmtFreeResultDescriptorId,
        api::kSblrStmtFreeResultDescriptorVersion}},
      {"stmt-cancel", ".stmt_cancel", "CSC-TEST-003591",
       {api::kSblrStmtCancelExecutorId,
        api::kSblrStmtCancelOpcodeCode,
        api::kSblrStmtCancelOpcodeVersion,
        api::kSblrStmtCancelOperandDescriptorId,
        api::kSblrStmtCancelResultDescriptorId,
        api::kSblrStmtCancelResultDescriptorVersion}},
  }};
}

[[noreturn]] void Fail(const Profile& profile, const char* message) {
  std::cerr << profile.csc_id << ": " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(const Profile& profile, bool condition, const char* message) {
  if (!condition) Fail(profile, message);
}

class Fixture {
 public:
  explicit Fixture(std::string_view mode)
      : database_path(
            (std::filesystem::temp_directory_path() /
             ("sb_statement_management_availability_" + std::string(mode) +
              "_" +
              std::to_string(std::chrono::steady_clock::now()
                                 .time_since_epoch()
                                 .count())))
                .string()) {}

  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  ~Fixture() {
    std::error_code ignored;
    for (const auto& profile : Profiles()) {
      std::filesystem::remove(
          database_path +
              ".sb.sblr_executor_availability_registry.v1" +
              std::string(profile.suffix),
          ignored);
    }
  }

  std::string database_path;
};

api::EngineRequestContext Context(const Fixture& fixture,
                                  const char* statement_uuid) {
  api::EngineRequestContext context;
  context.database_path = fixture.database_path;
  context.database_uuid.canonical =
      "019d0000-0000-7000-8000-000000003575";
  context.statement_uuid.canonical = statement_uuid;
  context.security_context_present = true;
  context.trace_tags = {"right:SBLR_EXECUTOR_AVAILABILITY_ADMIN"};
  return context;
}

}  // namespace

int main(int argc, char** argv) {
  const auto profiles = Profiles();
  if (argc != 2) {
    std::cerr << "usage: " << argv[0]
              << " <stmt-prepare|stmt-execute|stmt-execute-direct|stmt-free|"
                 "stmt-cancel>\n";
    return EXIT_FAILURE;
  }

  const std::string_view mode = argv[1];
  const Profile* selected = nullptr;
  for (const auto& profile : profiles) {
    if (profile.mode == mode) {
      selected = &profile;
      break;
    }
  }
  if (selected == nullptr) {
    std::cerr << "unknown statement-management availability mode: " << mode
              << '\n';
    return EXIT_FAILURE;
  }

  Fixture fixture(mode);
  auto admission_context = Context(
      fixture, "019d0000-0000-7000-8000-000000003576");

  std::vector<api::SblrExecutorAvailabilityRowIdentity> identities;
  identities.reserve(profiles.size());
  for (const auto& profile : profiles) identities.push_back(profile.identity);

  const auto admitted = api::LoadSblrExecutorAvailabilitySnapshots(
      admission_context,
      std::span<const api::SblrExecutorAvailabilityRowIdentity>(identities));
  Require(*selected,
          admitted.ok && admitted.cohort != nullptr &&
              admitted.rows.size() == profiles.size(),
          "statement availability cohort was not loaded as one exact batch");
  for (std::size_t index = 0; index < profiles.size(); ++index) {
    Require(*selected,
            admitted.rows[index].ok && admitted.rows[index].snapshot.installed &&
                admitted.cohort->identities[index].executor_id ==
                    profiles[index].identity.executor_id &&
                admitted.cohort->rows[index].snapshot.row_identity_sha256 ==
                    admitted.rows[index].snapshot.row_identity_sha256,
            "batched availability cohort lost exact tuple order or identity");
  }
  admission_context.statement_executor_availability_cohort = admitted.cohort;

  std::size_t selected_index = 0;
  while (selected_index < profiles.size() &&
         profiles[selected_index].mode != selected->mode) {
    ++selected_index;
  }
  const auto& pinned = admitted.rows[selected_index].snapshot;

  api::SblrExecutorAvailabilitySetRequest revoke;
  revoke.database_uuid = admission_context.database_uuid.canonical;
  revoke.expected_snapshot_uuid = pinned.snapshot_uuid;
  revoke.expected_generation = pinned.generation;
  revoke.exact_row_identity = selected->identity;
  revoke.requested_state = api::SblrExecutorAvailabilityState::revoked;
  revoke.reason_code = std::string(selected->csc_id);
  const auto revoked =
      api::SetSblrExecutorAvailability(admission_context, revoke);
  Require(*selected,
          revoked.ok && !revoked.snapshot.installed &&
              revoked.snapshot.availability_state ==
                  api::SblrExecutorAvailabilityState::revoked,
          "exact statement executor revocation was not published");

  api::SblrExecutorAvailabilitySnapshot current;
  const auto diagnostic = api::RevalidateSblrExecutorAvailability(
      admission_context, selected->identity, pinned, &current);
  Require(*selected,
          diagnostic.code == "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
              current.snapshot_uuid == revoked.snapshot.snapshot_uuid &&
              current.generation == revoked.snapshot.generation &&
              !current.installed,
          "pinned statement cohort hid current exact-row revocation");

  auto next_statement = Context(
      fixture, "019d0000-0000-7000-8000-000000003577");
  const std::vector selected_identity{selected->identity};
  const auto observed = api::LoadSblrExecutorAvailabilitySnapshots(
      next_statement,
      std::span<const api::SblrExecutorAvailabilityRowIdentity>(
          selected_identity));
  Require(*selected,
          observed.ok && observed.cohort != nullptr &&
              observed.rows.size() == 1 && observed.rows.front().ok &&
              !observed.rows.front().snapshot.installed &&
              observed.rows.front().snapshot.availability_state ==
                  api::SblrExecutorAvailabilityState::revoked,
          "next statement did not observe exact revoked authority");
  return EXIT_SUCCESS;
}
