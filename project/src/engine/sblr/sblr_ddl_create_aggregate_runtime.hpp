#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr { using AggregateUuid=std::array<std::uint8_t,16>;using AggregateSha=std::array<std::uint8_t,32>;struct SblrDdlCreateAggregateRequestV1{AggregateUuid receipt{};std::uint64_t occurrence=0;std::uint32_t aggregate_occurrence=0;};struct SblrDdlCreateAggregateDescriptorV1{std::array<std::uint8_t,400> body{};AggregateSha evidence{};std::uint64_t availability=0;};struct SblrDdlCreateAggregateResultV1{std::array<std::uint8_t,240> body{};AggregateSha evidence{};std::uint64_t availability=0;AggregateUuid publication_barrier{};};std::vector<std::uint8_t> EncodeSblrDdlCreateAggregateRequestV1(const SblrDdlCreateAggregateRequestV1&);bool DecodeSblrDdlCreateAggregateRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateAggregateRequestV1*,std::string*);std::vector<std::uint8_t> EncodeSblrDdlCreateAggregateDescriptorV1(const SblrDdlCreateAggregateDescriptorV1&,bool);bool DecodeSblrDdlCreateAggregateDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateAggregateDescriptorV1*,std::string*,bool);std::vector<std::uint8_t> EncodeSblrDdlCreateAggregateResultV1(const SblrDdlCreateAggregateResultV1&);bool DecodeSblrDdlCreateAggregateResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateAggregateResultV1*,std::string*);}
