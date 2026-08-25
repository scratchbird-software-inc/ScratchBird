#include "engine/sblr/sblr_ddl_alter_publication_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <algorithm>
#include <cstring>
namespace scratchbird::engine::sblr { namespace {
template<class A> bool nz(const A& a){return std::any_of(a.begin(),a.end(),[](auto b){return b!=0;});}
void put(std::vector<std::uint8_t>& o,std::uint64_t v,std::size_t n){for(std::size_t i=0;i<n;++i)o.push_back(static_cast<std::uint8_t>(v>>(8*i)));}
std::uint64_t get(const std::uint8_t* p,std::size_t n){std::uint64_t v=0;for(std::size_t i=0;i<n;++i)v|=std::uint64_t(p[i])<<(8*i);return v;}
std::vector<std::uint8_t> header(const char* m,std::size_t){std::vector<std::uint8_t>o(m,m+4);put(o,1,2);put(o,8,2);put(o,0,4);put(o,0,4);return o;}
bool valid_header(const std::uint8_t*b,std::size_t n,const char*m,std::size_t z){return b&&n==z&&std::equal(b,b+4,m)&&get(b+4,2)==1&&get(b+6,2)==8;}
std::vector<std::uint8_t> envelope(const char* m){std::vector<std::uint8_t>o(m,m+4);put(o,1,2);put(o,0,2);return o;}
bool valid_envelope(const std::uint8_t*b,std::size_t n,const char*m,std::size_t z){return b&&n==z&&std::equal(b,b+4,m)&&get(b+4,2)==1&&get(b+6,2)==0;}
std::array<std::uint8_t,32> digest(const char* domain,const std::vector<std::uint8_t>& bytes){std::vector<std::uint8_t>x(domain,domain+std::strlen(domain));x.insert(x.end(),bytes.begin(),bytes.end());return scratchbird::core::hash::ComputeSha256Digest(x).digest;}
}
std::vector<std::uint8_t> EncodeSblrDdlAlterPublicationRequestV1(const SblrDdlAlterPublicationRequestV1&v){if(!nz(v.operation)||!nz(v.receipt)||v.descriptor_length!=320)return{};auto o=header("SBAQ",64);for(int i=0;i<4;++i)o[8+i]=static_cast<std::uint8_t>(7553>>(8*i));for(int i=0;i<4;++i)o[12+i]=static_cast<std::uint8_t>(540>>(8*i));o.insert(o.end(),v.operation.begin(),v.operation.end());o.insert(o.end(),v.receipt.begin(),v.receipt.end());put(o,v.descriptor_length,4);o.insert(o.end(),12,0);return o;}
bool DecodeSblrDdlAlterPublicationRequestV1(const std::uint8_t*b,std::size_t n,SblrDdlAlterPublicationRequestV1*out,std::string*d){if(!out||!valid_header(b,n,"SBAQ",64)||get(b+8,4)!=7553||get(b+12,4)!=540||std::any_of(b+52,b+64,[](auto x){return x!=0;})){if(d)*d="SBAQ invalid";return false;}SblrDdlAlterPublicationRequestV1 v;std::copy_n(b+16,16,v.operation.begin());std::copy_n(b+32,16,v.receipt.begin());v.descriptor_length=static_cast<std::uint32_t>(get(b+48,4));if(EncodeSblrDdlAlterPublicationRequestV1(v).empty())return false;*out=v;return true;}
std::vector<std::uint8_t> EncodeSblrDdlAlterPublicationDescriptorV1(const SblrDdlAlterPublicationDescriptorV1&v){if(!nz(v.body))return{};auto o=envelope("SLAP");o.insert(o.end(),v.body.begin(),v.body.end());std::fill(o.begin()+8+280,o.begin()+8+312,0);auto h=digest("ScratchBird.SblrDdlAlterPublicationDescriptor.V1",o);std::copy(h.begin(),h.end(),o.begin()+8+280);return o;}
bool DecodeSblrDdlAlterPublicationDescriptorV1(const std::uint8_t*b,std::size_t n,SblrDdlAlterPublicationDescriptorV1*out,std::string*d){if(!out||!valid_envelope(b,n,"SLAP",320)){if(d)*d="SLAP invalid";return false;}SblrDdlAlterPublicationDescriptorV1 v;std::copy_n(b+8,312,v.body.begin());auto expected=EncodeSblrDdlAlterPublicationDescriptorV1(v);if(expected.size()!=320||!std::equal(expected.begin(),expected.end(),b)){if(d)*d="SLAP hash invalid";return false;}*out=v;return true;}
std::vector<std::uint8_t> EncodeSblrDdlAlterPublicationResultV1(const SblrDdlAlterPublicationResultV1&v){if(!nz(v.body))return{};auto o=envelope("SLRR");o.insert(o.end(),v.body.begin(),v.body.end());std::fill(o.begin()+8+116,o.begin()+8+148,0);auto h=digest("ScratchBird.SblrDdlAlterPublicationResult.V1",o);std::copy(h.begin(),h.end(),o.begin()+8+116);return o;}
bool DecodeSblrDdlAlterPublicationResultV1(const std::uint8_t*b,std::size_t n,SblrDdlAlterPublicationResultV1*out,std::string*d){if(!out||!valid_envelope(b,n,"SLRR",192)){if(d)*d="SLRR invalid";return false;}SblrDdlAlterPublicationResultV1 v;std::copy_n(b+8,184,v.body.begin());auto expected=EncodeSblrDdlAlterPublicationResultV1(v);if(expected.size()!=192||!std::equal(expected.begin(),expected.end(),b)){if(d)*d="SLRR hash invalid";return false;}*out=v;return true;}
}
