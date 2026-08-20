#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlDropAggregateUuid=std::array<std::uint8_t,16>; using DdlDropAggregateSha=std::array<std::uint8_t,32>;
struct SblrDdlDropAggregateRequestV1{DdlDropAggregateUuid receipt{};std::uint64_t occurrence=0;std::uint32_t aggregate_occurrence=0;};
struct SblrDdlDropAggregateDescriptorV1{std::array<std::uint8_t,400> body{};DdlDropAggregateSha evidence{};std::uint64_t availability=0;};
struct SblrDdlDropAggregateResultV1{std::array<std::uint8_t,240> body{};DdlDropAggregateSha evidence{};std::uint64_t availability=0;DdlDropAggregateUuid publication_barrier{};};
std::vector<std::uint8_t> EncodeSblrDdlDropAggregateRequestV1(const SblrDdlDropAggregateRequestV1&); bool DecodeSblrDdlDropAggregateRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropAggregateRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropAggregateDescriptorV1(const SblrDdlDropAggregateDescriptorV1&,bool); bool DecodeSblrDdlDropAggregateDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropAggregateDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropAggregateResultV1(const SblrDdlDropAggregateResultV1&); bool DecodeSblrDdlDropAggregateResultV1(const std::uint8_t*,std::size_t,SblrDdlDropAggregateResultV1*,std::string*);
}
