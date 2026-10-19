// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "uuid.hpp"
#include <array>
#include <optional>

namespace scratchbird::core::catalog {

enum class AgentBootstrapDefinitionKind : u64 { policy = 1, registration = 2 };
struct CatalogAgentStartupVerificationSelection {
  Uuid repair_policy_uuid,quarantine_policy_uuid,salvage_policy_uuid,evidence_policy_uuid;
  u64 verification_levels=0;
  bool operator==(const CatalogAgentStartupVerificationSelection&) const=default;
};

// Creation declarations only. Origin identity is checked against committed
// inventory by the owning reader; this value cannot grant service authority.
// Policy settings remain schema-defined configuration text, while every object
// reference and creation/generation binding is a native typed field.
struct CatalogAgentBootstrapDefinition {
  Uuid definition_uuid, database_uuid, policy_uuid;
  AgentBootstrapDefinitionKind kind = AgentBootstrapDefinitionKind::policy;
  std::string_view agent_type, properties;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0, generation = 0;
  std::optional<CatalogAgentStartupVerificationSelection> startup_verification;
};
struct CatalogAgentBootstrapDefinitionResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogAgentBootstrapDefinition> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
inline constexpr u32 kCatalogAgentBootstrapSchemaId = 65824;
inline constexpr CatalogValueSchemaView CatalogAgentBootstrapSchemaView(u16 version=1) {
  using T = CatalogValueType;
  static constexpr CatalogValueFieldSchema fields[]{
      {1, T::engine_identity, true, 16, UuidKind::object},
      {2, T::engine_identity, true, 16, UuidKind::database},
      {3, T::unsigned_integer, true, 8},
      {4, T::utf8_text, true, 128},
      {5, T::utf8_text, true, 32768},
      {6, T::engine_identity, true, 16, UuidKind::object},
      {7, T::engine_identity, true, 16, UuidKind::transaction},
      {8, T::unsigned_integer, true, 8},
      {9, T::unsigned_integer, true, 8}};
  static constexpr CatalogValueFieldSchema startup_fields[]{
      fields[0],fields[1],fields[2],fields[3],fields[4],fields[5],fields[6],fields[7],fields[8],
      {10,T::engine_identity,true,16,UuidKind::object},
      {11,T::engine_identity,true,16,UuidKind::object},
      {12,T::engine_identity,true,16,UuidKind::object},
      {13,T::engine_identity,true,16,UuidKind::object},
      {14,T::unsigned_integer,true,8}};
  if(version==1)return {kCatalogAgentBootstrapSchemaId,1,fields};
  if(version==2)return {kCatalogAgentBootstrapSchemaId,2,startup_fields};
  return {};
}
inline bool ValidCatalogAgentBootstrapDefinition(const CatalogAgentBootstrapDefinition& r) {
  if(r.startup_verification){
    if(r.kind!=AgentBootstrapDefinitionKind::policy||r.agent_type!="agent_coordinator")return false;
    const auto& v=*r.startup_verification;
    if(!v.verification_levels||(v.verification_levels&~u64{1023}))return false;
    const std::array ids{v.repair_policy_uuid,v.quarantine_policy_uuid,v.salvage_policy_uuid,v.evidence_policy_uuid};
    for(std::size_t i=0;i<ids.size();++i){
      if(!uuid::IsEngineIdentityUuid(ids[i]))return false;
      for(const auto& excluded:{r.definition_uuid,r.database_uuid,r.policy_uuid,r.origin_transaction_uuid.value})
        if(ids[i]==excluded)return false;
      for(std::size_t j=0;j<i;++j)if(ids[j]==ids[i])return false;
    }
  }
  return uuid::IsEngineIdentityUuid(r.definition_uuid) && uuid::IsEngineIdentityUuid(r.database_uuid) &&
      uuid::IsEngineIdentityUuid(r.policy_uuid) &&
      r.origin_transaction_uuid.kind == UuidKind::transaction &&
      uuid::IsEngineIdentityUuid(r.origin_transaction_uuid.value) &&
      r.origin_local_transaction_id && r.generation && !r.agent_type.empty() && !r.properties.empty() &&
      (r.kind == AgentBootstrapDefinitionKind::registration ||
       (r.kind == AgentBootstrapDefinitionKind::policy && r.policy_uuid == r.definition_uuid));
}
inline CatalogValueEncodeResult EncodeCatalogAgentBootstrapDefinition(const CatalogAgentBootstrapDefinition& r) {
  if (!ValidCatalogAgentBootstrapDefinition(r)) return {CatalogValueError::invalid_value, {}};
  const auto view = CatalogAgentBootstrapSchemaView(r.startup_verification?2:1);
  const CatalogValueSchema schema{view.id, view.version, {view.fields.begin(), view.fields.end()}};
  std::vector<CatalogValueField> fields{
      {1, TypedUuid{UuidKind::object, r.definition_uuid}},
      {2, TypedUuid{UuidKind::database, r.database_uuid}},
      {3, static_cast<u64>(r.kind)}, {4, std::string(r.agent_type)}, {5, std::string(r.properties)},
      {6, TypedUuid{UuidKind::object, r.policy_uuid}}, {7, r.origin_transaction_uuid},
      {8, r.origin_local_transaction_id}, {9, r.generation}};
  if(r.startup_verification){const auto& v=*r.startup_verification;
    fields.push_back({10,TypedUuid{UuidKind::object,v.repair_policy_uuid}});
    fields.push_back({11,TypedUuid{UuidKind::object,v.quarantine_policy_uuid}});
    fields.push_back({12,TypedUuid{UuidKind::object,v.salvage_policy_uuid}});
    fields.push_back({13,TypedUuid{UuidKind::object,v.evidence_policy_uuid}});
    fields.push_back({14,v.verification_levels});
  }
  return EncodeCatalogValueBlock(schema,fields);
}
inline CatalogAgentBootstrapDefinitionResult DecodeCatalogAgentBootstrapDefinition(std::string_view bytes) {
  if(bytes.size()<kCatalogValueBlockHeaderBytes)return {CatalogValueError::invalid_value,{}};
  const auto version=platform::LoadLittle16(reinterpret_cast<const byte*>(bytes.data())+20);
  if(version!=1&&version!=2)return {CatalogValueError::invalid_value,{}};
  std::array<CatalogValueFieldView, 14> fields;
  const auto decoded = DecodeCatalogValueBlockInto(CatalogAgentBootstrapSchemaView(version),
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, fields);
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogAgentBootstrapDefinition r;
  if(version==2)r.startup_verification.emplace();
  for (const auto& f : decoded.fields) {
    switch (f.id) {
      case 1: r.definition_uuid = f.identity()->value; break;
      case 2: r.database_uuid = f.identity()->value; break;
      case 3: r.kind = static_cast<AgentBootstrapDefinitionKind>(*f.unsigned_value()); break;
      case 4: r.agent_type = {reinterpret_cast<const char*>(f.bytes.data()), f.bytes.size()}; break;
      case 5: r.properties = {reinterpret_cast<const char*>(f.bytes.data()), f.bytes.size()}; break;
      case 6: r.policy_uuid = f.identity()->value; break;
      case 7: r.origin_transaction_uuid = *f.identity(); break;
      case 8: r.origin_local_transaction_id = *f.unsigned_value(); break;
      case 9: r.generation = *f.unsigned_value(); break;
      case 10:r.startup_verification->repair_policy_uuid=f.identity()->value;break;
      case 11:r.startup_verification->quarantine_policy_uuid=f.identity()->value;break;
      case 12:r.startup_verification->salvage_policy_uuid=f.identity()->value;break;
      case 13:r.startup_verification->evidence_policy_uuid=f.identity()->value;break;
      case 14:r.startup_verification->verification_levels=*f.unsigned_value();break;
    }
  }
  if (!ValidCatalogAgentBootstrapDefinition(r)) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, r};
}
inline bool IsCatalogAgentBootstrapPayload(std::string_view bytes) {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0, 4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data()) + 16) == kCatalogAgentBootstrapSchemaId;
}
inline bool CatalogAgentBootstrapMatchesHeader(const CatalogTypedRecordView& record) {
  const auto decoded = DecodeCatalogAgentBootstrapDefinition(record.payload);
  if (!decoded.ok()) return false;
  const auto expected = decoded.record->kind == AgentBootstrapDefinitionKind::policy
      ? CatalogRecordKind::policy : CatalogRecordKind::config_profile;
  return record.header.kind == expected && record.header.object_uuid.kind == UuidKind::object &&
      record.header.object_uuid.value == decoded.record->definition_uuid;
}
}  // namespace scratchbird::core::catalog
