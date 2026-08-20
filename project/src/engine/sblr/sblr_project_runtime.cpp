#include "sblr_project_runtime.hpp"
#include "core/hash/hash_digest.hpp"
#include <algorithm>
#include <cstring>
namespace scratchbird::engine::sblr { namespace {
void put(std::vector<uint8_t>& o,uint64_t v,size_t n){for(size_t i=0;i<n;i++)o.push_back(v>>(8*i));}
uint64_t get(const uint8_t*b,size_t n){uint64_t v=0;for(size_t i=0;i<n;i++)v|=uint64_t(b[i])<<(8*i);return v;}
template<class T>bool nz(const T&x){return std::any_of(x.begin(),x.end(),[](auto v){return v;});}
auto header(const char*m,size_t n){std::vector<uint8_t>o(m,m+4);put(o,1,2);put(o,n,2);put(o,n,4);put(o,0,4);return o;}
bool valid_header(const uint8_t*b,size_t n,const char*m,size_t z){return b&&n==z&&std::equal(b,b+4,m)&&get(b+4,2)==1&&get(b+6,2)==z&&get(b+8,4)==z&&std::all_of(b+12,b+16,[](auto v){return !v;});}
ProjectSha hash(const char*d,const uint8_t*b,size_t n){std::vector<uint8_t>x(d,d+strlen(d));x.insert(x.end(),b,b+n);return scratchbird::core::hash::ComputeSha256Digest(x).digest;}
}
std::vector<uint8_t> EncodeSblrProjectRequestV1(const SblrProjectRequestV1&v){if(!nz(v.receipt)||!v.occurrence||!v.projection_occurrence)return{};auto o=header("PJRQ",64);o.insert(o.end(),v.receipt.begin(),v.receipt.end());put(o,v.occurrence,8);put(o,v.projection_occurrence,4);o.insert(o.end(),20,0);return o;}
bool DecodeSblrProjectRequestV1(const uint8_t*b,size_t n,SblrProjectRequestV1*out,std::string*d){if(!out||!valid_header(b,n,"PJRQ",64)||std::any_of(b+44,b+64,[](auto v){return v;})){if(d)*d="PJRQ invalid";return false;}SblrProjectRequestV1 v;std::copy_n(b+16,16,v.receipt.begin());v.occurrence=get(b+32,8);v.projection_occurrence=get(b+40,4);if(EncodeSblrProjectRequestV1(v).empty())return false;*out=v;return true;}
std::vector<uint8_t> EncodeSblrProjectDescriptorV1(const SblrProjectDescriptorV1&v,bool op){if(!nz(v.body)||!v.availability)return{};auto o=header(op?"PJDO":"PJDD",488);o.insert(o.end(),v.body.begin(),v.body.end());auto e=hash("ScratchBird.SblrProjectDescriptor.V1",o.data()+16,392);if(nz(v.evidence)&&e!=v.evidence)return{};o.insert(o.end(),e.begin(),e.end());put(o,v.availability,8);o.insert(o.end(),40,0);return o;}
bool DecodeSblrProjectDescriptorV1(const uint8_t*b,size_t n,SblrProjectDescriptorV1*out,std::string*d,bool op){if(!out||!valid_header(b,n,op?"PJDO":"PJDD",488)||std::any_of(b+448,b+488,[](auto v){return v;})){if(d)*d="PJD invalid";return false;}SblrProjectDescriptorV1 v;std::copy_n(b+16,392,v.body.begin());std::copy_n(b+408,32,v.evidence.begin());v.availability=get(b+440,8);if(EncodeSblrProjectDescriptorV1(v,op).empty())return false;*out=v;return true;}
std::vector<uint8_t> EncodeSblrProjectResultV1(const SblrProjectResultV1&v){const auto completion=v.body[24],state=v.body[25];const auto columns=get(v.body.data()+56,4);ProjectUuid handle{};std::copy_n(v.body.data()+68,16,handle.begin());if(!nz(v.body)||completion!=1||state>1||!columns||(state==0&&nz(handle))||(state==1&&!nz(handle))||!v.availability||!nz(v.publication_barrier))return{};auto o=header("PJRS",320);o.insert(o.end(),v.body.begin(),v.body.end());auto e=hash("ScratchBird.SblrProjectExecutorEvidence.V1",o.data()+16,240);if(nz(v.evidence)&&e!=v.evidence)return{};o.insert(o.end(),e.begin(),e.end());put(o,v.availability,8);o.insert(o.end(),v.publication_barrier.begin(),v.publication_barrier.end());o.insert(o.end(),8,0);return o;}
bool DecodeSblrProjectResultV1(const uint8_t*b,size_t n,SblrProjectResultV1*out,std::string*d){if(!out||!valid_header(b,n,"PJRS",320)||std::any_of(b+312,b+320,[](auto v){return v;})){if(d)*d="PJRS invalid";return false;}SblrProjectResultV1 v;std::copy_n(b+16,240,v.body.begin());std::copy_n(b+256,32,v.evidence.begin());v.availability=get(b+288,8);std::copy_n(b+296,16,v.publication_barrier.begin());if(EncodeSblrProjectResultV1(v).empty())return false;*out=v;return true;}
}
