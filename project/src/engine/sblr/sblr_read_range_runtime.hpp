#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using RangeUuid=std::array<std::uint8_t,16>;using RangeSha=std::array<std::uint8_t,32>;
struct SblrReadRangeRequestV1{RangeUuid receipt{};std::uint64_t occurrence=0;std::uint32_t range_occurrence=0;};
struct SblrReadRangeDescriptorV1{RangeUuid descriptor{},relation{},index{},schema{},mga{},security{},key_type{},codec{},collation{};RangeSha lower_sha{},upper_sha{},evidence{};std::array<std::uint8_t,32>lower{},upper{};std::uint64_t descriptor_generation=0,relation_generation=0,index_generation=0,schema_generation=0,mga_generation=0,security_generation=0,codec_generation=0,collation_generation=0,availability_generation=0;std::uint32_t maximum_rows=0;std::uint16_t lower_length=0,upper_length=0;std::uint8_t lower_state=0,upper_state=0,inclusivity=0,direction=0;};
struct SblrReadRangeResultV1{RangeUuid descriptor{},relation{},batch{};RangeSha batch_sha{},continuation{},evidence{};std::uint64_t descriptor_generation=0,availability_generation=0;std::uint32_t rows=0;std::uint8_t eof=0;};
std::vector<std::uint8_t>EncodeSblrReadRangeRequestV1(const SblrReadRangeRequestV1&);bool DecodeSblrReadRangeRequestV1(const std::uint8_t*,std::size_t,SblrReadRangeRequestV1*,std::string*);
std::vector<std::uint8_t>EncodeSblrReadRangeDescriptorV1(const SblrReadRangeDescriptorV1&,bool);bool DecodeSblrReadRangeDescriptorV1(const std::uint8_t*,std::size_t,SblrReadRangeDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t>EncodeSblrReadRangeResultV1(const SblrReadRangeResultV1&);bool DecodeSblrReadRangeResultV1(const std::uint8_t*,std::size_t,SblrReadRangeResultV1*,std::string*);}
