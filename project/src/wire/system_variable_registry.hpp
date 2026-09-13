// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../core/platform/runtime_platform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// Shared fixed seed metadata, not a catalog snapshot or execution authority.
// Parsers use names as a presentation projection. Engine binding is binary UUID
// only and must separately revalidate the owning catalog/security/MGA boundary.
namespace scratchbird::wire {
using SystemVariableUuid = scratchbird::core::platform::Uuid;

enum class SystemVariableResultType : std::uint8_t {
  text, uuid, int64, int32, text_array, timestamp_tz, boolean
};
enum class SystemVariableSource : std::uint8_t {
  security, catalog, session, cluster, txn, engine
};
enum class SystemVariableScope : std::uint8_t {
  connection, session, statement, transaction, cluster_authority
};
enum class SystemVariableVolatility : std::uint8_t { immutable, stable, volatile_value };
enum class SystemVariableRight : std::uint8_t {
  public_read, catalog_read, random_seed_control, private_profile_read
};
// This enum is an in-process dispatch discriminator, never wire identity.
// Generated seed sections: generate_system_variable_seed.py v1.
// BEGIN SYSTEM VARIABLE ENUM
enum class SystemVariable : std::uint8_t {
  current_user,
  session_user,
  current_role,
  current_database,
  current_schema,
  current_catalog,
  current_cluster,
  current_cluster_uuid,
  cluster_epoch,
  cluster_member_id,
  current_session_uuid,
  current_session_id,
  current_request_uuid,
  current_transaction_id,
  current_statement_uuid,
  current_isolation_level,
  current_timezone,
  current_locale,
  current_engine_version,
  current_dialect_version,
  application_name,
  client_address,
  client_port,
  client_protocol,
  current_capability_set,
  now,
  statement_timestamp,
  transaction_timestamp,
  clock_timestamp,
  random_seed,
  read_only_session,
  tx_read_only,
  lock_timeout_ms,
  statement_timeout_ms,
  idle_in_transaction_session_timeout_ms,
  private_profile_active,
  evidence_chain_uuid,
};
// END SYSTEM VARIABLE ENUM

struct SystemVariableEntry {
  SystemVariable variable;
  SystemVariableUuid variable_uuid;
  std::string_view variable_id;
  std::string_view canonical_name;
  SystemVariableResultType result_type;
  SystemVariableSource source;
  SystemVariableScope scope;
  SystemVariableVolatility volatility;
  SystemVariableRight required_right;
  std::string_view diagnostic_if_unavailable;
};

// BEGIN SYSTEM VARIABLE ROWS
inline constexpr std::array<SystemVariableEntry, 37> kSystemVariableRegistry{{
  {SystemVariable::current_user, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7c, 0x0c, 0x93, 0x40, 0xe1, 0xe7, 0xeb, 0xf7, 0xaa, 0xae}},
   "sb.variable.current_user", "current_user", SystemVariableResultType::text,
   SystemVariableSource::security, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::session_user, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x72, 0x9f, 0x8d, 0x65, 0x1b, 0x69, 0x37, 0x38, 0x09, 0x9c}},
   "sb.variable.session_user", "session_user", SystemVariableResultType::text,
   SystemVariableSource::security, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::current_role, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x70, 0xa0, 0x8f, 0xc3, 0x43, 0xba, 0xf0, 0x20, 0x44, 0x5d}},
   "sb.variable.current_role", "current_role", SystemVariableResultType::text,
   SystemVariableSource::security, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::current_database, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7b, 0x72, 0x83, 0xbf, 0xf0, 0x16, 0x42, 0x20, 0x19, 0x5c}},
   "sb.variable.current_database", "current_database", SystemVariableResultType::text,
   SystemVariableSource::catalog, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::current_schema, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x71, 0xc7, 0x91, 0x55, 0x0a, 0xe1, 0x60, 0x17, 0x35, 0x7a}},
   "sb.variable.current_schema", "current_schema", SystemVariableResultType::text,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::current_catalog, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7a, 0xba, 0x9b, 0xdf, 0x9b, 0xba, 0x32, 0x3f, 0x40, 0x09}},
   "sb.variable.current_catalog", "current_catalog", SystemVariableResultType::text,
   SystemVariableSource::catalog, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::current_cluster, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x70, 0xc7, 0xab, 0x56, 0xf0, 0x65, 0xd7, 0x0f, 0xe2, 0x31}},
   "sb.variable.current_cluster", "current_cluster", SystemVariableResultType::text,
   SystemVariableSource::cluster, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.CLUSTER.AUTHORITY_REQUIRED"},
  {SystemVariable::current_cluster_uuid, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x71, 0xfc, 0x90, 0x5c, 0xe6, 0x03, 0x34, 0x5e, 0x46, 0x8c}},
   "sb.variable.current_cluster_uuid", "current_cluster_uuid", SystemVariableResultType::uuid,
   SystemVariableSource::cluster, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.CLUSTER.AUTHORITY_REQUIRED"},
  {SystemVariable::cluster_epoch, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x71, 0xd5, 0x9e, 0x60, 0x6f, 0x28, 0x72, 0xd8, 0xcb, 0xf0}},
   "sb.variable.cluster_epoch", "cluster_epoch", SystemVariableResultType::int64,
   SystemVariableSource::cluster, SystemVariableScope::cluster_authority,
   SystemVariableVolatility::volatile_value, SystemVariableRight::catalog_read,
   "SBSQL.CLUSTER.AUTHORITY_REQUIRED"},
  {SystemVariable::cluster_member_id, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7a, 0xaf, 0x9c, 0x80, 0xde, 0x2b, 0x56, 0xc4, 0x8d, 0x9d}},
   "sb.variable.cluster_member_id", "cluster_member_id", SystemVariableResultType::text,
   SystemVariableSource::cluster, SystemVariableScope::cluster_authority,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.CLUSTER.AUTHORITY_REQUIRED"},
  {SystemVariable::current_session_uuid, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7e, 0x97, 0xaf, 0xa6, 0x3e, 0xdf, 0xfb, 0x1b, 0x8c, 0x6d}},
   "sb.variable.current_session_uuid", "current_session_uuid", SystemVariableResultType::uuid,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::current_session_id, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x79, 0xf0, 0xaf, 0xac, 0x8f, 0x71, 0x72, 0x0e, 0xd8, 0xc9}},
   "sb.variable.current_session_id", "current_session_id", SystemVariableResultType::int64,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::current_request_uuid, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x76, 0x7e, 0xb8, 0xcf, 0xea, 0x1c, 0xcb, 0x6f, 0xc8, 0x38}},
   "sb.variable.current_request_uuid", "current_request_uuid", SystemVariableResultType::uuid,
   SystemVariableSource::session, SystemVariableScope::statement,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NO_REQUEST"},
  {SystemVariable::current_transaction_id, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x76, 0xb0, 0x9b, 0xb6, 0x30, 0x66, 0x75, 0x70, 0x18, 0x93}},
   "sb.variable.current_transaction_id", "current_transaction_id", SystemVariableResultType::int64,
   SystemVariableSource::txn, SystemVariableScope::transaction,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NO_TRANSACTION"},
  {SystemVariable::current_statement_uuid, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x70, 0xd2, 0xa1, 0x76, 0x5f, 0x02, 0x53, 0xe2, 0x0b, 0x5a}},
   "sb.variable.current_statement_uuid", "current_statement_uuid", SystemVariableResultType::uuid,
   SystemVariableSource::session, SystemVariableScope::statement,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NO_STATEMENT"},
  {SystemVariable::current_isolation_level, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x78, 0x3b, 0x89, 0xac, 0x63, 0xb3, 0xd3, 0x76, 0xa6, 0x62}},
   "sb.variable.current_isolation_level", "current_isolation_level", SystemVariableResultType::text,
   SystemVariableSource::txn, SystemVariableScope::transaction,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NO_TRANSACTION"},
  {SystemVariable::current_timezone, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x70, 0x86, 0xaa, 0xb7, 0x1f, 0xf7, 0x20, 0x59, 0x18, 0x11}},
   "sb.variable.current_timezone", "current_timezone", SystemVariableResultType::text,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::current_locale, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x75, 0x72, 0xa0, 0x9a, 0xc0, 0x1a, 0x6f, 0x0c, 0x06, 0x54}},
   "sb.variable.current_locale", "current_locale", SystemVariableResultType::text,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::current_engine_version, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x78, 0x25, 0xbc, 0xc8, 0xde, 0x85, 0xfa, 0x94, 0x7f, 0x72}},
   "sb.variable.current_engine_version", "current_engine_version", SystemVariableResultType::text,
   SystemVariableSource::engine, SystemVariableScope::session,
   SystemVariableVolatility::immutable, SystemVariableRight::public_read,
   "not_applicable"},
  {SystemVariable::current_dialect_version, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7f, 0x6a, 0xaa, 0xe2, 0x03, 0xce, 0x3c, 0x4d, 0x52, 0xa5}},
   "sb.variable.current_dialect_version", "current_dialect_version", SystemVariableResultType::text,
   SystemVariableSource::engine, SystemVariableScope::session,
   SystemVariableVolatility::immutable, SystemVariableRight::public_read,
   "not_applicable"},
  {SystemVariable::application_name, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x76, 0xd8, 0x8b, 0x2f, 0x35, 0xce, 0x31, 0xea, 0x6f, 0x51}},
   "sb.variable.application_name", "application_name", SystemVariableResultType::text,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::volatile_value, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::client_address, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7a, 0x41, 0xa3, 0xac, 0xb7, 0xff, 0x66, 0x52, 0x08, 0x1e}},
   "sb.variable.client_address", "client_address", SystemVariableResultType::text,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::client_port, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x72, 0x48, 0xa1, 0x21, 0xbd, 0x6e, 0xfb, 0x02, 0xe0, 0xbe}},
   "sb.variable.client_port", "client_port", SystemVariableResultType::int32,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::client_protocol, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x73, 0x43, 0x8f, 0x00, 0xa8, 0x03, 0x01, 0x35, 0xd3, 0x74}},
   "sb.variable.client_protocol", "client_protocol", SystemVariableResultType::text,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::current_capability_set, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x78, 0x34, 0x9c, 0xd1, 0x1a, 0x01, 0x83, 0xfa, 0x84, 0xdc}},
   "sb.variable.current_capability_set", "current_capability_set", SystemVariableResultType::text_array,
   SystemVariableSource::security, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::now, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x72, 0x4d, 0x8e, 0xbd, 0x23, 0x3d, 0xf9, 0xf1, 0xc3, 0x31}},
   "sb.variable.now", "now", SystemVariableResultType::timestamp_tz,
   SystemVariableSource::engine, SystemVariableScope::statement,
   SystemVariableVolatility::volatile_value, SystemVariableRight::public_read,
   "not_applicable"},
  {SystemVariable::statement_timestamp, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x74, 0x08, 0x81, 0x5f, 0x36, 0xa1, 0xeb, 0xe3, 0x34, 0xaa}},
   "sb.variable.statement_timestamp", "statement_timestamp", SystemVariableResultType::timestamp_tz,
   SystemVariableSource::engine, SystemVariableScope::statement,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "not_applicable"},
  {SystemVariable::transaction_timestamp, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x76, 0x2d, 0xbf, 0xcd, 0xc2, 0x08, 0xec, 0x87, 0x13, 0x77}},
   "sb.variable.transaction_timestamp", "transaction_timestamp", SystemVariableResultType::timestamp_tz,
   SystemVariableSource::txn, SystemVariableScope::transaction,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NO_TRANSACTION"},
  {SystemVariable::clock_timestamp, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7e, 0x5b, 0x94, 0x2f, 0xb1, 0x92, 0xf2, 0xcc, 0xea, 0x92}},
   "sb.variable.clock_timestamp", "clock_timestamp", SystemVariableResultType::timestamp_tz,
   SystemVariableSource::engine, SystemVariableScope::statement,
   SystemVariableVolatility::volatile_value, SystemVariableRight::public_read,
   "not_applicable"},
  {SystemVariable::random_seed, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7f, 0x4c, 0xae, 0x71, 0x1f, 0xff, 0x66, 0x2e, 0x4a, 0xb5}},
   "sb.variable.random_seed", "random_seed", SystemVariableResultType::int64,
   SystemVariableSource::engine, SystemVariableScope::session,
   SystemVariableVolatility::volatile_value, SystemVariableRight::random_seed_control,
   "SBSQL.CAPABILITY_REQUIRED"},
  {SystemVariable::read_only_session, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x74, 0xb0, 0xbf, 0xa1, 0x1b, 0x72, 0x83, 0x41, 0x47, 0x09}},
   "sb.variable.read_only_session", "read_only_session", SystemVariableResultType::boolean,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::tx_read_only, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7b, 0x17, 0x89, 0xb6, 0x74, 0x3b, 0x0e, 0x19, 0x65, 0x04}},
   "sb.variable.tx_read_only", "tx_read_only", SystemVariableResultType::boolean,
   SystemVariableSource::txn, SystemVariableScope::transaction,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NO_TRANSACTION"},
  {SystemVariable::lock_timeout_ms, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x71, 0xab, 0x85, 0x0e, 0xf0, 0x6d, 0x07, 0x2a, 0xef, 0x4e}},
   "sb.variable.lock_timeout_ms", "lock_timeout_ms", SystemVariableResultType::int32,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::statement_timeout_ms, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7e, 0x29, 0xa3, 0x50, 0xf1, 0x24, 0xdb, 0x05, 0xaf, 0x6b}},
   "sb.variable.statement_timeout_ms", "statement_timeout_ms", SystemVariableResultType::int32,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::idle_in_transaction_session_timeout_ms, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x71, 0x1a, 0x95, 0x6b, 0x9c, 0xc0, 0x52, 0x62, 0xff, 0xdf}},
   "sb.variable.idle_in_transaction_session_timeout_ms", "idle_in_transaction_session_timeout_ms", SystemVariableResultType::int32,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NOT_CONNECTED"},
  {SystemVariable::private_profile_active, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x75, 0x3c, 0xb5, 0xbb, 0xc6, 0x5d, 0xb5, 0x4c, 0x40, 0x48}},
   "sb.variable.private_profile_active", "private_profile_active", SystemVariableResultType::boolean,
   SystemVariableSource::session, SystemVariableScope::session,
   SystemVariableVolatility::stable, SystemVariableRight::private_profile_read,
   "SBSQL.CAPABILITY_REQUIRED"},
  {SystemVariable::evidence_chain_uuid, {{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x72, 0x34, 0xab, 0x1c, 0x8f, 0x95, 0x37, 0x06, 0xda, 0xb4}},
   "sb.variable.evidence_chain_uuid", "evidence_chain_uuid", SystemVariableResultType::uuid,
   SystemVariableSource::txn, SystemVariableScope::transaction,
   SystemVariableVolatility::stable, SystemVariableRight::public_read,
   "SBSQL.NO_TRANSACTION"},
}};
// END SYSTEM VARIABLE ROWS

