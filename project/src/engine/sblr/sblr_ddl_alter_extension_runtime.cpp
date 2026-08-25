#include "engine/sblr/sblr_ddl_alter_extension_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <algorithm>
#include <cstring>
namespace scratchbird::engine::sblr { namespace {
template<class A> bool nonzero(const A& a){ return std::any_of(a.begin(),a.end(),[](auto v){return v!=0;}); }
void put(std::vector<std::uint8_t>& o,std::uint64_t v,std::size_t n){for(std::size_t i=0;i<n;++i)o.push_back(static_cast<std::uint8_t>(v>>(8*i)));}
std::uint64_t get(const std::uint8_t* p,std::size_t n){std::uint64_t v=0;for(std::size_t i=0;i<n;++i)v|=std::uint64_t(p[i])<<(8*i);return v;}
std::array<std::uint8_t,32> hash(const char* domain,const std::uint8_t* p,std::size_t n){std::vector<std::uint8_t>x(domain,domain+std::strlen(domain));x.insert(x.end(),p,p+n);return core::hash::ComputeSha256Digest(x).digest;}
std::vector<std::uint8_t> frame(const char magic[4],const std::array<std::uint8_t,376>& body,const char* domain){std::vector<std::uint8_t> o{(std::uint8_t)magic[0],(std::uint8_t)magic[1],(std::uint8_t)magic[2],(std::uint8_t)magic[3],1,0,0,0};o.insert(o.end(),body.begin(),body.end());std::fill(o.begin()+352,o.begin()+384,0);auto h=hash(domain,o.data(),352);std::copy(h.begin(),h.end(),o.begin()+352);return o;}
bool unframe(const std::uint8_t*p,std::size_t n,const char magic[4],std::array<std::uint8_t,376>* body,const char* domain,std::string* e){if(!body||!p||n!=384||std::memcmp(p,magic,4)!=0||p[4]!=1||p[5]!=0||p[6]!=0||p[7]!=0){if(e)*e="frame invalid";return false;}auto h=hash(domain,p,352);if(!std::equal(h.begin(),h.end(),p+352)){if(e)*e="hash invalid";return false;}std::copy_n(p+8,376,body->begin());return true;}
}
std::vector<std::uint8_t> EncodeSblrDdlAlterExtensionRequestV1(const SblrDdlAlterExtensionRequestV1& v){if(!nonzero(v.operation)||!nonzero(v.receipt)||v.descriptor_length!=384)return{};std::vector<std::uint8_t>o{'S','B','D','Q'};put(o,1,2);put(o,8,2);put(o,7589,4);put(o,576,4);o.insert(o.end(),v.operation.begin(),v.operation.end());o.insert(o.end(),v.receipt.begin(),v.receipt.end());put(o,384,4);o.insert(o.end(),12,0);return o;}
bool DecodeSblrDdlAlterExtensionRequestV1(const std::uint8_t*p,std::size_t n,SblrDdlAlterExtensionRequestV1* o,std::string*e){if(!o||!p||n!=64||std::memcmp(p,"SBDQ",4)!=0||get(p+4,2)!=1||get(p+6,2)!=8||get(p+8,4)!=7589||get(p+12,4)!=576||std::any_of(p+52,p+64,[](auto v){return v!=0;})){if(e)*e="request invalid";return false;}std::copy_n(p+16,16,o->operation.begin());std::copy_n(p+32,16,o->receipt.begin());o->descriptor_length=static_cast<std::uint32_t>(get(p+48,4));return nonzero(o->operation)&&nonzero(o->receipt)&&o->descriptor_length==384;}
std::vector<std::uint8_t> EncodeSblrDdlAlterExtensionDescriptorV1(const SblrDdlAlterExtensionDescriptorV1&v){return nonzero(v.body)?frame("SBDD",v.body,"SBOD|SBDD|v1"):std::vector<std::uint8_t>{};}
bool DecodeSblrDdlAlterExtensionDescriptorV1(const std::uint8_t*p,std::size_t n,SblrDdlAlterExtensionDescriptorV1*o,std::string*e){return o&&unframe(p,n,"SBDD",&o->body,"SBOD|SBDD|v1",e);}
std::vector<std::uint8_t> EncodeSblrDdlAlterExtensionResultV1(const SblrDdlAlterExtensionResultV1&v){return nonzero(v.body)?frame("SLRR",v.body,"ScratchBird.SblrDdlAlterExtension.Result.V1"):std::vector<std::uint8_t>{};}
bool DecodeSblrDdlAlterExtensionResultV1(const std::uint8_t*p,std::size_t n,SblrDdlAlterExtensionResultV1*o,std::string*e){return o&&unframe(p,n,"SLRR",&o->body,"ScratchBird.SblrDdlAlterExtension.Result.V1",e);}
}
