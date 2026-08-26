#include "engine/sblr/sblr_versioned_history_aux_runtime.hpp"
namespace scratchbird::engine::sblr { namespace {void id(std::vector<std::uint8_t>&x,std::uint32_t a,std::uint16_t b){x[8]=a&255;x[9]=a>>8;x[10]=a>>16;x[11]=a>>24;x[12]=b&255;x[13]=b>>8;}template<class Q>std::vector<std::uint8_t> enc(const Q&v,std::uint32_t a,std::uint16_t b){auto x=EncodeSblrAccelGpuCompileRequestV1(v);if(x.size()==64)id(x,a,b);return x;}template<class Q>bool dec(const std::uint8_t*p,std::size_t n,Q*o,std::string*e,std::uint32_t a,std::uint16_t b){if(!p||n!=64||p[8]!=(a&255)||p[9]!=((a>>8)&255)||p[12]!=(b&255)||p[13]!=((b>>8)&255)){if(e)*e="request invalid";return false;}std::vector<std::uint8_t>x(p,p+n);id(x,7643,630);return DecodeSblrAccelGpuCompileRequestV1(x.data(),x.size(),o,e);}}
#define SBVH_IMPL(PFX,Q,D,A,B) std::vector<std::uint8_t> Encode##PFX##RequestV1(const Q&v){return enc(v,A,B);} bool Decode##PFX##RequestV1(const std::uint8_t*p,std::size_t n,Q*o,std::string*e){return dec(p,n,o,e,A,B);} std::vector<std::uint8_t> Encode##PFX##DescriptorV1(const D&v){return EncodeSblrAccelGpuCompileDescriptorV1(v);} bool Decode##PFX##DescriptorV1(const std::uint8_t*p,std::size_t n,D*o,std::string*e){return DecodeSblrAccelGpuCompileDescriptorV1(p,n,o,e);}
SBVH_IMPL(SblrVerifyProofDescriptor,SblrVerifyProofDescriptorRequestV1,SblrVerifyProofDescriptorDescriptorV1,7667,654)
SBVH_IMPL(SblrVersionedMerge,SblrVersionedMergeRequestV1,SblrVersionedMergeDescriptorV1,7669,656)
SBVH_IMPL(SblrVersionedHashRead,SblrVersionedHashReadRequestV1,SblrVersionedHashReadDescriptorV1,7671,658)
#undef SBVH_IMPL
}
