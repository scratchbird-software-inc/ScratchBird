// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_namespace_verification.hpp"
#include "catalog_name_path.hpp"
#include <numeric>

namespace scratchbird::engine::internal_api {
enum class NativeCatalogNamePathVerificationError {
  none,source_required,workspace_required,duplicate_language_default,
  unnamed_scope,ambiguous_scope,cycle,too_deep,path_mismatch
};
struct NativeCatalogNamePathVerification {
  NativeCatalogNamePathVerificationError error=NativeCatalogNamePathVerificationError::source_required;
  core::platform::Uuid entry_uuid;
  std::size_t entries=0;
  bool ok() const noexcept{return error==NativeCatalogNamePathVerificationError::none;}
};
// SB_NATIVE_NAME_PATH: every live name entry's stored full path (lookup key and
// component count) is the one the path rule resolves from the captured names.
// An object with two live entries marked default_for_language in one language
// is refused (the rule would have to pick). Allocation-free: `order` indexes the
// rows by object UUID, `parents` the language-default entries by (object,
// language); `colors` is not used. Runs after VerifyNativeCatalogNameBindings,
// whose object/vector/default bindings the fallback to a default entry relies on.
inline NativeCatalogNamePathVerification VerifyNativeCatalogNamePaths(
    const storage::database::NativeCatalogCapture& capture,NativeCatalogNamespaceWorkspace workspace) noexcept {
  namespace c=core::catalog;namespace ranges=storage::disk::detail;
  using E=NativeCatalogNamePathVerificationError;
  NativeCatalogNamePathVerification out;
  const auto fail=[&](E error,core::platform::Uuid entry={}){out.error=error;out.entry_uuid=entry;out.entries=0;return out;};
  if(!capture.ok())return out;
  const auto rows=capture.rows();const auto count=rows.size();
  if(workspace.order.size()<count||workspace.parents.size()<count||
      !ranges::DisjointNativeDecodeRegions(workspace.order,workspace.parents)||
      !capture.DisjointWorkspace(std::as_bytes(workspace.order))||
      !capture.DisjointWorkspace(std::as_bytes(workspace.parents)))return fail(E::workspace_required);
  const auto identity=[&](std::size_t i){return rows[i].row.metadata.record.header.object_uuid.value;};
  auto order=workspace.order.first(count);std::iota(order.begin(),order.end(),std::size_t{0});
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return identity(a)<identity(b);});
  const auto find=[&](const core::platform::Uuid& id)->const storage::database::NativeCatalogVisibleRow*{
    const auto at=std::lower_bound(order.begin(),order.end(),id,[&](auto i,const auto& value){return identity(i)<value;});
    if(at==order.end()||identity(*at)!=id||rows[*at].row.metadata.record.header.deleted)return nullptr;
    return &rows[*at].row;
  };
  const auto entry_of=[&](std::size_t i)->const c::CatalogNameEntryView*{
    const auto& row=rows[i].row;
    if(row.metadata.record.header.deleted||!row.name_payload)return nullptr;
    return std::get_if<c::CatalogNameEntryView>(&*row.name_payload);
  };
  std::size_t defaults=0;
  for(std::size_t i=0;i<count;++i)if(const auto* e=entry_of(i);e&&e->default_for_language)workspace.parents[defaults++]=i;
  auto by_language=workspace.parents.first(defaults);
  const auto language_key=[&](std::size_t i){const auto* e=entry_of(i);return std::pair{e->object_uuid.value,e->language_tag};};
  std::sort(by_language.begin(),by_language.end(),[&](auto a,auto b){return language_key(a)<language_key(b);});
  for(std::size_t i=1;i<by_language.size();++i)if(language_key(by_language[i-1])==language_key(by_language[i]))
    return fail(E::duplicate_language_default,entry_of(by_language[i])->name_entry_uuid.value);
  const auto path_name=[&](const core::platform::Uuid& object,std::string_view language){
    c::CatalogNamePathName name;
    const std::pair key{object,language};
    const auto at=std::lower_bound(by_language.begin(),by_language.end(),key,[&](auto i,const auto& value){return language_key(i)<value;});
    if(at!=by_language.end()&&language_key(*at)==key){name.name=*entry_of(*at);return name;}
    const auto* definition=find(object);
    if(!definition||definition->name_payload)return name;
    const auto* entry=find(definition->metadata.default_name_uuid.value);
    if(!entry||!entry->name_payload)return name;
    if(const auto* e=std::get_if<c::CatalogNameEntryView>(&*entry->name_payload))name.name=*e;
    return name;
  };
  for(std::size_t i=0;i<count;++i){
    const auto* e=entry_of(i);if(!e)continue;
    ++out.entries;
    switch(c::CheckCatalogNameEntryPath(*e,path_name)){
      case c::CatalogNamePathError::none:break;
      case c::CatalogNamePathError::unnamed_scope:return fail(E::unnamed_scope,e->name_entry_uuid.value);
      case c::CatalogNamePathError::ambiguous_scope:return fail(E::ambiguous_scope,e->name_entry_uuid.value);
      case c::CatalogNamePathError::cycle:return fail(E::cycle,e->name_entry_uuid.value);
      case c::CatalogNamePathError::too_deep:return fail(E::too_deep,e->name_entry_uuid.value);
      case c::CatalogNamePathError::mismatch:return fail(E::path_mismatch,e->name_entry_uuid.value);
    }
  }
  out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
