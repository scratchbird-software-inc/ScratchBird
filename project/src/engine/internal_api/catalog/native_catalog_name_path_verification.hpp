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
// SB_NATIVE_NAME_PATH: the lookup the path rule uses, over a capture and
// without allocating: `order` indexes the live rows by object UUID, `parents`
// the live entries marked default_for_language by (object, language). Built
// once by a verifier; shared by the path and name-tree verifiers.
struct NativeCatalogNamePathLookup {
  std::span<const storage::database::NativeCapturedCatalogRow> rows;
  std::span<std::size_t> order,by_language;
  // False with `duplicate` set when an object has two live entries marked
  // default_for_language in one language (owner Q43 = 1: refused, not picked
  // between); false with `workspace` set when the spans do not fit.
  static bool Build(const storage::database::NativeCatalogCapture& capture,NativeCatalogNamespaceWorkspace workspace,
      NativeCatalogNamePathLookup& out,bool& workspace_short,core::platform::Uuid& duplicate) noexcept {
    namespace c=core::catalog;namespace ranges=storage::disk::detail;
    workspace_short=false;duplicate={};
    const auto rows=capture.rows();const auto count=rows.size();
    if(workspace.order.size()<count||workspace.parents.size()<count||
        !ranges::DisjointNativeDecodeRegions(workspace.order,workspace.parents)||
        !capture.DisjointWorkspace(std::as_bytes(workspace.order))||
        !capture.DisjointWorkspace(std::as_bytes(workspace.parents))){workspace_short=true;return false;}
    out.rows=rows;
    const auto identity=[&](std::size_t i){return rows[i].row.metadata.record.header.object_uuid.value;};
    out.order=workspace.order.first(count);std::iota(out.order.begin(),out.order.end(),std::size_t{0});
    std::sort(out.order.begin(),out.order.end(),[&](auto a,auto b){return identity(a)<identity(b);});
    std::size_t defaults=0;
    for(std::size_t i=0;i<count;++i)if(const auto* e=out.Entry(i);e&&e->default_for_language)workspace.parents[defaults++]=i;
    out.by_language=workspace.parents.first(defaults);
    std::sort(out.by_language.begin(),out.by_language.end(),[&](auto a,auto b){return out.LanguageKey(a)<out.LanguageKey(b);});
    for(std::size_t i=1;i<out.by_language.size();++i)if(out.LanguageKey(out.by_language[i-1])==out.LanguageKey(out.by_language[i])){
      duplicate=out.Entry(out.by_language[i])->name_entry_uuid.value;return false;}
    return true;
  }
  // The live name entry captured row i holds, or nullptr.
  const core::catalog::CatalogNameEntryView* Entry(std::size_t i) const noexcept {
    const auto& row=rows[i].row;
    if(row.metadata.record.header.deleted||!row.name_payload)return nullptr;
    return std::get_if<core::catalog::CatalogNameEntryView>(&*row.name_payload);
  }
  std::pair<core::platform::Uuid,core::platform::Uuid> LanguageKey(std::size_t i) const noexcept {
    const auto* e=Entry(i);return {e->object_uuid.value,e->language_uuid.value};
  }
  // The live row with this object identity, or nullptr.
  const storage::database::NativeCatalogVisibleRow* Find(const core::platform::Uuid& id) const noexcept {
    const auto identity=[&](std::size_t i){return rows[i].row.metadata.record.header.object_uuid.value;};
    const auto at=std::lower_bound(order.begin(),order.end(),id,[&](auto i,const auto& value){return identity(i)<value;});
    if(at==order.end()||identity(*at)!=id||rows[*at].row.metadata.record.header.deleted)return nullptr;
    return &rows[*at].row;
  }
  core::catalog::CatalogNamePathName operator()(const core::platform::Uuid& object,const core::platform::Uuid& language) const noexcept {
    core::catalog::CatalogNamePathName name;
    const std::pair key{object,language};
    const auto at=std::lower_bound(by_language.begin(),by_language.end(),key,[&](auto i,const auto& value){return LanguageKey(i)<value;});
    if(at!=by_language.end()&&LanguageKey(*at)==key){name.name=*Entry(*at);return name;}
    const auto* definition=Find(object);
    if(!definition||definition->name_payload)return name;
    const auto* entry=Find(definition->metadata.default_name_uuid.value);
    if(!entry||!entry->name_payload)return name;
    if(const auto* e=std::get_if<core::catalog::CatalogNameEntryView>(&*entry->name_payload))name.name=*e;
    return name;
  }
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
  namespace c=core::catalog;
  using E=NativeCatalogNamePathVerificationError;
  NativeCatalogNamePathVerification out;
  const auto fail=[&](E error,core::platform::Uuid entry={}){out.error=error;out.entry_uuid=entry;out.entries=0;return out;};
  if(!capture.ok())return out;
  NativeCatalogNamePathLookup lookup;bool workspace_short=false;core::platform::Uuid duplicate;
  if(!NativeCatalogNamePathLookup::Build(capture,workspace,lookup,workspace_short,duplicate))
    return workspace_short?fail(E::workspace_required):fail(E::duplicate_language_default,duplicate);
  for(std::size_t i=0;i<lookup.rows.size();++i){
    const auto* e=lookup.Entry(i);if(!e)continue;
    ++out.entries;
    switch(c::CheckCatalogNameEntryPath(*e,lookup)){
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
