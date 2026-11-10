// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_index_btree_page.hpp"
#include "hash_digest_parts.hpp"
#include "disk_device.hpp"
#include "../disk/native_decoded_storage_ranges.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <new>
#include <stdexcept>
#include <string_view>
#include <limits>
#include <type_traits>

namespace scratchbird::storage::page {
namespace {
using namespace scratchbird::core::platform;
namespace hash=scratchbird::core::hash;
using E=NativeBtreeError;
constexpr std::size_t seal=512;
bool Zero(const byte* p,std::size_t n){return std::all_of(p,p+n,[](byte b){return b==0;});}
bool V7(const Uuid& u){return !u.is_nil()&&(u.bytes[6]>>4)==7&&(u.bytes[8]&0xc0)==0x80;}
Uuid Get(const byte* p){Uuid u;std::copy_n(p,16,u.bytes.begin());return u;}
void Put(byte* p,const Uuid& u){std::copy(u.bytes.begin(),u.bytes.end(),p);}
NativeBtreePageResult Fail(E e){NativeBtreePageResult r;r.error=e;return r;}
template<class Page> disk::NativePageReference Self(const Page& p){const auto& h=p.header;return {h.filespace_uuid,h.page_number,h.page_generation,h.page_size_profile_uuid};}
bool ValidRef(const disk::NativePageReference& r){return V7(r.filespace_uuid)&&r.page_number&&r.page_generation&&disk::FindCanonicalFilespacePageProfile(r.page_size_profile_uuid);}
bool ValidDependencies(const NativeBtreeDependencies& d){return V7(d.index_uuid)&&d.descriptor_generation&&d.storage_generation&&V7(d.key_profile_uuid)
  &&V7(d.visibility_profile_uuid)&&V7(d.dependency_map_uuid)&&!Zero(d.dependency_map_sha256.data(),32);}
template<class Key> bool ValidKey(const Key& k){return !k.encoded_key.empty()&&V7(k.row_uuid)&&V7(k.version_uuid);}
std::optional<disk::NativePageReference> GetRef(const byte* p){if(Zero(p,48))return {};return disk::NativePageReference{Get(p),LoadLittle64(p+16),LoadLittle64(p+24),Get(p+32)};}
void PutRef(byte* p,const std::optional<disk::NativePageReference>& r){if(!r)return;Put(p,r->filespace_uuid);StoreLittle64(p+16,r->page_number);StoreLittle64(p+24,r->page_generation);Put(p+32,r->page_size_profile_uuid);}
auto Digest(std::span<const byte> b){const std::array<byte,32> zeros{};const hash::HashDigestSegment parts[]={{b.data(),seal},{zeros.data(),32},{b.data()+seal+32,b.size()-seal-32}};return hash::ComputeSha256DigestPartsNative(parts,3);}
struct OwningReferences {
  std::map<Uuid,Uuid> profiles;
  std::set<std::pair<Uuid,u64>> children;
  bool Add(const disk::NativePageReference& r,bool child){
    const auto [it,inserted]=profiles.emplace(r.filespace_uuid,r.page_size_profile_uuid);
    return (inserted||it->second==r.page_size_profile_uuid)&&
      (!child||children.emplace(r.filespace_uuid,r.page_number).second);
  }
};
struct BorrowedReferences {
  std::span<NativeBtreeReferenceSlot> slots;
  explicit BorrowedReferences(std::span<NativeBtreeReferenceSlot> storage):slots(storage){
    std::fill(slots.begin(),slots.end(),NativeBtreeReferenceSlot{});
  }
  bool Insert(const disk::NativePageReference& r,u64 page){
    u64 hash=14695981039346656037ULL;
    for(auto b:r.filespace_uuid.bytes){hash^=b;hash*=1099511628211ULL;}
    for(unsigned i=0;i<8;++i){hash^=(page>>(8*i))&255;hash*=1099511628211ULL;}
    std::size_t at=hash%slots.size();
    for(std::size_t probe=0;probe<slots.size();++probe){auto& entry=slots[at];
      if(entry.filespace_uuid.is_nil()){entry={r.filespace_uuid,r.page_size_profile_uuid,page};return true;}
      if(entry.filespace_uuid==r.filespace_uuid&&entry.page_number==page)
        return !page&&entry.profile_uuid==r.page_size_profile_uuid;
      if(++at==slots.size())at=0;
    }
    return false; // Required capacity exceeds twice the maximum possible entries.
  }
  bool Add(const disk::NativePageReference& r,bool child){
    return Insert(r,0)&&(!child||Insert(r,r.page_number));
  }
};
template<class Page,class References> E Validate(const Page& p,References& references) {
  const auto h=disk::EncodeNativeCommonPageHeader(p.header);
  if(!h.ok()||p.header.page_type<0x200||p.header.page_type>0x202)return E::invalid_header;
  if(p.header.flags&1u)return E::encrypted_requires_crypto_authority;
  if(!ValidDependencies(p.dependencies))return E::invalid_dependencies;
  // SB_NATIVE_BTREE_SYSTEM_PAGE (owner Q30 = 1): a page a later change adds
  // to a catalog tree (a split's new page) is the system's, with no creator:
  // a nil transaction and local number 0, both or neither. Visibility stays
  // with the row versions the cells name, as in FirebirdSQL, whose index
  // pages carry no transaction.
  const bool system=p.creator_transaction_uuid.is_nil()&&!p.creator_local_transaction_id;
  if((!system&&(!V7(p.creator_transaction_uuid)||!p.creator_local_transaction_id))||p.maintenance_state<1||p.maintenance_state>9)
    return E::invalid_family;
  const bool root=p.header.page_type==0x200,branch=p.tree_level>0;
  if((p.header.page_type==0x201&&!branch)||(p.header.page_type==0x202&&branch)
    ||(root&&(p.parent||p.left||p.right||p.low_fence||p.high_fence))||(!root&&!p.parent)
    ||(p.cells.empty()&&(!root||branch))||branch!=p.first_child.has_value())return E::invalid_family;
  if((p.left&&!p.low_fence)||(p.right&&!p.high_fence))return E::invalid_fence;
  if((p.low_fence&&!ValidKey(*p.low_fence))||(p.high_fence&&!ValidKey(*p.high_fence))
    ||(p.low_fence&&p.high_fence&&CompareNativeBtreeKeys(*p.low_fence,*p.high_fence)>=0))return E::invalid_fence;
  if(!references.Add(Self(p),false))return E::invalid_reference;
  const auto ref=[&](const auto& r,bool child){if(!r)return true;
    if(!ValidRef(*r)||(r->filespace_uuid==p.header.filespace_uuid&&r->page_number==p.header.page_number))return false;
    return references.Add(*r,child);};
  if(!ref(p.parent,false)||!ref(p.left,false)||!ref(p.right,false)||!ref(p.first_child,true))return E::invalid_reference;
  for(std::size_t i=0;i<p.cells.size();++i){const auto& c=p.cells[i];
    if(!ValidKey(c.key)||branch!=c.child.has_value()||(branch&&(c.base_page||c.deleted)))return E::invalid_family;
    if(!ref(c.child,true)||!ref(c.base_page,false))return E::invalid_reference;
    if(i&&CompareNativeBtreeKeys(p.cells[i-1].key,c.key)>=0)return E::invalid_order;
    if((p.low_fence&&CompareNativeBtreeKeys(c.key,*p.low_fence)<0)||(p.high_fence&&CompareNativeBtreeKeys(c.key,*p.high_fence)>=0))return E::invalid_fence;
  }
  return E::none;
}
E Validate(const NativeBtreePage& p){OwningReferences references;return Validate(p,references);}
template<class Key> int CompareKeys(const Key& a,const Key& b) noexcept {
  if(!std::equal(a.encoded_key.begin(),a.encoded_key.end(),b.encoded_key.begin(),b.encoded_key.end()))
    return std::lexicographical_compare(a.encoded_key.begin(),a.encoded_key.end(),b.encoded_key.begin(),b.encoded_key.end())?-1:1;
  if(a.row_uuid!=b.row_uuid)return a.row_uuid<b.row_uuid?-1:1;
  if(a.version_uuid!=b.version_uuid)return a.version_uuid<b.version_uuid?-1:1;
  return 0;
}
} // namespace

int CompareNativeBtreeKeys(const NativeBtreeKey& a,const NativeBtreeKey& b) noexcept {
  return CompareKeys(a,b);
}
int CompareNativeBtreeKeys(const NativeBtreeKeyView& a,const NativeBtreeKeyView& b) noexcept {return CompareKeys(a,b);}
bool NativeBtreeDependenciesValid(const NativeBtreeDependencies& d) noexcept {return ValidDependencies(d);}
// ---- SBBTP002 (SB_NATIVE_BTREE_COMPRESSED_PAGE) --------------------------------
namespace {
constexpr std::size_t start=640;
constexpr std::size_t min_cell=kNativeBtreeMinimumCellBytes;
// Unsigned LEB128, minimal.
void PutVarint(std::vector<byte>& out,u64 v){do{byte b=static_cast<byte>(v&0x7f);v>>=7;if(v)b|=0x80;out.push_back(b);}while(v);}
std::size_t VarintBytes(u64 v){std::size_t n=1;while(v>>=7)++n;return n;}
// Reads one minimal LEB128 value; nullopt on overlong, overflow or truncation.
std::optional<u64> GetVarint(std::span<const byte> b,std::size_t& at,std::size_t end){
  u64 v=0;
  for(unsigned i=0;i<10;++i){
    if(at>=end)return std::nullopt;
    const byte x=b[at++];
    if(i==9&&(x&0xfe))return std::nullopt;
    v|=u64(x&0x7f)<<(7*i);
    if(!(x&0x80)){if(i&&!x)return std::nullopt;return v;}
  }
  return std::nullopt;
}
template<class Bytes> std::size_t SharedPrefix(const Bytes& a,const Bytes& b){
  const auto n=std::min(a.size(),b.size());std::size_t i=0;while(i<n&&a[i]==b[i])++i;return i;}
template<class Page> bool RowIsKey(const Page& p){
  if(p.cells.empty())return false;
  for(const auto& c:p.cells)if(c.key.encoded_key.size()!=16||!std::equal(c.key.encoded_key.begin(),c.key.encoded_key.end(),c.key.row_uuid.bytes.begin()))return false;
  return true;
}
template<class Page> bool Compact(const Page& p,const disk::NativePageReference& r){
  return r.filespace_uuid==p.header.filespace_uuid&&r.page_size_profile_uuid==p.header.page_size_profile_uuid;}
// The rules SBBTP002 adds to the shared ones.
template<class Page> E ValidateCells(const Page& p){
  const bool branch=p.tree_level>0;const std::size_t quarter=p.header.page_size_bytes/4;
  for(const auto* fence:{&p.low_fence,&p.high_fence})if(*fence&&(*fence)->encoded_key.size()>quarter)return E::key_too_long;
  for(const auto& c:p.cells){
    if(branch?(c.creator_local_transaction_id||c.superseding_local_transaction_id||!c.payload.empty())
             :(!c.creator_local_transaction_id||!c.base_page||
               (c.superseding_local_transaction_id&&c.superseding_local_transaction_id<=c.creator_local_transaction_id)))
      return E::invalid_family;
    if(c.key.encoded_key.size()+c.payload.size()>quarter)return E::key_too_long;
  }
  return E::none;
}
}  // namespace

NativeBtreeViewRequirements NativeBtreePageViewBackingRequirements(std::size_t bytes) noexcept {
  if(bytes<start||bytes>std::numeric_limits<u32>::max())return {};
  const auto cells=(bytes-start)/min_cell;
  return {cells,4*cells+13,bytes};
}

NativeBtreePageResult EncodeNativeBtreePage(const NativeBtreePage& p) noexcept {
  try {
    if(const auto valid=Validate(p);valid!=E::none)return Fail(valid);
    if(const auto valid=ValidateCells(p);valid!=E::none)return Fail(valid);
    const std::size_t size=p.header.page_size_bytes;
    const bool branch=p.tree_level>0,row_is_key=RowIsKey(p);
    std::size_t expanded=0;for(const auto& c:p.cells)expanded+=c.key.encoded_key.size();
    if(expanded>size)return Fail(E::resource_exhausted);
    const std::size_t count=p.cells.size(),jumps=(count+kNativeBtreeJumpInterval-1)/kNativeBtreeJumpInterval;
    // The body after the family header, built then placed.
    std::vector<byte> body;std::vector<u32> jump_offsets;std::array<std::pair<u32,u32>,2> fences{};
    const auto put_uuid=[&](const Uuid& u){body.insert(body.end(),u.bytes.begin(),u.bytes.end());};
    const auto put64=[&](u64 v){for(unsigned i=0;i<8;++i)body.push_back(static_cast<byte>(v>>(8*i)));};
    const auto put32=[&](u32 v){for(unsigned i=0;i<4;++i)body.push_back(static_cast<byte>(v>>(8*i)));};
    for(unsigned f=0;f<2;++f){const auto& key=f?p.high_fence:p.low_fence;if(!key)continue;
      fences[f]={static_cast<u32>(start+body.size()),static_cast<u32>(40+key->encoded_key.size())};
      put32(static_cast<u32>(key->encoded_key.size()));put32(0);put_uuid(key->row_uuid);put_uuid(key->version_uuid);
      body.insert(body.end(),key->encoded_key.begin(),key->encoded_key.end());}
    const std::size_t jump_table=start+body.size();
    body.resize(body.size()+4*jumps);
    for(std::size_t i=0;i<count;++i){const auto& c=p.cells[i];
      if(start+body.size()>size)return Fail(E::resource_exhausted);
      if(i%kNativeBtreeJumpInterval==0)jump_offsets.push_back(static_cast<u32>(start+body.size()));
      const auto& ref=branch?*c.child:*c.base_page;const bool compact=Compact(p,ref);
      body.push_back(static_cast<byte>((c.deleted?1:0)|(compact?2:0)|(c.payload.empty()?0:4)|(c.superseding_local_transaction_id?8:0)));
      const std::size_t shared=i%kNativeBtreeJumpInterval?SharedPrefix(p.cells[i-1].key.encoded_key,c.key.encoded_key):0;
      PutVarint(body,shared);PutVarint(body,c.key.encoded_key.size()-shared);
      body.insert(body.end(),c.key.encoded_key.begin()+static_cast<std::ptrdiff_t>(shared),c.key.encoded_key.end());
      if(!row_is_key)put_uuid(c.key.row_uuid);
      put_uuid(c.key.version_uuid);
      if(!branch)PutVarint(body,c.creator_local_transaction_id);
      if(c.superseding_local_transaction_id)PutVarint(body,c.superseding_local_transaction_id);
      if(compact){put64(ref.page_number);put64(ref.page_generation);}
      else{put_uuid(ref.filespace_uuid);put64(ref.page_number);put64(ref.page_generation);put_uuid(ref.page_size_profile_uuid);}
      if(!c.payload.empty()){PutVarint(body,c.payload.size());body.insert(body.end(),c.payload.begin(),c.payload.end());}
    }
    const std::size_t used=start+body.size();
    if(used>size)return Fail(E::resource_exhausted);
    for(std::size_t j=0;j<jumps;++j)for(unsigned k=0;k<4;++k)body[jump_table-start+4*j+k]=static_cast<byte>(jump_offsets[j]>>(8*k));
    std::vector<byte> b(size,0);const auto common=disk::EncodeNativeCommonPageHeader(p.header);std::copy(common.bytes->begin(),common.bytes->end(),b.begin());
    auto* f=b.data()+128;const std::string_view magic="SBBTP002";std::copy(magic.begin(),magic.end(),f);StoreLittle16(f+8,2);StoreLittle16(f+10,512);StoreLittle32(f+12,static_cast<u32>(used));
    const auto& d=p.dependencies;Put(f+16,d.index_uuid);StoreLittle64(f+32,d.descriptor_generation);StoreLittle64(f+40,d.storage_generation);
    Put(f+48,d.key_profile_uuid);Put(f+64,d.visibility_profile_uuid);Put(f+80,d.dependency_map_uuid);std::copy(d.dependency_map_sha256.begin(),d.dependency_map_sha256.end(),f+96);
    Put(f+128,p.creator_transaction_uuid);StoreLittle64(f+144,p.creator_local_transaction_id);StoreLittle16(f+152,p.maintenance_state);StoreLittle16(f+154,p.tree_level);
    StoreLittle16(f+156,row_is_key?1:0);StoreLittle16(f+158,kNativeBtreeJumpInterval);
    PutRef(f+160,p.parent);PutRef(f+208,p.left);PutRef(f+256,p.right);PutRef(f+304,p.first_child);
    StoreLittle32(f+352,fences[0].first);StoreLittle32(f+356,fences[0].second);StoreLittle32(f+360,fences[1].first);StoreLittle32(f+364,fences[1].second);
    StoreLittle32(f+368,static_cast<u32>(count));StoreLittle32(f+372,static_cast<u32>(std::count_if(p.cells.begin(),p.cells.end(),[](const auto& c){return c.deleted;})));
    StoreLittle32(f+376,static_cast<u32>(jump_table));StoreLittle32(f+380,static_cast<u32>(jumps));StoreLittle32(f+416,static_cast<u32>(expanded));
    std::copy(body.begin(),body.end(),b.begin()+start);
    const auto digest=Digest(b);if(!digest.ok())return Fail(E::hash_failure);std::copy(digest.digest.begin(),digest.digest.end(),b.begin()+seal);
    return {E::none,p,std::move(b)};
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::invalid_family);}
}

