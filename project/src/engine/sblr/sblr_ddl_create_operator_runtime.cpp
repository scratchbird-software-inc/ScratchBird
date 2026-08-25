#include "engine/sblr/sblr_ddl_create_operator_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <algorithm>
#include <cstring>
namespace scratchbird::engine::sblr { namespace {
template<class A> bool nz(const A&a){return std::any_of(a.begin(),a.end(),[](auto b){return b!=0;});}
void put(std::vector<std::uint8_t>&o,std::uint64_t v,std::size_t n){for(std::size_t i=0;i<n;++i)o.push_back(static_cast<std::uint8_t>(v>>(8*i)));}
std::uint64_t get(const std::uint8_t*b,std::size_t n){std::uint64_t v=0;for(std::size_t i=0;i<n;++i)v|=std::uint64_t(b[i])<<(8*i);return v;}
std::array<std::uint8_t,32> dg(const char*d,const std::vector<std::uint8_t>&b){std::vector<std::uint8_t>x(d,d+std::strlen(d));x.insert(x.end(),b.begin(),b.end());return scratchbird::core::hash::ComputeSha256Digest(x).digest;}
std::vector<std::uint8_t> pack(const char*m,const std::array<std::uint8_t,376>&b,const char*d){std::vector<std::uint8_t>o{(std::uint8_t)m[0],(std::uint8_t)m[1],(std::uint8_t)m[2],(std::uint8_t)m[3],1,0,0,0};o.insert(o.end(),b.begin(),b.end());std::fill(o.begin()+352,o.begin()+384,0);auto h=dg(d,o);std::copy(h.begin(),h.end(),o.begin()+352);return o;}
bool unpack(const std::uint8_t*b,std::size_t n,const char*m,std::array<std::uint8_t,376>*o,const char*d,std::string*e){if(!o||n!=384||std::memcmp(b,m,4)||b[4]!=1){if(e)*e="descriptor invalid";return false;}std::copy_n(b+8,376,o->begin());auto x=pack(m,*o,d);if(!std::equal(x.begin(),x.end(),b)){if(e)*e="descriptor hash invalid";return false;}return true;}
}
std::vector<std::uint8_t> EncodeSblrDdlCreateOperatorRequestV1(const SblrDdlCreateOperatorRequestV1&v){if(!nz(v.operation)||!nz(v.receipt)||v.descriptor_length!=384)return{};std::vector<std::uint8_t>o={'S','B','D','Q'};put(o,1,2);put(o,8,2);put(o,7563,4);put(o,550,4);o.insert(o.end(),v.operation.begin(),v.operation.end());o.insert(o.end(),v.receipt.begin(),v.receipt.end());put(o,384,4);o.insert(o.end(),12,0);return o;}
bool DecodeSblrDdlCreateOperatorRequestV1(const std::uint8_t*b,std::size_t n,SblrDdlCreateOperatorRequestV1*o,std::string*e){if(!o||n!=64||std::memcmp(b,"SBDQ",4)||get(b+4,2)!=1||get(b+6,2)!=8||get(b+8,4)!=7563||get(b+12,4)!=550){if(e)*e="SBDQ invalid";return false;}SblrDdlCreateOperatorRequestV1 v;std::copy_n(b+16,16,v.operation.begin());std::copy_n(b+32,16,v.receipt.begin());v.descriptor_length=get(b+48,4);if(EncodeSblrDdlCreateOperatorRequestV1(v).empty())return false;*o=v;return true;}
std::vector<std::uint8_t> EncodeSblrDdlCreateOperatorDescriptorV1(const SblrDdlCreateOperatorDescriptorV1&v){return nz(v.body)?pack("SBDD",v.body,"SBOD|SBDD|v1"):std::vector<std::uint8_t>{};}
bool DecodeSblrDdlCreateOperatorDescriptorV1(const std::uint8_t*b,std::size_t n,SblrDdlCreateOperatorDescriptorV1*o,std::string*e){return o&&unpack(b,n,"SBDD",&o->body,"SBOD|SBDD|v1",e);}
std::vector<std::uint8_t> EncodeSblrDdlCreateOperatorResultV1(const SblrDdlCreateOperatorResultV1&v){return nz(v.body)?pack("SLRR",v.body,"ScratchBird.SblrDdlCreateOperator.Result.V1"):std::vector<std::uint8_t>{};}
bool DecodeSblrDdlCreateOperatorResultV1(const std::uint8_t*b,std::size_t n,SblrDdlCreateOperatorResultV1*o,std::string*e){return o&&unpack(b,n,"SLRR",&o->body,"ScratchBird.SblrDdlCreateOperator.Result.V1",e);}
}
