#include "sblr_executor_availability_registry.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(const char* message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}
void Require(bool value, const char* message) { if (!value) Fail(message); }

std::string Id(UuidKind kind, std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1786831000000ull + salt);
  Require(generated.ok(), "uuid generation failed");
  return uuid::UuidToString(generated.value.value);
}

struct Fixture {
  std::string database_path;
  std::string database_uuid;
  ~Fixture() {
    std::error_code ignored;
    std::filesystem::remove(
        database_path + ".sb.sblr_executor_availability_registry.v1", ignored);
    std::filesystem::remove(
        database_path + ".sb.sblr_executor_availability_registry.v1.parameter",
        ignored);
  }
};

Fixture MakeFixture(std::uint64_t salt) {
  Fixture fixture;
  fixture.database_path =
      (std::filesystem::temp_directory_path() /
       ("sb_executor_availability_" + std::to_string(salt) + "_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
          .string();
  fixture.database_uuid = Id(UuidKind::database, salt);
  return fixture;
}

api::EngineRequestContext Context(const Fixture& fixture, bool admin) {
  api::EngineRequestContext context;
  context.database_path = fixture.database_path;
  context.database_uuid.canonical = fixture.database_uuid;
  context.security_context_present = true;
  if (admin) context.trace_tags.push_back(
      "right:SBLR_EXECUTOR_AVAILABILITY_ADMIN");
  return context;
}

api::SblrExecutorAvailabilitySetRequest SetRequest(
    const Fixture& fixture,
    const api::SblrExecutorAvailabilitySnapshot& expected,
    api::SblrExecutorAvailabilityState state,
    std::string reason) {
  api::SblrExecutorAvailabilitySetRequest request;
  request.database_uuid = fixture.database_uuid;
  request.expected_snapshot_uuid = expected.snapshot_uuid;
  request.expected_generation = expected.generation;
  request.requested_state = state;
  request.reason_code = std::move(reason);
  return request;
}
}  // namespace

int main() {
  Fixture fixture = MakeFixture(1);
  const auto context = Context(fixture, true);
  api::SblrExecutorAvailabilityRowIdentity parameter_identity;
  parameter_identity.executor_id = api::kSblrParameterExecutorId;
  parameter_identity.opcode_code = api::kSblrParameterOpcodeCode;
  parameter_identity.opcode_version = api::kSblrParameterOpcodeVersion;
  parameter_identity.operand_descriptor_id =
      api::kSblrParameterOperandDescriptorId;
  parameter_identity.result_descriptor_id =
      api::kSblrParameterResultDescriptorId;
  parameter_identity.result_descriptor_version =
      api::kSblrParameterResultDescriptorVersion;
  const auto parameter_bootstrap =
      api::LoadSblrExecutorAvailabilitySnapshot(context, parameter_identity);
  Require(parameter_bootstrap.ok && parameter_bootstrap.snapshot.installed &&
              parameter_bootstrap.snapshot.generation == 1,
          "parameter executor bootstrap missing");
  Require(parameter_bootstrap.snapshot.row_identity_sha256 !=
              api::ComputeSblrExecutorAvailabilityRowIdentitySha256({}),
          "parameter executor reused literal row identity");
  auto parameter_revoke_request =
      SetRequest(fixture, parameter_bootstrap.snapshot,
                 api::SblrExecutorAvailabilityState::revoked,
                 "test.parameter.revoke");
  parameter_revoke_request.exact_row_identity = parameter_identity;
  const auto parameter_revoked = api::SetSblrExecutorAvailability(
      context, parameter_revoke_request);
  Require(parameter_revoked.ok && parameter_revoked.snapshot.generation == 2,
          "parameter executor revoke failed");
  api::SblrExecutorAvailabilitySnapshot parameter_observed;
  const auto parameter_diagnostic = api::RevalidateSblrExecutorAvailability(
      context, parameter_identity, parameter_bootstrap.snapshot,
      &parameter_observed);
  Require(parameter_diagnostic.code ==
              "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
              parameter_observed.snapshot_uuid ==
                  parameter_revoked.snapshot.snapshot_uuid,
          "parameter executor revocation was not independent");
  const auto bootstrap = api::LoadSblrExecutorAvailabilitySnapshot(context);
  Require(bootstrap.ok && bootstrap.snapshot.generation == 1 &&
              bootstrap.snapshot.installed &&
              bootstrap.snapshot.availability_state ==
                  api::SblrExecutorAvailabilityState::installed &&
              bootstrap.snapshot.row_identity_sha256.starts_with("sha256:") &&
              bootstrap.snapshot.decision_evidence_sha256.starts_with("sha256:"),
          "exact durable bootstrap snapshot missing");

  const auto restarted = api::LoadSblrExecutorAvailabilitySnapshot(context);
  Require(restarted.ok &&
              restarted.snapshot.snapshot_uuid == bootstrap.snapshot.snapshot_uuid &&
              restarted.snapshot.generation == bootstrap.snapshot.generation,
          "restart did not recover immutable bootstrap snapshot");

  const auto unauthorized = api::SetSblrExecutorAvailability(
      Context(fixture, false),
      SetRequest(fixture, bootstrap.snapshot,
                 api::SblrExecutorAvailabilityState::revoked,
                 "test.unauthorized"));
  Require(!unauthorized.ok &&
              unauthorized.diagnostic.code == "SECURITY.ACCESS_DENIED",
          "unauthorized registry change did not fail closed");

  const auto revoked = api::SetSblrExecutorAvailability(
      context, SetRequest(fixture, bootstrap.snapshot,
                          api::SblrExecutorAvailabilityState::revoked,
                          "test.revoke.after.admission"));
  Require(revoked.ok && revoked.snapshot.generation == 2 &&
              !revoked.snapshot.installed,
          "compare-and-publish revoke failed");
  api::SblrExecutorAvailabilitySnapshot observed;
  auto diagnostic = api::RevalidateSblrExecutorAvailability(
      context, bootstrap.snapshot, &observed);
  Require(diagnostic.code == "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING" &&
              observed.snapshot_uuid == revoked.snapshot.snapshot_uuid,
          "dispatch did not observe durable revocation");

  const auto stale_set = api::SetSblrExecutorAvailability(
      context, SetRequest(fixture, bootstrap.snapshot,
                          api::SblrExecutorAvailabilityState::installed,
                          "test.stale.compare"));
  Require(!stale_set.ok && stale_set.diagnostic.code ==
              "SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE",
          "stale compare-and-publish was not refused");

  const auto unavailable = api::SetSblrExecutorAvailability(
      context, SetRequest(fixture, revoked.snapshot,
                          api::SblrExecutorAvailabilityState::unavailable,
                          "test.unavailable"));
  Require(unavailable.ok && unavailable.snapshot.generation == 3,
          "unavailable publication failed");
  diagnostic = api::RevalidateSblrExecutorAvailability(
      context, revoked.snapshot, nullptr);
  Require(diagnostic.code == "SBLR.OPCODE.EXECUTOR_UNAVAILABLE",
          "explicit unavailable precedence mismatch");

  const auto installed = api::SetSblrExecutorAvailability(
      context, SetRequest(fixture, unavailable.snapshot,
                          api::SblrExecutorAvailabilityState::installed,
                          "test.reinstall"));
  Require(installed.ok && installed.snapshot.generation == 4,
          "reinstall publication failed");
  diagnostic = api::RevalidateSblrExecutorAvailability(
      context, bootstrap.snapshot, nullptr);
  Require(diagnostic.code == "SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE",
          "installed generation mismatch did not refuse stale evidence");

  // A durable evidence record without its matching immutable snapshot is a
  // torn publication and must not fall back to generation 4.
  {
    const std::string path = fixture.database_path +
        ".sb.sblr_executor_availability_registry.v1";
    std::ifstream in(path, std::ios::binary);
    std::string evidence;
    std::getline(in, evidence);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << evidence << '\n';
  }
  const auto torn = api::LoadSblrExecutorAvailabilitySnapshot(context);
  Require(!torn.ok && torn.diagnostic.code ==
              "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "torn restart recovery inferred installed state");

  Fixture contradictory_fixture = MakeFixture(2);
  const auto contradictory_context = Context(contradictory_fixture, true);
  Require(api::LoadSblrExecutorAvailabilitySnapshot(contradictory_context).ok,
          "contradiction fixture bootstrap failed");
  const std::string contradiction_path = contradictory_fixture.database_path +
      ".sb.sblr_executor_availability_registry.v1";
  std::ifstream input(contradiction_path, std::ios::binary);
  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  const auto hash = contents.find("sha256:");
  Require(hash != std::string::npos, "bootstrap evidence hash absent");
  contents[hash + 7] = contents[hash + 7] == '0' ? '1' : '0';
  {
    std::ofstream output(contradiction_path,
                         std::ios::binary | std::ios::trunc);
    output << contents;
  }
  const auto contradictory =
      api::LoadSblrExecutorAvailabilitySnapshot(contradictory_context);
  Require(!contradictory.ok && contradictory.diagnostic.code ==
              "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "contradictory evidence did not fail closed");
  return EXIT_SUCCESS;
}
