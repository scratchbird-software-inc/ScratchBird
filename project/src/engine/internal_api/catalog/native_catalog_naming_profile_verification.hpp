// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "catalog_identifier_profile.hpp"
#include "catalog_identifier_lookup_key.hpp"
#include "catalog_naming_dialect_profile.hpp"
#include <numeric>

namespace scratchbird::engine::internal_api {
enum class NativeCatalogNamingProfileError {
  none,source_required,workspace_required,duplicate_identity,provisional_row,
  missing_identifier,invalid_identifier,missing_dialect,invalid_dialect,
  identifier_generation,mode_flags,entry_binding,unsupported_supplement,lookup_key
};
struct NativeCatalogNamingProfileVerification {
  NativeCatalogNamingProfileError error=NativeCatalogNamingProfileError::source_required;
  core::platform::Uuid object_uuid,referenced_uuid;
  core::catalog::CatalogIdentifierKeyError lookup_error=core::catalog::CatalogIdentifierKeyError::none;
  std::size_t identifiers=0,dialects=0,entries=0;
  bool ok()const noexcept{return error==NativeCatalogNamingProfileError::none;}
};
// Structural reference closure against the complete selected native catalog.
// Exact/ASCII component keys are checked against the actual selected profile.
// Resource admission, collision checks and executable dialect flag meanings
// remain obligations of the owning resolver. Supplemental
// fold/quoted families currently refuse explicitly instead of becoming defaults.
inline NativeCatalogNamingProfileVerification VerifyNativeCatalogNamingProfileReferences(
    const storage::database::NativeCatalogCapture& capture,std::span<std::size_t> scratch) noexcept {
  namespace c=core::catalog;using E=NativeCatalogNamingProfileError;using K=c::CatalogRecordKind;
  NativeCatalogNamingProfileVerification out;
  const auto fail=[&](E error,core::platform::Uuid object={},core::platform::Uuid reference={},
      c::CatalogIdentifierKeyError key_error=c::CatalogIdentifierKeyError::none){
    out.error=error;out.object_uuid=object;out.referenced_uuid=reference;
    out.lookup_error=key_error;
    out.identifiers=out.dialects=out.entries=0;return out;
  };
  if(!capture.ok())return out;
  const auto rows=capture.rows();
  if(scratch.size()<rows.size()||!storage::disk::detail::DecodedRange(scratch).valid||
      !capture.DisjointWorkspace(std::as_bytes(scratch)))return fail(E::workspace_required);
  auto order=scratch.first(rows.size());std::iota(order.begin(),order.end(),std::size_t{0});
  const auto identity=[&](std::size_t i){return rows[i].row.metadata.record.header.object_uuid.value;};
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return identity(a)<identity(b);});
  for(std::size_t i=1;i<order.size();++i)if(identity(order[i-1])==identity(order[i]))
    return fail(E::duplicate_identity,identity(order[i]));
  const auto find=[&](const core::platform::Uuid& id)->const storage::database::NativeCapturedCatalogRow*{
    const auto at=std::lower_bound(order.begin(),order.end(),id,[&](auto i,const auto& value){return identity(i)<value;});
    return at==order.end()||identity(*at)!=id||rows[*at].row.metadata.record.header.deleted?nullptr:&rows[*at];
  };
  const auto available=[](const storage::database::NativeCapturedCatalogRow& row){
    const auto& m=row.row.metadata;
    return m.status==c::CatalogObjectStatus::active&&m.lifecycle==c::CatalogObjectLifecycle::active;
  };
  const auto db=capture.source()->database_uuid;
  const auto identifier=[&](const storage::database::NativeCapturedCatalogRow& row){
    return row.role==5&&!row.row.provisional&&row.row.metadata.record.header.kind==K::config_profile&&
        row.row.metadata.object_subtype=="identifier_resolution_profile"&&
        c::CatalogIdentifierProfileMatchesMetadata(row.row.metadata)&&c::CatalogIdentifierProfileMatchesDatabase(row.row.metadata,db);
  };
  const auto dialect=[&](const storage::database::NativeCapturedCatalogRow& row){
    return row.role==5&&!row.row.provisional&&row.row.metadata.record.header.kind==K::config_profile&&
        row.row.metadata.object_subtype=="naming_dialect_profile"&&
        c::CatalogNamingDialectProfileMatchesMetadata(row.row.metadata)&&c::CatalogNamingDialectProfileMatchesDatabase(row.row.metadata,db);
  };
  for(const auto& captured:rows){
    const auto& m=captured.row.metadata;const auto id=m.record.header.object_uuid.value;
    if(captured.row.provisional)return fail(E::provisional_row,id);
    if(m.record.header.deleted)continue;
    if(m.object_subtype=="identifier_resolution_profile"||c::IsCatalogIdentifierProfilePayload(m.record.payload)){
      if(!identifier(captured))return fail(E::invalid_identifier,id);++out.identifiers;
    }
    if(m.object_subtype=="naming_dialect_profile"||c::IsCatalogNamingDialectProfilePayload(m.record.payload)){
      if(!dialect(captured))return fail(E::invalid_dialect,id);
      const auto profile=*c::DecodeCatalogNamingDialectProfile(m.record.payload).record;
      const auto* referenced=find(profile.identifier_profile_uuid);
      if(!referenced)return fail(E::missing_identifier,id,profile.identifier_profile_uuid);
      if(!identifier(*referenced))return fail(E::invalid_identifier,id,profile.identifier_profile_uuid);
      const auto value=*c::DecodeCatalogIdentifierProfile(referenced->row.metadata.record.payload).record;
      if(value.generation!=profile.identifier_profile_generation)return fail(E::identifier_generation,id,profile.identifier_profile_uuid);
      if(value.dialect_mode_flags&~profile.allowed_dialect_mode_flags)return fail(E::mode_flags,id,profile.identifier_profile_uuid);
      ++out.dialects;
    }
  }
  for(const auto& captured:rows){
    const auto& row=captured.row;if(row.metadata.record.header.deleted||!row.name_payload)continue;
    const auto* entry=std::get_if<c::CatalogNameEntryView>(&*row.name_payload);if(!entry)continue;
    const auto id=row.metadata.record.header.object_uuid.value;
    const auto* selected=find(entry->dialect_profile_uuid.value);
    if(!selected)return fail(E::missing_dialect,id,entry->dialect_profile_uuid.value);
    if(!dialect(*selected)||!available(*selected))return fail(E::invalid_dialect,id,entry->dialect_profile_uuid.value);
    const auto profile=*c::DecodeCatalogNamingDialectProfile(selected->row.metadata.record.payload).record;
    if(entry->identifier_profile_uuid.value!=profile.identifier_profile_uuid)
      return fail(E::entry_binding,id,entry->identifier_profile_uuid.value);
    const auto* definition=find(profile.identifier_profile_uuid);
    if(!definition)return fail(E::missing_identifier,id,profile.identifier_profile_uuid);
    if(!identifier(*definition)||!available(*definition))return fail(E::invalid_identifier,id,profile.identifier_profile_uuid);
    if(entry->case_fold_profile_uuid||entry->quoted_identifier_profile_uuid)
      return fail(E::unsupported_supplement,id,entry->case_fold_profile_uuid?
          entry->case_fold_profile_uuid->value:entry->quoted_identifier_profile_uuid->value);
    const auto key_error=c::CheckCatalogIdentifierLookupKeys(
        *c::DecodeCatalogIdentifierProfile(definition->row.metadata.record.payload).record,*entry);
    if(key_error!=c::CatalogIdentifierKeyError::none)return fail(E::lookup_key,id,profile.identifier_profile_uuid,key_error);
    ++out.entries;
  }
  out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
