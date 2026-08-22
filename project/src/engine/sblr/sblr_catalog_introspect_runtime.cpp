#include "sblr_catalog_introspect_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <algorithm>
#include <cstring>
namespace scratchbird::engine::sblr { namespace {
void put(std::vector<uint8_t>&o,uint64_t v,size_t n){for(size_t i=0;i<n;i++)o.push_back(uint8_t(v>>(8*i)));}
uint64_t get(const uint8_t*b,size_t n){uint64_t v=0;for(size_t i=0;i<n;i++)v|=uint64_t(b[i])<<(8*i);return v;}
template<class T>bool nz(const T&x){return std::any_of(x.begin(),x.end(),[](auto v){return v!=0;});}
std::vector<uint8_t> hdr(const char*m,size_t n){std::vector<uint8_t>o(m,m+4);put(o,1,2);put(o,n,2);put(o,n,4);put(o,0,4);return o;}
bool vh(const uint8_t*b,size_t n,const char*m,size_t z){return b&&n==z&&std::equal(b,b+4,m)&&get(b+4,2)==1&&get(b+6,2)==z&&get(b+8,4)==z&&std::all_of(b+12,b+16,[](auto v){return v==0;});}
CatalogSha sha(const char*d,const uint8_t*b,size_t n){std::vector<uint8_t>x(d,d+std::strlen(d));x.insert(x.end(),b,b+n);return scratchbird::core::hash::ComputeSha256Digest(x).digest;}
}
std::vector<uint8_t> EncodeSblrCatalogIntrospectRequestV1(const SblrCatalogIntrospectRequestV1&v){if(!nz(v.receipt)||!v.occurrence||!v.object_occurrence)return{};auto o=hdr("CIRQ",64);o.insert(o.end(),v.receipt.begin(),v.receipt.end());put(o,v.occurrence,8);put(o,v.object_occurrence,4);o.insert(o.end(),20,0);return o;}
bool DecodeSblrCatalogIntrospectRequestV1(const uint8_t*b,size_t n,SblrCatalogIntrospectRequestV1*out,std::string*d){if(!out||!vh(b,n,"CIRQ",64)||std::any_of(b+44,b+64,[](auto v){return v!=0;})){if(d)*d="CIRQ invalid";return false;}SblrCatalogIntrospectRequestV1 v;std::copy_n(b+16,16,v.receipt.begin());v.occurrence=get(b+32,8);v.object_occurrence=get(b+40,4);if(EncodeSblrCatalogIntrospectRequestV1(v).empty())return false;*out=v;return true;}
std::vector<uint8_t> EncodeSblrCatalogIntrospectDescriptorV1(const SblrCatalogIntrospectDescriptorV1&v,bool op){if(!nz(v.body)||!v.availability)return{};auto o=hdr(op?"CIDO":"CIDD",488);o.insert(o.end(),v.body.begin(),v.body.end());auto e=sha("ScratchBird.SblrCatalogIntrospectShowObjectDetailDescriptor.V1",o.data()+16,392);if(nz(v.evidence)&&e!=v.evidence)return{};o.insert(o.end(),e.begin(),e.end());put(o,v.availability,8);o.insert(o.end(),40,0);return o;}
bool DecodeSblrCatalogIntrospectDescriptorV1(const uint8_t*b,size_t n,SblrCatalogIntrospectDescriptorV1*out,std::string*d,bool op){if(!out||!vh(b,n,op?"CIDO":"CIDD",488)||std::any_of(b+448,b+488,[](auto v){return v!=0;})){if(d)*d="CID invalid";return false;}SblrCatalogIntrospectDescriptorV1 v;std::copy_n(b+16,392,v.body.begin());std::copy_n(b+408,32,v.evidence.begin());v.availability=get(b+440,8);if(EncodeSblrCatalogIntrospectDescriptorV1(v,op).empty())return false;*out=v;return true;}
std::vector<uint8_t> EncodeSblrCatalogIntrospectResultV1(const SblrCatalogIntrospectResultV1&v){if(!nz(v.body)||!v.availability||!nz(v.publication_barrier))return{};auto o=hdr("CIRS",320);o.insert(o.end(),v.body.begin(),v.body.end());auto e=sha("ScratchBird.SblrCatalogIntrospectShowObjectDetailExecutorEvidence.V1",o.data()+16,240);if(nz(v.evidence)&&e!=v.evidence)return{};o.insert(o.end(),e.begin(),e.end());put(o,v.availability,8);o.insert(o.end(),v.publication_barrier.begin(),v.publication_barrier.end());o.insert(o.end(),8,0);return o;}
bool DecodeSblrCatalogIntrospectResultV1(const uint8_t*b,size_t n,SblrCatalogIntrospectResultV1*out,std::string*d){if(!out||!vh(b,n,"CIRS",320)||std::any_of(b+312,b+320,[](auto v){return v!=0;})){if(d)*d="CIRS invalid";return false;}SblrCatalogIntrospectResultV1 v;std::copy_n(b+16,240,v.body.begin());std::copy_n(b+256,32,v.evidence.begin());v.availability=get(b+288,8);std::copy_n(b+296,16,v.publication_barrier.begin());if(EncodeSblrCatalogIntrospectResultV1(v).empty())return false;*out=v;return true;}
}
