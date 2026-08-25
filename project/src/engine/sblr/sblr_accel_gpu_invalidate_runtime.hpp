#pragma once
#include "sblr_accel_gpu_compile_runtime.hpp"
namespace scratchbird::engine::sblr { using SblrAccelGpuInvalidateRequestV1=SblrAccelGpuCompileRequestV1; using SblrAccelGpuInvalidateDescriptorV1=SblrAccelGpuCompileDescriptorV1; using SblrAccelGpuInvalidateResultV1=SblrAccelGpuCompileResultV1;
std::vector<std::uint8_t> EncodeSblrAccelGpuInvalidateRequestV1(const SblrAccelGpuInvalidateRequestV1&);
bool DecodeSblrAccelGpuInvalidateRequestV1(const std::uint8_t*,std::size_t,SblrAccelGpuInvalidateRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrAccelGpuInvalidateDescriptorV1(const SblrAccelGpuInvalidateDescriptorV1&);
bool DecodeSblrAccelGpuInvalidateDescriptorV1(const std::uint8_t*,std::size_t,SblrAccelGpuInvalidateDescriptorV1*,std::string*);
}
