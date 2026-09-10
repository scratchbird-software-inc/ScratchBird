// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/delete_durable_authority_codec.hpp"
#include "dml/datatype_operator_registry_projection.hpp"
#include "hash_digest.hpp"

namespace scratchbird::engine::internal_api {
bool MatchesDmlDeleteDurableAuthorityOwnerV1(const EngineRequestContext& c,
    const DmlDeleteDurableAuthorityBundleV1& b) {
  namespace p = datatype_operator_projection;
  const auto& d = b.descriptor;
  wire::TypedUpdateHash owner{};
  return c.security_context_present && c.authorization_context.present &&
      c.statement_metadata_snapshot_engine_owned && c.statement_snapshot_generation &&
      !c.read_only_mode && !c.cluster_transaction_active && !c.route_fence_present &&
      c.principal_uuid.canonical == c.authorization_context.principal_uuid.canonical &&
      p::UuidText(b.database_uuid) == c.database_uuid.canonical &&
      p::UuidText(b.session_uuid) == c.session_uuid.canonical &&
      p::UuidText(b.principal_uuid) == c.principal_uuid.canonical &&
      p::UuidText(d.owning_transaction_uuid) == c.transaction_uuid.canonical &&
      d.owning_local_transaction_id == c.local_transaction_id &&
      p::UuidText(d.authenticated_statement_receipt_uuid) == c.statement_receipt_uuid.canonical &&
      p::UuidText(d.statement_snapshot_uuid) == c.statement_snapshot_uuid.canonical &&
      p::UuidText(d.catalog_snapshot_uuid) == c.statement_metadata_snapshot_uuid.canonical &&
      d.catalog_generation == c.catalog_generation_id &&
      d.datatype_registry_generation == c.datatype_registry_generation &&
      p::UuidText(b.datatypes.identity.vector_uuid) == c.datatype_catalog_snapshot_uuid.canonical &&
      b.security.security_context_uuid == c.authorization_context.authority_uuid.canonical &&
      b.security.security_context_generation == c.authorization_context.security_context_generation &&
      ComputeDmlDeleteOwnerContextHashV1(c, &owner) && owner == b.owner_context_sha256;
}

bool ComputeDmlDeleteOwnerContextHashV1(const EngineRequestContext& c, wire::TypedUpdateHash* out) {
  if (!out) return false;
  constexpr std::string_view domain = "ScratchBird.DmlDelete.OwnerContext.V1";
  std::vector<std::uint8_t> bytes(domain.begin(), domain.end());
  const auto number = [&](std::uint64_t value) {
    for (unsigned n = 0; n < 8; ++n) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * n)));
  };
  const auto text = [&](std::string_view value) {
    number(value.size()); bytes.insert(bytes.end(), value.begin(), value.end());
  };
  const auto uuid = [&](const EngineUuid& value, bool optional = false) {
    wire::TypedUpdateUuid binary{};
    if (!(optional && value.canonical.empty()) && !datatype_operator_projection::TypedUuid(value.canonical, &binary))
      return false;
    bytes.insert(bytes.end(), binary.begin(), binary.end()); return true;
  };
  const auto list = [&](const auto& values) {
    number(values.size()); for (const auto value : values) number(value);
  };
  text(c.database_path);
  if (!uuid(c.database_uuid) || !uuid(c.session_uuid) || !uuid(c.principal_uuid) || !uuid(c.transaction_uuid)) return false;
  number(c.local_transaction_id);
  if (!uuid(c.statement_uuid) || !uuid(c.statement_receipt_uuid) || !uuid(c.statement_snapshot_uuid)) return false;
  number(c.statement_snapshot_generation);
  if (!uuid(c.statement_metadata_snapshot_uuid)) return false;
  number(c.statement_metadata_snapshot_engine_owned);
  number(c.snapshot_visible_through_local_transaction_id);
  number(c.statement_metadata_snapshot_visible_through_local_transaction_id);
  list(c.statement_metadata_snapshot_active_excluded_local_transaction_ids);
  list(c.statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids);
  number(c.catalog_generation_id);
  if (!uuid(c.catalog_epoch_uuid, true)) return false;
  number(c.resource_epoch);
  if (!uuid(c.resource_admission_uuid)) return false;
  number(c.security_context_present); number(c.security_epoch); number(c.authorization_context.present);
  if (!uuid(c.authorization_context.authority_uuid)) return false;
  number(c.authorization_context.security_context_generation);
  if (!uuid(c.authorization_context.principal_uuid)) return false;
  number(c.authorization_context.catalog_generation_id); number(c.authorization_context.security_epoch);
  number(c.authorization_context.policy_epoch);
  if (!uuid(c.current_role_uuid, true) || !uuid(c.transaction_policy_snapshot_uuid, true)) return false;
  number(c.transaction_policy_snapshot_generation);
  if (!uuid(c.datatype_catalog_snapshot_uuid)) return false;
  number(c.datatype_catalog_generation); number(c.datatype_registry_generation);
  text(c.transaction_isolation_level);
  number(c.read_only_mode); number(c.cluster_transaction_active); number(c.route_fence_present);
  const auto hash = scratchbird::core::hash::ComputeSha256Digest(bytes);
  if (!hash.ok() || !std::any_of(hash.digest.begin(), hash.digest.end(), [](auto b) { return b != 0; })) return false;
  *out = hash.digest; return true;
}
}  // namespace scratchbird::engine::internal_api
