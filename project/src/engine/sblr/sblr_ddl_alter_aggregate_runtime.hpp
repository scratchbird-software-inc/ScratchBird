#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlAlterAggregateUuid=std::array<std::uint8_t,16>; using DdlAlterAggregateSha=std::array<std::uint8_t,32>;
struct SblrDdlAlterAggregateRequestV1{DdlAlterAggregateUuid receipt{};std::uint64_t occurrence=0;std::uint32_t aggregate_occurrence=0;};
struct SblrDdlAlterAggregateDescriptorV1{std::array<std::uint8_t,400> body{};DdlAlterAggregateSha evidence{};std::uint64_t availability=0;};
struct SblrDdlAlterAggregateResultV1{std::array<std::uint8_t,240> body{};DdlAlterAggregateSha evidence{};std::uint64_t availability=0;DdlAlterAggregateUuid publication_barrier{};};
std::vector<std::uint8_t> EncodeSblrDdlAlterAggregateRequestV1(const SblrDdlAlterAggregateRequestV1&); bool DecodeSblrDdlAlterAggregateRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterAggregateRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterAggregateDescriptorV1(const SblrDdlAlterAggregateDescriptorV1&,bool); bool DecodeSblrDdlAlterAggregateDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterAggregateDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlAlterAggregateResultV1(const SblrDdlAlterAggregateResultV1&); bool DecodeSblrDdlAlterAggregateResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterAggregateResultV1*,std::string*);
}
