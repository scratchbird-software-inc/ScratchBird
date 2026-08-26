#pragma once
#include "sblr_accel_gpu_compile_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrVersionedStatusReadRequestV1 = SblrAccelGpuCompileRequestV1;
using SblrVersionedStatusReadDescriptorV1 = SblrAccelGpuCompileDescriptorV1;
using SblrVersionedStatusReadResultV1 = SblrAccelGpuCompileResultV1;
std::vector<std::uint8_t> EncodeSblrVersionedStatusReadRequestV1(const SblrVersionedStatusReadRequestV1&);
bool DecodeSblrVersionedStatusReadRequestV1(const std::uint8_t*,std::size_t,SblrVersionedStatusReadRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrVersionedStatusReadDescriptorV1(const SblrVersionedStatusReadDescriptorV1&);
bool DecodeSblrVersionedStatusReadDescriptorV1(const std::uint8_t*,std::size_t,SblrVersionedStatusReadDescriptorV1*,std::string*);
}
