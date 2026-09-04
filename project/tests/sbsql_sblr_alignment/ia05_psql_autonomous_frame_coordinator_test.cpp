#include "engine/internal_api/sblr_autonomous_frame_coordinator.hpp"
#include "uuid.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>

namespace api = scratchbird::engine::internal_api;
namespace {
std::string Id(scratchbird::core::platform::UuidKind kind) {
  static std::uint64_t stamp = 1787010000000ull;
  if (!scratchbird::core::uuid::UuidKindAllowsDurableIdentity(kind)) {
    auto raw = scratchbird::core::uuid::GenerateCompatibilityUnixTimeV7(++stamp);
    assert(raw.ok());
    auto typed = scratchbird::core::uuid::MakeTypedUuid(kind, raw.value);
    assert(typed.ok());
    return scratchbird::core::uuid::UuidToString(typed.value.value);
  }
  auto value = scratchbird::core::uuid::GenerateEngineIdentityV7(kind, ++stamp);
  assert(value.ok());
  return scratchbird::core::uuid::UuidToString(value.value.value);
}
}

int main() {
  const auto base = std::filesystem::temp_directory_path() /
      ("sb_autonomous_" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  api::EngineRequestContext context;
  context.database_path = base.string();
  context.database_uuid.canonical = Id(scratchbird::core::platform::UuidKind::database);
  context.session_uuid.canonical = Id(scratchbird::core::platform::UuidKind::session);
  context.principal_uuid.canonical = Id(scratchbird::core::platform::UuidKind::principal);
  context.transaction_uuid.canonical = Id(scratchbird::core::platform::UuidKind::object);
  context.statement_uuid.canonical = Id(scratchbird::core::platform::UuidKind::object);
  context.security_context_present = true;
  context.statement_metadata_snapshot_engine_owned = true;
  context.trace_tags = {"private_psql_autonomous_body_compiler"};

  api::SblrAutonomousBodyFrameProjectionV1 projection;
  projection.preliminary_receipt_uuid = context.statement_uuid.canonical;
  projection.structural_occurrence_id = 7;
  projection.parent_transaction_uuid = context.transaction_uuid.canonical;
  projection.parent_frame_uuid = Id(scratchbird::core::platform::UuidKind::object);
  projection.database_uuid = context.database_uuid.canonical;
  projection.attachment_uuid = Id(scratchbird::core::platform::UuidKind::object);
  projection.session_uuid = context.session_uuid.canonical;
  projection.principal_uuid = context.principal_uuid.canonical;
  projection.security_snapshot_uuid = Id(scratchbird::core::platform::UuidKind::object);
  projection.policy_snapshot_uuid = Id(scratchbird::core::platform::UuidKind::object);
  projection.catalog_generation = 3;
  projection.capability_generation = 4;
  projection.body_sblr_uuid = Id(scratchbird::core::platform::UuidKind::object);
  projection.body_sblr_sha256 = "sha256:" + std::string(64, '1');
  projection.intent = 2;
  projection.nesting_depth = 1;
  projection.effect_count = 2;
  projection.effect_set_sha256 = "sha256:" + std::string(64, '2');
  projection.projection_evidence_sha256 = "sha256:" + std::string(64, '3');

  assert(api::PublishSblrAutonomousBodyFrameProjection(context, projection).code == "OK");
  context.trace_tags = {"private_psql_autonomous_frame_coordination"};
  auto reserved = api::ReserveSblrAutonomousFrame(
      context, projection.preliminary_receipt_uuid,
      projection.structural_occurrence_id);
  assert(reserved.ok && reserved.snapshot.frame_generation &&
         reserved.snapshot.child_transaction_number &&
         reserved.snapshot.authority.body_sblr_uuid == projection.body_sblr_uuid &&
         reserved.snapshot.descriptor_evidence_sha256 != projection.effect_set_sha256);
  auto finalized = api::FinalizeSblrAutonomousFrame(
      context, reserved.snapshot.frame_uuid,
      reserved.snapshot.frame_generation, true);
  assert(finalized.ok &&
         finalized.snapshot.state == api::SblrAutonomousFrameState::committed);
  auto replay = api::FinalizeSblrAutonomousFrame(
      context, reserved.snapshot.frame_uuid,
      reserved.snapshot.frame_generation, true);
  assert(!replay.ok && replay.diagnostic.code == "PSQL.AUTONOMOUS_TRANSACTION_REFUSED");

  context.trace_tags = {"private_psql_autonomous_body_compiler"};
  assert(api::CompileAndPublishSblrAutonomousBodyFrameProjection(
             context, context.statement_uuid.canonical, 8).code == "OK");
  context.trace_tags = {"private_psql_autonomous_frame_coordination"};
  auto compiled = api::ReserveSblrAutonomousFrame(
      context, context.statement_uuid.canonical, 8);
  assert(compiled.ok && compiled.snapshot.authority.structural_occurrence_id == 8);

  std::error_code ec;
  std::filesystem::remove(base.string() + ".sb.sblr_autonomous_frame.v1", ec);
  std::filesystem::remove(base.string() + ".sb.sblr_autonomous_body_frame_projection.v1", ec);
  return 0;
}
