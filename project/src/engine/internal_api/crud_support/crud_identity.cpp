// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "crud_support/crud_store.hpp"
#include "time.hpp"
#include "uuid.hpp"

namespace scratchbird::engine::internal_api {
namespace {
namespace p = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;
namespace time = scratchbird::core::time;

[[noreturn]] void InvalidSuppliedIdentity() {
  throw CrudIdentityIssuanceError(uuid::MakeUuidDiagnostic(
      {p::StatusCode::uuid_invalid, p::Severity::error, p::Subsystem::uuid},
      "UUID.ENGINE_IDENTITY_NOT_V7", "crud.identity.requires_uuidv7"));
}
} // namespace

EngineUuid GenerateCrudEngineUuid(
    std::string_view kind, std::optional<std::uint64_t> unix_epoch_millis) {
  if (!unix_epoch_millis) {
    auto clock = time::ReadLocalNodeClockSnapshot();
    if (!clock.ok()) throw CrudIdentityIssuanceError(std::move(clock.diagnostic));
    auto converted = time::WallClockToUuidV7Millis(clock.value.wall_clock);
    if (!converted.ok()) throw CrudIdentityIssuanceError(std::move(converted.diagnostic));
    unix_epoch_millis = converted.unix_epoch_millis;
  }
  // Preserve the existing declared kind mapping. Operational labels are not
  // additional UUID kinds and supply no catalog, time or security authority.
  p::UuidKind uuid_kind = p::UuidKind::object;
  if (kind == "row") uuid_kind = p::UuidKind::row;
  else if (kind == "transaction") uuid_kind = p::UuidKind::transaction;
  else if (kind == "schema") uuid_kind = p::UuidKind::schema;
  else if (kind == "database") uuid_kind = p::UuidKind::database;
  auto generated = uuid::GenerateEngineIdentityV7(uuid_kind, *unix_epoch_millis);
  if (!generated.ok()) throw CrudIdentityIssuanceError(std::move(generated.diagnostic));
  return generated.value.value;
}

EngineUuid UuidOrGenerated(const EngineUuid& value, std::string_view kind) {
  if (value.is_nil()) return GenerateCrudEngineUuid(kind);
  if (!uuid::IsEngineIdentityUuid(value)) InvalidSuppliedIdentity();
  return value;
}
} // namespace scratchbird::engine::internal_api