constexpr bool ValidateSystemVariableSeed() noexcept {
  for (std::size_t i=0; i<kSystemVariableRegistry.size(); ++i) {
    const auto& a=kSystemVariableRegistry[i];
    if ((a.variable_uuid.bytes[6]>>4)!=7 || (a.variable_uuid.bytes[8]&0xc0)!=0x80 ||
        a.variable_id.empty() || a.canonical_name.empty() || a.diagnostic_if_unavailable.empty()) return false;
    for (std::size_t j=0; j<i; ++j) {
      const auto& b=kSystemVariableRegistry[j];
      if (a.variable==b.variable || a.variable_uuid==b.variable_uuid ||
          a.variable_id==b.variable_id || a.canonical_name==b.canonical_name) return false;
    }
  }
  return true;
}
static_assert(ValidateSystemVariableSeed());
static_assert(sizeof(SystemVariableUuid)==16);

inline constexpr auto kSystemVariableUuidOrder=[] {
  std::array<std::size_t,kSystemVariableRegistry.size()> indices{};
  for(std::size_t i=0;i<indices.size();++i) {
    std::size_t at=i;
    while(at && kSystemVariableRegistry[i].variable_uuid <
                     kSystemVariableRegistry[indices[at-1]].variable_uuid) {
      indices[at]=indices[at-1]; --at;
    }
    indices[at]=i;
  }
  return indices;
}();

constexpr std::span<const SystemVariableEntry> StandardSystemVariableRegistry() noexcept {
  return kSystemVariableRegistry;
}

constexpr const SystemVariableEntry* FindSystemVariable(
    const SystemVariableUuid& uuid) noexcept {
  std::size_t first=0, count=kSystemVariableUuidOrder.size();
  while(count) {
    const auto step=count/2, at=first+step;
    const auto& candidate=kSystemVariableRegistry[kSystemVariableUuidOrder[at]];
    if(candidate.variable_uuid<uuid) {first=at+1;count-=step+1;}
    else count=step;
  }
  if(first==kSystemVariableUuidOrder.size()) return nullptr;
  const auto& candidate=kSystemVariableRegistry[kSystemVariableUuidOrder[first]];
  return candidate.variable_uuid==uuid ? &candidate : nullptr;
}
} // namespace scratchbird::wire
