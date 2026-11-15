// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_identifier_lookup_key.hpp"
#include "../datatypes/canonical_utf8.hpp"
#include <algorithm>
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
// SEARCH_KEY: SB_NATIVE_NAME_ROW_V4 (owner Q147 = 1). The lanes of a stored
// entry, derived: the folds Value() applies, per lane, as a fold of the raw
// text. normalized: the unquoted fold; exact: the raw byte unless the name was
// quoted under a profile that does not preserve quoted case.
std::vector<byte> CatalogNameDerivedText::Bytes() const {
  std::vector<byte> out(size());
  for(std::size_t i=0;i<out.size();++i)out[i]=(*this)[i];
  return out;
}
std::string CatalogNameDerivedText::String() const {
  std::string out(size(),'\0');
  for(std::size_t i=0;i<out.size();++i)out[i]=static_cast<char>((*this)[i]);
  return out;
}
bool CatalogNameDerivedText::Equals(std::span<const byte> bytes) const noexcept {
  if(bytes.size()!=size())return false;
  for(std::size_t i=0;i<bytes.size();++i)if(bytes[i]!=(*this)[i])return false;
  return true;
}
int CompareCatalogNameDerivedText(const CatalogNameDerivedText& x,const CatalogNameDerivedText& y) noexcept {
  const auto n=std::min(x.size(),y.size());
  for(std::size_t i=0;i<n;++i){const auto a=x[i],b=y[i];if(a!=b)return a<b?-1:1;}
  return x.size()==y.size()?0:(x.size()<y.size()?-1:1);
}
CatalogNameEntryKeys DeriveCatalogNameEntryKeys(
    const CatalogIdentifierResolutionProfile& profile,const CatalogNameEntryView& entry) noexcept {
  CatalogNameEntryKeys out;
  const CatalogIdentifierKeyRequest request{&profile,entry.raw_name_text,entry.was_quoted,entry.quote_style,entry.requires_exact_match};
  if(const auto error=Admit(request);error!=E::none){out.error=error;return out;}
  if(entry.identifier_profile_uuid.kind!=UuidKind::object||entry.identifier_profile_uuid.value!=profile.profile_uuid){
    out.error=E::invalid_profile;return out;}
  if(entry.case_fold_profile_uuid||entry.quoted_identifier_profile_uuid){out.error=E::resource_admission_required;return out;}
  const auto fold=profile.unquoted_fold_mode;
  // Admit refuses the resource folds: only none, upper_ascii, lower_ascii remain.
  if(fold!=IdentifierFoldMode::none&&fold!=IdentifierFoldMode::upper_ascii&&fold!=IdentifierFoldMode::lower_ascii){
    out.error=E::resource_admission_required;return out;}
  out.normalized={entry.raw_name_text,fold};
  out.exact={entry.raw_name_text,(!entry.was_quoted||profile.quoted_preserves_case)?IdentifierFoldMode::none:fold};
  out.own=entry.requires_exact_match?out.exact:out.normalized;
  // Display (owner Q147: display = raw under preserve_user_text). The
  // store_folded_display mode displays the own lane (the exact lane for an
  // exact-match name, else the normalized one), the form version 3's verifier
  // required of a stored display; donor_projection was refused by Admit.
  if(profile.storage_display_mode==IdentifierDisplayMode::preserve_user_text)out.display={entry.raw_name_text,IdentifierFoldMode::none};
  else if(profile.storage_display_mode==IdentifierDisplayMode::store_folded_display)out.display=out.own;
  else{out.error=E::display_mismatch;return out;}
  out.error=E::none;return out;
}
CatalogIdentifierKeyError CheckCatalogIdentifierLookupKeys(
    const CatalogIdentifierResolutionProfile& profile,const CatalogNameEntryView& entry) noexcept {
  return DeriveCatalogNameEntryKeys(profile,entry).error;
}
} // namespace scratchbird::core::catalog
