#pragma once
#include "sblr_accel_gpu_compile_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrVerifyProofDescriptorRequestV1=SblrAccelGpuCompileRequestV1; using SblrVerifyProofDescriptorDescriptorV1=SblrAccelGpuCompileDescriptorV1;
using SblrVersionedMergeRequestV1=SblrAccelGpuCompileRequestV1; using SblrVersionedMergeDescriptorV1=SblrAccelGpuCompileDescriptorV1;
using SblrVersionedHashReadRequestV1=SblrAccelGpuCompileRequestV1; using SblrVersionedHashReadDescriptorV1=SblrAccelGpuCompileDescriptorV1;
#define SBVH_DECL(PFX,REQ,DESC) std::vector<std::uint8_t> Encode##PFX##RequestV1(const REQ&); bool Decode##PFX##RequestV1(const std::uint8_t*,std::size_t,REQ*,std::string*); std::vector<std::uint8_t> Encode##PFX##DescriptorV1(const DESC&); bool Decode##PFX##DescriptorV1(const std::uint8_t*,std::size_t,DESC*,std::string*);
SBVH_DECL(SblrVerifyProofDescriptor,SblrVerifyProofDescriptorRequestV1,SblrVerifyProofDescriptorDescriptorV1)
SBVH_DECL(SblrVersionedMerge,SblrVersionedMergeRequestV1,SblrVersionedMergeDescriptorV1)
SBVH_DECL(SblrVersionedHashRead,SblrVersionedHashReadRequestV1,SblrVersionedHashReadDescriptorV1)
#undef SBVH_DECL
}
