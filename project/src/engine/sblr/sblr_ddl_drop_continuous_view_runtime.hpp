#pragma once
#include "sblr_ddl_create_continuous_view_runtime.hpp"
namespace scratchbird::engine::sblr {
using SblrDdlDropContinuousViewRequestV1 = SblrDdlCreateContinuousViewRequestV1;
using SblrDdlDropContinuousViewDescriptorV1 = SblrDdlCreateContinuousViewDescriptorV1;
using SblrDdlDropContinuousViewResultV1 = SblrDdlCreateContinuousViewResultV1;
using DdlDropContinuousViewSha = std::array<std::uint8_t, 32>;
std::vector<std::uint8_t> EncodeSblrDdlDropContinuousViewRequestV1(const SblrDdlDropContinuousViewRequestV1&);
bool DecodeSblrDdlDropContinuousViewRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropContinuousViewRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropContinuousViewDescriptorV1(const SblrDdlDropContinuousViewDescriptorV1&,bool);
bool DecodeSblrDdlDropContinuousViewDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropContinuousViewDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropContinuousViewResultV1(const SblrDdlDropContinuousViewResultV1&);
bool DecodeSblrDdlDropContinuousViewResultV1(const std::uint8_t*,std::size_t,SblrDdlDropContinuousViewResultV1*,std::string*);
}
