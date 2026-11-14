// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_identifier_lookup_key.hpp"
#include "../datatypes/canonical_utf8.hpp"
#include <limits>

namespace scratchbird::core::catalog {
// SEARCH_KEY: SB_CATALOG_IDENTIFIER_TEXT_RULE (owner Q122 = 1, 2026-11-14).
// The one copy of the profile-independent text rule is
// AdmitCatalogIdentifierText in catalog_identifier_text_rule.hpp (moved
// there unchanged, header-only, for Q128 = 1); Admit below applies it after
// the profile checks, and callers that must refuse a name before any profile
// exists (native CREATE DATABASE, the SBLR create descriptor codec) call it
// directly.
// SEARCH_KEY: SB_CATALOG_IDENTIFIER_PROFILE_LENGTH_RULE (owner Q127 = 3).
CatalogIdentifierKeyError AdmitCatalogIdentifierProfileLength(std::string_view text,u64 scalar_count,
    u64 maximum_identifier_length,IdentifierLengthUnit length_unit) noexcept {
  if(maximum_identifier_length&&
      (length_unit==IdentifierLengthUnit::encoded_octets?text.size():scalar_count)>maximum_identifier_length)
    return CatalogIdentifierKeyError::identifier_too_long;
  return CatalogIdentifierKeyError::none;
}
namespace {
using E=CatalogIdentifierKeyError;
E Admit(const CatalogIdentifierKeyRequest& r) noexcept {
  if(!r.profile||!CatalogIdentifierProfileEncodedBytes(*r.profile))return E::invalid_profile;
  const auto& p=*r.profile;
  if(p.dialect_mode_flags)return E::dialect_admission_required;
  if(p.resources||p.platform||p.unquoted_fold_mode==IdentifierFoldMode::resource_upper||
      p.unquoted_fold_mode==IdentifierFoldMode::resource_lower||p.comparison_mode==IdentifierComparisonMode::case_insensitive||
      p.comparison_mode==IdentifierComparisonMode::profile_platform||p.storage_display_mode==IdentifierDisplayMode::donor_projection)
    return E::resource_admission_required;
  u64 scalars=0;
  if(const auto text=AdmitCatalogIdentifierText(r.text,&scalars);text!=E::none)return text;
  // Q127: the same rule, now one function (was written inline here).
  if(const auto length=AdmitCatalogIdentifierProfileLength(r.text,scalars,p.maximum_identifier_length,p.length_unit);
      length!=E::none)return length;
  if(!r.was_quoted)return r.quote_style==CatalogNameQuoteStyle::none?E::none:E::invalid_quote_mode;
  if(!r.requires_exact_match)return E::invalid_quote_mode;
  u64 mask=0;
  switch(r.quote_style){
    case CatalogNameQuoteStyle::double_quote:mask=1;break;
    case CatalogNameQuoteStyle::backtick:mask=2;break;
    case CatalogNameQuoteStyle::bracket:mask=4;break;
    case CatalogNameQuoteStyle::donor_api:mask=8;break;
    default:return E::invalid_quote_mode;
  }
  return p.quote_styles&mask?E::none:E::invalid_quote_mode;
}
byte Fold(byte b,IdentifierFoldMode mode) noexcept {
  if(mode==IdentifierFoldMode::upper_ascii&&b>='a'&&b<='z')return b-('a'-'A');
  if(mode==IdentifierFoldMode::lower_ascii&&b>='A'&&b<='Z')return b+('a'-'A');
  return b;
}
byte Value(const CatalogIdentifierKeyRequest& r,std::size_t at,bool exact) noexcept {
  const auto b=static_cast<byte>(r.text[at]);
  return exact&&(!r.was_quoted||r.profile->quoted_preserves_case)?b:Fold(b,r.profile->unquoted_fold_mode);
}
bool Matches(const CatalogIdentifierKeyRequest& r,std::span<const byte> stored,bool exact) noexcept {
  if(stored.size()!=r.text.size()||(!stored.data()&&stored.size()))return false;
  for(std::size_t i=0;i<stored.size();++i)if(stored[i]!=Value(r,i,exact))return false;
  return true;
}
bool Overlap(std::span<byte> output,const void* input,std::size_t size) noexcept {
  const auto a=reinterpret_cast<std::uintptr_t>(output.data()),b=reinterpret_cast<std::uintptr_t>(input);
  if((output.size()&&!a)||(size&&!b)||output.size()>UINTPTR_MAX-a||size>UINTPTR_MAX-b)return true;
  return output.size()&&size&&a<b+size&&b<a+output.size();
}
}
CatalogIdentifierKeyResult DeriveCatalogIdentifierLookupKeysInto(
    const CatalogIdentifierKeyRequest& request,std::span<byte> output) noexcept {
  const auto error=Admit(request);if(error!=E::none)return {error,{},{}};
  const auto& p=*request.profile;
  // 3b: the profile carries no borrowed name text any more.
  if(Overlap(output,&request,sizeof(request))||Overlap(output,&p,sizeof(p))||
      Overlap(output,request.text.data(),request.text.size()))
    return {E::invalid_backing,{},{}};
  const auto size=request.text.size();if(output.size()/2<size)return {E::size_limit,{},{}};
  for(std::size_t i=0;i<size;++i){output[i]=Value(request,i,false);output[size+i]=Value(request,i,true);}
  return {E::none,output.first(size),output.subspan(size,size)};
}
CatalogIdentifierKeyError CheckCatalogIdentifierLookupKeys(
    const CatalogIdentifierResolutionProfile& profile,const CatalogNameEntryView& entry) noexcept {
  const CatalogIdentifierKeyRequest request{&profile,entry.raw_name_text,entry.was_quoted,entry.quote_style,entry.requires_exact_match};
  const auto error=Admit(request);if(error!=E::none)return error;
  if(entry.identifier_profile_uuid.kind!=UuidKind::object||entry.identifier_profile_uuid.value!=profile.profile_uuid)return E::invalid_profile;
  if(entry.case_fold_profile_uuid||entry.quoted_identifier_profile_uuid)return E::resource_admission_required;
  if((!entry.requires_exact_match||!entry.normalized_lookup_key.empty())&&!Matches(request,entry.normalized_lookup_key,false))return E::key_mismatch;
  if((entry.requires_exact_match||!entry.exact_lookup_key.empty())&&!Matches(request,entry.exact_lookup_key,true))return E::key_mismatch;
  if(!entry.display_name.data()&&!entry.display_name.empty())return E::display_mismatch;
  const auto display=std::span<const byte>(reinterpret_cast<const byte*>(entry.display_name.data()),entry.display_name.size());
  if(profile.storage_display_mode==IdentifierDisplayMode::preserve_user_text){
    if(entry.display_name!=entry.raw_name_text)return E::display_mismatch;
  }else if(!Matches(request,display,entry.requires_exact_match))return E::display_mismatch;
  return E::none;
}
} // namespace scratchbird::core::catalog
