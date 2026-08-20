#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using CompareUuid=std::array<std::uint8_t,16>;using CompareSha=std::array<std::uint8_t,32>;struct SblrCompareRequestV1{CompareUuid receipt{};std::uint64_t occurrence=0;std::uint32_t comparison_occurrence=0;};struct SblrCompareDescriptorV1{std::array<std::uint8_t,360>canonical_body{};CompareSha evidence{};std::uint64_t availability_generation=0;};struct SblrCompareResultV1{CompareUuid comparison_uuid{};std::uint64_t comparison_generation=0;std::uint8_t boolean_value=0;bool is_null=false;CompareSha executor_evidence{};std::uint64_t availability_generation=0;CompareUuid publication_barrier{};};std::vector<std::uint8_t>EncodeSblrCompareRequestV1(const SblrCompareRequestV1&);bool DecodeSblrCompareRequestV1(const std::uint8_t*,std::size_t,SblrCompareRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrCompareDescriptorV1(const SblrCompareDescriptorV1&,bool);bool DecodeSblrCompareDescriptorV1(const std::uint8_t*,std::size_t,SblrCompareDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrCompareResultV1(const SblrCompareResultV1&);bool DecodeSblrCompareResultV1(const std::uint8_t*,std::size_t,SblrCompareResultV1*,std::string*);}
