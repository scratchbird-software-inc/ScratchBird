#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr{using AggregateInvokeUuid=std::array<std::uint8_t,16>;using AggregateInvokeSha=std::array<std::uint8_t,32>;struct SblrAggregateInvokeRequestV1{AggregateInvokeUuid receipt{};std::uint64_t occurrence=0;std::uint32_t invocation_occurrence=0;};struct SblrAggregateInvokeDescriptorV1{std::array<std::uint8_t,408>body{};AggregateInvokeSha evidence{};std::uint64_t availability=0;};struct SblrAggregateInvokeResultV1{std::array<std::uint8_t,176>body{};AggregateInvokeSha evidence{};std::uint64_t availability=0;};std::vector<std::uint8_t>EncodeSblrAggregateInvokeRequestV1(const SblrAggregateInvokeRequestV1&);bool DecodeSblrAggregateInvokeRequestV1(const std::uint8_t*,std::size_t,SblrAggregateInvokeRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrAggregateInvokeDescriptorV1(const SblrAggregateInvokeDescriptorV1&,bool);bool DecodeSblrAggregateInvokeDescriptorV1(const std::uint8_t*,std::size_t,SblrAggregateInvokeDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrAggregateInvokeResultV1(const SblrAggregateInvokeResultV1&);bool DecodeSblrAggregateInvokeResultV1(const std::uint8_t*,std::size_t,SblrAggregateInvokeResultV1*,std::string*);}
