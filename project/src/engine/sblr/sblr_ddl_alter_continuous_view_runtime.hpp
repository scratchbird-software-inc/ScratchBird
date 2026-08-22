#pragma once
#include "sblr_ddl_create_continuous_view_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrDdlAlterContinuousViewRequestV1 = SblrDdlCreateContinuousViewRequestV1;
using SblrDdlAlterContinuousViewDescriptorV1 = SblrDdlCreateContinuousViewDescriptorV1;
using SblrDdlAlterContinuousViewResultV1 = SblrDdlCreateContinuousViewResultV1;
std::vector<std::uint8_t> EncodeSblrDdlAlterContinuousViewRequestV1(const SblrDdlAlterContinuousViewRequestV1&);
bool DecodeSblrDdlAlterContinuousViewRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterContinuousViewRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterContinuousViewDescriptorV1(const SblrDdlAlterContinuousViewDescriptorV1&,bool);
bool DecodeSblrDdlAlterContinuousViewDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterContinuousViewDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlAlterContinuousViewResultV1(const SblrDdlAlterContinuousViewResultV1&);
bool DecodeSblrDdlAlterContinuousViewResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterContinuousViewResultV1*,std::string*);
}