namespace {
template<bool Borrowed>
auto DecodeBtree(std::span<const byte> b,NativeBtreeViewWorkspace workspace) noexcept
    -> std::conditional_t<Borrowed,NativeBtreePageViewResult,NativeBtreePageResult> {
  using Result=std::conditional_t<Borrowed,NativeBtreePageViewResult,NativeBtreePageResult>;
  using Page=std::conditional_t<Borrowed,NativeBtreePageView,NativeBtreePage>;
  using Cell=std::conditional_t<Borrowed,NativeBtreeCellView,NativeBtreeCell>;
  const auto fail=[](E error){Result result;result.error=error;return result;};
  try {
    if(b.size()<start)return fail(E::invalid_header);
    const auto common=disk::DecodeNativeCommonPageHeader(b.data(),128);
    if(!common.ok()||common.header->page_size_bytes!=b.size()||common.header->page_type<0x200||common.header->page_type>0x202)return fail(E::invalid_header);
    if(common.header->flags&1u)return fail(E::encrypted_requires_crypto_authority);
    const auto digest=Digest(b);if(!digest.ok())return fail(E::hash_failure);
    if(!std::equal(digest.digest.begin(),digest.digest.end(),b.begin()+seal))return fail(E::invalid_integrity);
    const auto* f=b.data()+128;
    const std::size_t size=b.size(),used=LoadLittle32(f+12),count=LoadLittle32(f+368),deleted=LoadLittle32(f+372),
        jump_table=LoadLittle32(f+376),jumps=LoadLittle32(f+380),expanded=LoadLittle32(f+416);
    const u16 key_form=LoadLittle16(f+156);
    if(std::string_view(reinterpret_cast<const char*>(f),8)!="SBBTP002"||LoadLittle16(f+8)!=2||LoadLittle16(f+10)!=512
      ||used<start||used>size||key_form>1||LoadLittle16(f+158)!=kNativeBtreeJumpInterval
      ||count>(used-start)/min_cell||deleted>count||jumps!=(count+kNativeBtreeJumpInterval-1)/kNativeBtreeJumpInterval
      ||expanded>size||!Zero(f+420,92)||!Zero(b.data()+used,size-used))return fail(E::invalid_family);
    const bool row_is_key=key_form==1;
    if constexpr(Borrowed){
      const auto aligned=[](auto region){using T=typename decltype(region)::element_type;
        return region.empty()||(region.data()&&reinterpret_cast<std::uintptr_t>(region.data())%alignof(T)==0);};
      if(!aligned(workspace.cells)||!aligned(workspace.references)||
          !disk::detail::DisjointNativeDecodeRegions(b,workspace.cells,workspace.references)||
          !disk::detail::DisjointNativeDecodeRegions(b,workspace.keys,workspace.cells)||
          !disk::detail::DisjointNativeDecodeRegions(b,workspace.keys,workspace.references))
        return fail(E::invalid_workspace);
      if(workspace.cells.size()<count||workspace.references.size()<4*count+13||workspace.keys.size()<expanded)
        return fail(E::resource_exhausted);
    }
    Page p;p.header=*common.header;auto& d=p.dependencies;
    d.index_uuid=Get(f+16);d.descriptor_generation=LoadLittle64(f+32);d.storage_generation=LoadLittle64(f+40);d.key_profile_uuid=Get(f+48);
    d.visibility_profile_uuid=Get(f+64);d.dependency_map_uuid=Get(f+80);std::copy_n(f+96,32,d.dependency_map_sha256.begin());
    p.creator_transaction_uuid=Get(f+128);p.creator_local_transaction_id=LoadLittle64(f+144);p.maintenance_state=LoadLittle16(f+152);p.tree_level=LoadLittle16(f+154);
    p.parent=GetRef(f+160);p.left=GetRef(f+208);p.right=GetRef(f+256);p.first_child=GetRef(f+304);
    const bool branch=p.tree_level>0;const std::size_t quarter=size/4;
    std::size_t at=start;
    const auto fence=[&](auto& key,std::size_t offset){const auto pos=LoadLittle32(f+offset),length=LoadLittle32(f+offset+4);
      if(!pos&&!length)return true;if(pos!=at||length<41||length>used-at)return false;
      const auto* in=b.data()+at;const auto n=LoadLittle32(in);if(n!=length-40||!Zero(in+4,4)||n>quarter)return false;
      typename Page::Key k;k.row_uuid=Get(in+8);k.version_uuid=Get(in+24);
      if constexpr(Borrowed)k.encoded_key={in+40,length-40};else k.encoded_key.assign(in+40,in+length);
      key=std::move(k);at+=length;return true;};
    if(!fence(p.low_fence,352)||!fence(p.high_fence,360))return fail(E::invalid_fence);
    if(jump_table!=at||4*jumps>used-at)return fail(E::invalid_family);
    at+=4*jumps;
    if constexpr(Borrowed)p.cells=workspace.cells.first(count);
    else p.cells.reserve(count);  // `previous` borrows the prior cell's key: no reallocation
    std::size_t rebuilt=0;bool all_row_keys=count>0;
    std::span<const byte> previous;
    for(std::size_t i=0;i<count;++i){
      if(i%kNativeBtreeJumpInterval==0&&LoadLittle32(b.data()+jump_table+4*(i/kNativeBtreeJumpInterval))!=at)return fail(E::invalid_family);
      if(at>=used)return fail(E::invalid_family);
      const byte flags=b[at++];
      if(flags&~byte{15}||(branch&&(flags&13)))return fail(E::invalid_family);
      const auto shared=GetVarint(b,at,used),suffix=shared?GetVarint(b,at,used):std::nullopt;
      if(!shared||!suffix||*suffix>used-at)return fail(E::invalid_family);
      const std::size_t prior_size=i?previous.size():0;
      if((i%kNativeBtreeJumpInterval==0&&*shared)||*shared>prior_size)return fail(E::invalid_family);
      const std::size_t length=*shared+*suffix;
      if(!length||length>quarter)return fail(length?E::key_too_long:E::invalid_family);
      // Canonical: a cell other than a restart cell shares exactly the longest
      // shared prefix (a restart cell stores its whole key by rule).
      if(i%kNativeBtreeJumpInterval&&*suffix&&*shared<prior_size&&b[at]==previous[*shared])return fail(E::invalid_family);
      if(rebuilt+length>expanded)return fail(E::invalid_family);
      Cell c;c.deleted=flags&1;
      std::span<const byte> key;
      if constexpr(Borrowed){
        auto out=workspace.keys.subspan(rebuilt,length);
        std::copy_n(previous.begin(),*shared,out.begin());std::copy_n(b.begin()+static_cast<std::ptrdiff_t>(at),*suffix,out.begin()+static_cast<std::ptrdiff_t>(*shared));
        c.key.encoded_key=out;key=out;
      }else{
        c.key.encoded_key.assign(previous.begin(),previous.begin()+static_cast<std::ptrdiff_t>(*shared));
        c.key.encoded_key.insert(c.key.encoded_key.end(),b.begin()+static_cast<std::ptrdiff_t>(at),b.begin()+static_cast<std::ptrdiff_t>(at+*suffix));
      }
      at+=*suffix;rebuilt+=length;
      const std::size_t fixed=(row_is_key?0:16)+16;
      if(fixed>used-at)return fail(E::invalid_family);
      if(row_is_key){if(length!=16)return fail(E::invalid_family);
        if constexpr(Borrowed)std::copy_n(key.begin(),16,c.key.row_uuid.bytes.begin());
        else std::copy_n(c.key.encoded_key.begin(),16,c.key.row_uuid.bytes.begin());
      }else{c.key.row_uuid=Get(b.data()+at);at+=16;}
      c.key.version_uuid=Get(b.data()+at);at+=16;
      if(!branch){const auto creator=GetVarint(b,at,used);if(!creator||!*creator)return fail(E::invalid_family);c.creator_local_transaction_id=*creator;}
      if(flags&8){const auto superseding=GetVarint(b,at,used);
        if(!superseding||*superseding<=c.creator_local_transaction_id)return fail(E::invalid_family);c.superseding_local_transaction_id=*superseding;}
      disk::NativePageReference ref;
      if(flags&2){if(16>used-at)return fail(E::invalid_family);
        ref={p.header.filespace_uuid,LoadLittle64(b.data()+at),LoadLittle64(b.data()+at+8),p.header.page_size_profile_uuid};at+=16;}
      else{if(48>used-at)return fail(E::invalid_family);const auto full=GetRef(b.data()+at);at+=48;
        if(!full||Compact(p,*full))return fail(E::invalid_family);ref=*full;}
      (branch?c.child:c.base_page)=ref;
      if(flags&4){const auto n=GetVarint(b,at,used);if(!n||!*n||*n>used-at)return fail(E::invalid_family);
        if constexpr(Borrowed)c.payload={b.data()+at,static_cast<std::size_t>(*n)};else c.payload.assign(b.begin()+static_cast<std::ptrdiff_t>(at),b.begin()+static_cast<std::ptrdiff_t>(at+*n));
        at+=*n;}
      if(!row_is_key&&(length!=16||!std::equal(c.key.encoded_key.begin(),c.key.encoded_key.end(),c.key.row_uuid.bytes.begin())))all_row_keys=false;
      if constexpr(Borrowed){workspace.cells[i]=std::move(c);previous=workspace.cells[i].key.encoded_key;}
      else{p.cells.push_back(std::move(c));previous=p.cells.back().key.encoded_key;}
    }
    if(at!=used||rebuilt!=expanded||row_is_key!=all_row_keys||
       deleted!=static_cast<std::size_t>(std::count_if(p.cells.begin(),p.cells.end(),[](const auto& c){return c.deleted;})))return fail(E::invalid_family);
    E valid;
    if constexpr(Borrowed){BorrowedReferences references(workspace.references.first(4*count+13));valid=Validate(p,references);}
    else valid=Validate(p);
    if(valid==E::none)valid=ValidateCells(p);
    if(valid!=E::none)return fail(valid);
    Result out;out.error=E::none;out.page=std::move(p);
    if constexpr(Borrowed)out.bytes=b;else out.bytes.assign(b.begin(),b.end());
    return out;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}catch(const std::length_error&){return fail(E::resource_exhausted);}catch(...){return fail(E::invalid_family);}
}
}  // namespace
NativeBtreePageResult DecodeNativeBtreePage(const std::vector<byte>& bytes) noexcept {
  return DecodeBtree<false>(bytes,{});
}
NativeBtreePageViewResult DecodeNativeBtreePageInto(std::span<const byte> bytes,NativeBtreeViewWorkspace workspace) noexcept {
  return DecodeBtree<true>(bytes,workspace);
}

