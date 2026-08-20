#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using AggregateUuid=std::array<std::uint8_t,16>;using AggregateSha=std::array<std::uint8_t,32>;struct SblrAggregateRequestV1{AggregateUuid receipt{};std::uint64_t occurrence=0;std::uint32_t aggregate_occurrence=0;};struct SblrAggregateDescriptorV1{std::array<std::uint8_t,400>body{};AggregateSha evidence{};std::uint64_t availability=0;};struct SblrAggregateResultV1{std::array<std::uint8_t,240>body{};AggregateSha evidence{};std::uint64_t availability=0;AggregateUuid publication_barrier{};};std::vector<std::uint8_t>EncodeSblrAggregateRequestV1(const SblrAggregateRequestV1&);bool DecodeSblrAggregateRequestV1(const std::uint8_t*,std::size_t,SblrAggregateRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrAggregateDescriptorV1(const SblrAggregateDescriptorV1&,bool);bool DecodeSblrAggregateDescriptorV1(const std::uint8_t*,std::size_t,SblrAggregateDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrAggregateResultV1(const SblrAggregateResultV1&);bool DecodeSblrAggregateResultV1(const std::uint8_t*,std::size_t,SblrAggregateResultV1*,std::string*);}
