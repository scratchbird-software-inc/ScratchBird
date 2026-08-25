#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrAccelLlvmPolicySetRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrAccelLlvmPolicySetDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrAccelLlvmPolicySetResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrAccelLlvmPolicySetRequestV1(const SblrAccelLlvmPolicySetRequestV1&);bool DecodeSblrAccelLlvmPolicySetRequestV1(const std::uint8_t*,std::size_t,SblrAccelLlvmPolicySetRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrAccelLlvmPolicySetDescriptorV1(const SblrAccelLlvmPolicySetDescriptorV1&);bool DecodeSblrAccelLlvmPolicySetDescriptorV1(const std::uint8_t*,std::size_t,SblrAccelLlvmPolicySetDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrAccelLlvmPolicySetResultV1(const SblrAccelLlvmPolicySetResultV1&);bool DecodeSblrAccelLlvmPolicySetResultV1(const std::uint8_t*,std::size_t,SblrAccelLlvmPolicySetResultV1*,std::string*);}