NativeBtreePageResult ReadNativeBtreePageFromOpenDevice(disk::FileDevice& device,const Uuid& db,
    const disk::NativePageReference& ref,u32 type,const NativeBtreeDependencies& dependencies) noexcept {
  try {
    if(!V7(db)||!ValidRef(ref)||type<0x200||type>0x202)return Fail(E::invalid_reference);
    if(!ValidDependencies(dependencies))return Fail(E::invalid_dependencies);
    const auto guard=device.AcquireOperationGuard();const disk::FilespaceBootstrapBinding expected{db,ref.filespace_uuid,ref.page_size_profile_uuid};
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device,&expected);
    if(!zero.ok()){
      if(zero.error==disk::FilespacePageZeroError::resource_exhausted)return Fail(E::resource_exhausted);
      if(zero.error==disk::FilespacePageZeroError::hash_provider_failure)return Fail(E::hash_failure);
      if(zero.error==disk::FilespacePageZeroError::io_failure)return Fail(E::io_failure);
      return Fail(E::invalid_filespace);}
    const auto& z=*zero.record;
    // Registered filespace roles describe purpose, not an index placement ban.
    // The validated bootstrap, actual capacity and owning security/lifecycle
    // admission remain mandatory; this reader does not authorize creation.
    if(ref.page_number>=z.total_pages)return Fail(E::invalid_filespace);
    if(z.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)return Fail(E::encrypted_requires_crypto_authority);
    if(z.bootstrap.flags&disk::FilespaceBootstrapFlag::cluster_authority_required)return Fail(E::cluster_requires_authority);
    std::vector<byte> bytes(z.bootstrap.page_size_bytes);const auto io=device.ReadAt(ref.page_number*z.bootstrap.page_size_bytes,bytes.data(),bytes.size());
    if(!io.ok()||io.bytes_transferred!=bytes.size())return Fail(E::io_failure);
    const disk::NativeCommonPageHeaderBinding header_binding{{db,ref.filespace_uuid,ref.page_size_profile_uuid},ref.page_number,ref.page_generation,type,{}};
    const auto common=disk::DecodeNativeCommonPageHeader(bytes.data(),128,&header_binding);
    if(!common.ok())return Fail(common.error==disk::NativeCommonPageHeaderError::binding_mismatch?E::binding_mismatch:E::invalid_header);
    if(common.header->flags&1u)return Fail(E::encrypted_requires_crypto_authority);
    if(common.header->flags&2u)return Fail(E::cluster_requires_authority);
    if(common.header->flags&12u)return Fail(E::header_policy_requires_authority);
    auto result=DecodeNativeBtreePage(bytes);if(!result.ok())return result;const auto& p=*result.page;
    if(p.header.database_uuid!=db||Self(p)!=ref||p.header.page_type!=type||p.dependencies!=dependencies)return Fail(E::binding_mismatch);
    const auto in_range=[&](const auto& r){return !r||r->filespace_uuid!=ref.filespace_uuid||r->page_number<z.total_pages;};
    if(!in_range(p.parent)||!in_range(p.left)||!in_range(p.right)||!in_range(p.first_child))return Fail(E::invalid_reference);
    for(const auto& c:p.cells)if(!in_range(c.child)||!in_range(c.base_page))return Fail(E::invalid_reference);
    return result;
  }catch(const std::bad_alloc&){return Fail(E::resource_exhausted);}catch(const std::length_error&){return Fail(E::resource_exhausted);}catch(...){return Fail(E::io_failure);}
}
NativeBtreeTreeResult ReadNativeBtreeTreeFromOpenDevices(const Uuid& db,
    const std::vector<disk::NativeFilespaceDevice>& devices,const disk::NativePageReference& root,
    const NativeBtreeDependencies& dependencies,u64 maximum_retained_image_bytes) noexcept {
  const auto fail=[](E e){NativeBtreeTreeResult r;r.error=e;return r;};
  try {
    if(!V7(db)||!ValidRef(root)||devices.empty()||!maximum_retained_image_bytes)return fail(E::invalid_reference);
    if(!ValidDependencies(dependencies))return fail(E::invalid_dependencies);
    auto ordered=devices;std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.filespace_uuid<b.filespace_uuid;});
    for(std::size_t i=0;i<ordered.size();++i){const auto& fs=ordered[i];
      if(!V7(fs.filespace_uuid)||!disk::FindCanonicalFilespacePageProfile(fs.page_size_profile_uuid)||!fs.device
        ||(i&&fs.filespace_uuid==ordered[i-1].filespace_uuid))return fail(E::invalid_filespace);
      for(std::size_t j=0;j<i;++j)if(fs.device==ordered[j].device)return fail(E::invalid_filespace);}
    std::vector<std::unique_lock<std::recursive_mutex>> guards;guards.reserve(ordered.size());
    for(const auto& fs:ordered)guards.push_back(fs.device->AcquireOperationGuard());
    std::vector<disk::FilespacePageZero> zeros;zeros.reserve(ordered.size());
    for(const auto& fs:ordered){const disk::FilespaceBootstrapBinding binding{db,fs.filespace_uuid,fs.page_size_profile_uuid};
      auto zero=disk::ReadFilespacePageZeroFromOpenDevice(*fs.device,&binding);
      if(!zero.ok()){
        if(zero.error==disk::FilespacePageZeroError::resource_exhausted)return fail(E::resource_exhausted);
        if(zero.error==disk::FilespacePageZeroError::hash_provider_failure)return fail(E::hash_failure);
        if(zero.error==disk::FilespacePageZeroError::io_failure)return fail(E::io_failure);
        return fail(E::invalid_filespace);}
      zeros.push_back(std::move(*zero.record));}
    const auto filespace=[&](const auto& ref)->std::size_t{
      const auto fs=std::lower_bound(ordered.begin(),ordered.end(),ref.filespace_uuid,[](const auto& a,const auto& id){return a.filespace_uuid<id;});
      if(fs==ordered.end()||fs->filespace_uuid!=ref.filespace_uuid||fs->page_size_profile_uuid!=ref.page_size_profile_uuid)return ordered.size();
      const auto i=static_cast<std::size_t>(fs-ordered.begin());return ref.page_number<zeros[i].total_pages?i:ordered.size();};
    struct Pending {
      disk::NativePageReference ref;
      std::optional<std::size_t> parent;
      u16 level=0;
      std::optional<NativeBtreeKey> low,high;
    };
    std::vector<Pending> pending{{root,{},0,{},{}}};NativeBtreeTreeResult result;
    std::set<std::pair<Uuid,u64>> slots;std::set<Uuid> page_ids;std::map<u16,std::size_t> last_by_level;
    while(!pending.empty()){
      auto next=std::move(pending.back());pending.pop_back();const auto fs=filespace(next.ref);
      if(fs==ordered.size())return fail(E::invalid_filespace);
      if(!slots.emplace(next.ref.filespace_uuid,next.ref.page_number).second)return fail(E::tree_reference_mismatch);
      const auto page_size=zeros[fs].bootstrap.page_size_bytes;
      if(page_size>maximum_retained_image_bytes-result.retained_image_bytes)return fail(E::resource_exhausted);
      const u32 type=!next.parent?0x200:next.level?0x201:0x202;
      auto loaded=ReadNativeBtreePageFromOpenDevice(*ordered[fs].device,db,next.ref,type,dependencies);
      if(!loaded.ok())return fail(loaded.error);const auto& page=*loaded.page;
      if(!page_ids.insert(page.header.page_uuid).second)return fail(E::tree_reference_mismatch);
      const auto previous=last_by_level.find(page.tree_level);
      const auto topology=detail::ValidateNativeBtreeTraversalStep(page,
        next.parent?&*result.pages[*next.parent].page:nullptr,next.level,next.low,next.high,
        previous==last_by_level.end()?nullptr:&*result.pages[previous->second].page);
      if(topology!=E::none)return fail(topology);
      for(const auto& cell:page.cells)if(cell.base_page&&filespace(*cell.base_page)==ordered.size())return fail(E::invalid_filespace);
      const auto index=result.pages.size();last_by_level[page.tree_level]=index;
      result.retained_image_bytes+=loaded.bytes.size();result.pages.push_back(std::move(loaded));
      // Borrow only after insertion; later vector growth must not invalidate a
      // parent reference used while preparing the reverse-order DFS work list.
      const auto& current=*result.pages.back().page;
      if(!current.tree_level){result.leaves.push_back(index);continue;}
      const auto child_level=static_cast<u16>(current.tree_level-1);
      for(std::size_t i=current.cells.size();i>0;--i){const auto& cell=current.cells[i-1];
        pending.push_back({*cell.child,index,child_level,cell.key,i==current.cells.size()?current.high_fence:std::optional{current.cells[i].key}});}
      pending.push_back({*current.first_child,index,child_level,current.low_fence,current.cells.front().key});
    }
    for(const auto& [level,index]:last_by_level)if(result.pages[index].page->right)return fail(E::tree_sibling_mismatch);
    result.error=E::none;return result;
  }catch(const std::bad_alloc&){return fail(E::resource_exhausted);}catch(const std::length_error&){return fail(E::resource_exhausted);}catch(...){return fail(E::io_failure);}
}
} // namespace scratchbird::storage::page
