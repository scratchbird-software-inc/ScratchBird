#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{struct SblrVersionedBranchCreateRequestV1{std::array<std::uint8_t,16>operation{},receipt{};std::uint32_t descriptor_length=0;};struct SblrVersionedBranchCreateDescriptorV1{std::array<std::uint8_t,376>body{};};struct SblrVersionedBranchCreateResultV1{std::array<std::uint8_t,376>body{};};std::vector<std::uint8_t>EncodeSblrVersionedBranchCreateRequestV1(const SblrVersionedBranchCreateRequestV1&);bool DecodeSblrVersionedBranchCreateRequestV1(const std::uint8_t*,std::size_t,SblrVersionedBranchCreateRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrVersionedBranchCreateDescriptorV1(const SblrVersionedBranchCreateDescriptorV1&);bool DecodeSblrVersionedBranchCreateDescriptorV1(const std::uint8_t*,std::size_t,SblrVersionedBranchCreateDescriptorV1*,std::string*);std::vector<std::uint8_t>EncodeSblrVersionedBranchCreateResultV1(const SblrVersionedBranchCreateResultV1&);bool DecodeSblrVersionedBranchCreateResultV1(const std::uint8_t*,std::size_t,SblrVersionedBranchCreateResultV1*,std::string*);}
