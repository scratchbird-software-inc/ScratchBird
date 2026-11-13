// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_catalog_cohort_capture.hpp"
#include "native_decoded_storage_ranges.hpp"
#include "catalog_comment_record_codec.hpp"
#include <algorithm>
#include <numeric>

namespace scratchbird::engine::internal_api {
enum class NativeCatalogCommentVerificationError {
  none,source_required,workspace_required,duplicate_identity,provisional_row,invalid_role,
  invalid_comment,missing_target,duplicate_comment
};
struct NativeCatalogCommentVerification {
  NativeCatalogCommentVerificationError error=NativeCatalogCommentVerificationError::source_required;
  core::platform::Uuid object_uuid,referenced_uuid;
  std::size_t comments=0;
  bool ok() const noexcept{return error==NativeCatalogCommentVerificationError::none;}
};
// SEARCH_KEY: SB_NATIVE_COMMENT_RELATION (owner Q77 = 2, Q79 = 1, Q81 = 1,
// Q82 = 1; 2026-11-13). One semantic component of startup verification, over
// the captured (visible) rows. The comments relation (role 7) holds exactly the
// localized-comment rows (the name verifier refuses a comment row elsewhere or
// another row there), and each live one:
//   - is the typed schema-65678 record under subtype localized_comment, with no
//     name (CatalogCommentMatchesMetadata: the comment UUID is the envelope's
//     object, the target its parent, the language a catalog_language constant,
//     the text UTF-8 and within the maximum);
//   - comments on an object that exists: a live definition row that is not a
//     name row or another comment (as a name's vector and entries must name a
//     live definition);
//   - is the only live comment for its (target, language) (owner Q82 = 1).
// Deleted rows (a dropped comment) are skipped. The comment tree's cells are
// bound to their rows by the storage reader (SB_NATIVE_COMMENT_TREE).
// Heap-free; `scratch` holds one index per captured row.
inline NativeCatalogCommentVerification VerifyNativeCatalogComments(
    const storage::database::NativeCatalogCapture& capture,std::span<std::size_t> scratch) noexcept {
  namespace c=core::catalog;using E=NativeCatalogCommentVerificationError;
  NativeCatalogCommentVerification out;
  const auto fail=[&](E error,core::platform::Uuid object={},core::platform::Uuid reference={}){
    out.error=error;out.object_uuid=object;out.referenced_uuid=reference;out.comments=0;return out;};
  if(!capture.ok())return out;
  const auto rows=capture.rows();
  if(scratch.size()<rows.size()||!storage::disk::detail::DecodedRange(scratch).valid||
      !capture.DisjointWorkspace(std::as_bytes(scratch)))
    return fail(E::workspace_required);
  auto order=scratch.first(rows.size());std::iota(order.begin(),order.end(),std::size_t{0});
  const auto identity=[&](std::size_t i){return rows[i].row.metadata.record.header.object_uuid.value;};
  std::sort(order.begin(),order.end(),[&](auto a,auto b){return identity(a)<identity(b);});
  for(std::size_t i=1;i<order.size();++i)if(identity(order[i-1])==identity(order[i]))
    return fail(E::duplicate_identity,identity(order[i]));
  const auto find=[&](const core::platform::Uuid& id)->const storage::database::NativeCapturedCatalogRow*{
    const auto at=std::lower_bound(order.begin(),order.end(),id,[&](auto i,const auto& value){return identity(i)<value;});
    if(at==order.end()||identity(*at)!=id||rows[*at].row.metadata.record.header.deleted)return nullptr;
    return &rows[*at];
  };
  const auto live_comment=[&](std::size_t i){
    const auto& m=rows[i].row.metadata;
    return !m.record.header.deleted&&m.record.header.kind==c::CatalogRecordKind::localized_comment;
  };
  for(std::size_t i=0;i<rows.size();++i){
    const auto& captured=rows[i];const auto& m=captured.row.metadata;const auto id=identity(i);
    if(captured.row.provisional)return fail(E::provisional_row,id);
    const bool comment=m.record.header.kind==c::CatalogRecordKind::localized_comment;
    if(comment!=(captured.role==storage::page::kNativeCommentRelationRole)||(comment&&captured.row.name_payload))
      return fail(E::invalid_role,id);
    if(!live_comment(i))continue;
    if(!c::CatalogCommentMatchesMetadata(m))return fail(E::invalid_comment,id);
    const auto record=c::DecodeCatalogComment(m.record.payload);
    const auto* target=find(record.record->target_object_uuid);
    if(!target||target->row.name_payload||
        target->row.metadata.record.header.kind==c::CatalogRecordKind::localized_comment)
      return fail(E::missing_target,id,record.record->target_object_uuid);
    ++out.comments;
  }
  // (target, language) unique among the live comments: the live comments in
  // key order, then adjacent keys compared.
  std::size_t count=0;
  for(std::size_t i=0;i<rows.size();++i)if(live_comment(i))order[count++]=i;
  const auto key=[&](std::size_t i){
    const auto r=c::DecodeCatalogComment(rows[i].row.metadata.record.payload);
    return c::CatalogCommentTreeKey(r.record->target_object_uuid,r.record->language_uuid);
  };
  auto comments=order.first(count);
  std::sort(comments.begin(),comments.end(),[&](auto a,auto b){return key(a)<key(b);});
  for(std::size_t i=1;i<comments.size();++i)
    if(key(comments[i-1])==key(comments[i]))return fail(E::duplicate_comment,identity(comments[i]),identity(comments[i-1]));
  out.error=E::none;return out;
}
} // namespace scratchbird::engine::internal_api
