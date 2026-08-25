#include "engine/sblr/sblr_versioned_revert_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <algorithm>
#include <cstring>

namespace scratchbird::engine::sblr { namespace {
template<class A> bool NonZero(const A& a) { return std::any_of(a.begin(), a.end(), [](auto v){ return v != 0; }); }
void Put(std::vector<std::uint8_t>& o, std::uint64_t v, std::size_t n) { for (std::size_t i=0;i<n;++i) o.push_back(static_cast<std::uint8_t>(v>>(8*i))); }
std::uint64_t Get(const std::uint8_t* p, std::size_t n) { std::uint64_t v=0; for (std::size_t i=0;i<n;++i) v|=static_cast<std::uint64_t>(p[i])<<(8*i); return v; }
std::array<std::uint8_t,32> Hash(const char* domain, const std::uint8_t* p) {
  std::vector<std::uint8_t> x(domain, domain+std::strlen(domain)); x.insert(x.end(),p,p+352); return core::hash::ComputeSha256Digest(x).digest;
}
template<class A> std::vector<std::uint8_t> EncodeFrame(const char* magic, const A& body, const char* domain) {
  std::vector<std::uint8_t> o={(std::uint8_t)magic[0],(std::uint8_t)magic[1],(std::uint8_t)magic[2],(std::uint8_t)magic[3],1,0,0,0};
  o.insert(o.end(),body.begin(),body.end()); std::fill(o.begin()+352,o.begin()+384,0); auto h=Hash(domain,o.data()); std::copy(h.begin(),h.end(),o.begin()+352); return o;
}
template<class A> bool DecodeFrame(const std::uint8_t* p,std::size_t n,const char* magic,A* body,const char* domain,std::string* e) {
  if(!p||!body||n!=384||std::memcmp(p,magic,4)!=0||p[4]!=1||p[5]||p[6]||p[7]) { if(e)*e="frame invalid"; return false; }
  auto h=Hash(domain,p); if(!std::equal(h.begin(),h.end(),p+352)) { if(e)*e="hash invalid"; return false; }
  std::copy_n(p+8,376,body->begin()); return true;
}
}
std::vector<std::uint8_t> EncodeSblrVersionedRevertRequestV1(const SblrVersionedRevertRequestV1& v) {
  if(!NonZero(v.operation)||!NonZero(v.receipt)||v.descriptor_length!=384) return {};
  std::vector<std::uint8_t> o={'S','B','D','Q'}; Put(o,1,2); Put(o,8,2); Put(o,7629,4); Put(o,616,4);
  o.insert(o.end(),v.operation.begin(),v.operation.end()); o.insert(o.end(),v.receipt.begin(),v.receipt.end()); Put(o,384,4); o.insert(o.end(),12,0); return o;
}
bool DecodeSblrVersionedRevertRequestV1(const std::uint8_t* p,std::size_t n,SblrVersionedRevertRequestV1* o,std::string* e) {
  if(!o||!p||n!=64||std::memcmp(p,"SBDQ",4)||Get(p+4,2)!=1||Get(p+6,2)!=8||Get(p+8,4)!=7629||Get(p+12,4)!=616||Get(p+48,4)!=384||std::any_of(p+52,p+64,[](auto v){return v!=0;})){if(e)*e="request invalid";return false;}
  std::copy_n(p+16,16,o->operation.begin()); std::copy_n(p+32,16,o->receipt.begin()); o->descriptor_length=384; return NonZero(o->operation)&&NonZero(o->receipt);
}
std::vector<std::uint8_t> EncodeSblrVersionedRevertDescriptorV1(const SblrVersionedRevertDescriptorV1& v){return NonZero(v.body)?EncodeFrame("SBDD",v.body,"SBOD|SBDD|v1"):std::vector<std::uint8_t>{};}
bool DecodeSblrVersionedRevertDescriptorV1(const std::uint8_t* p,std::size_t n,SblrVersionedRevertDescriptorV1* o,std::string* e){return o&&DecodeFrame(p,n,"SBDD",&o->body,"SBOD|SBDD|v1",e);}
std::vector<std::uint8_t> EncodeSblrVersionedRevertResultV1(const SblrVersionedRevertResultV1& v){return NonZero(v.body)?EncodeFrame("SLRR",v.body,"ScratchBird.SblrVersionedRevert.Result.V1"):std::vector<std::uint8_t>{};}
bool DecodeSblrVersionedRevertResultV1(const std::uint8_t* p,std::size_t n,SblrVersionedRevertResultV1* o,std::string* e){return o&&DecodeFrame(p,n,"SLRR",&o->body,"ScratchBird.SblrVersionedRevert.Result.V1",e);}
}
