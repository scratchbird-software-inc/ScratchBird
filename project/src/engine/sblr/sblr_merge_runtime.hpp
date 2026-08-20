#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using MergeUuid=std::array<std::uint8_t,16>;using MergeSha=std::array<std::uint8_t,32>;struct SblrMergeRequestV1{MergeUuid receipt{};std::uint64_t occurrence=0;std::uint32_t merge_occurrence=0;};struct SblrMergeDescriptorV1{std::array<std::uint8_t,432> canonical_body{};MergeSha evidence{};std::uint64_t availability_generation=0;};struct SblrMergeResultV1{std::array<std::uint8_t,136> canonical_body{};MergeSha evidence{};std::uint64_t availability_generation=0;};std::vector<std::uint8_t>EncodeSblrMergeRequestV1(const SblrMergeRequestV1&);bool DecodeSblrMergeRequestV1(const std::uint8_t*,std::size_t,SblrMergeRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrMergeDescriptorV1(const SblrMergeDescriptorV1&,bool);bool DecodeSblrMergeDescriptorV1(const std::uint8_t*,std::size_t,SblrMergeDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrMergeResultV1(const SblrMergeResultV1&);bool DecodeSblrMergeResultV1(const std::uint8_t*,std::size_t,SblrMergeResultV1*,std::string*);}
