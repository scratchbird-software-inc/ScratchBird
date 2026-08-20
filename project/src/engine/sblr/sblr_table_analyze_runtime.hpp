#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using TableAnalyzeUuid=std::array<std::uint8_t,16>;using TableAnalyzeSha=std::array<std::uint8_t,32>;struct SblrTableAnalyzeRequestV1{TableAnalyzeUuid receipt{};std::uint64_t occurrence=0;std::uint32_t analyze_occurrence=0;};struct SblrTableAnalyzeDescriptorV1{std::array<std::uint8_t,368> canonical_body{};TableAnalyzeSha evidence{};std::uint64_t availability_generation=0;};struct SblrTableAnalyzeResultV1{std::array<std::uint8_t,136> canonical_body{};TableAnalyzeSha evidence{};std::uint64_t availability_generation=0;};std::vector<std::uint8_t>EncodeSblrTableAnalyzeRequestV1(const SblrTableAnalyzeRequestV1&);bool DecodeSblrTableAnalyzeRequestV1(const std::uint8_t*,std::size_t,SblrTableAnalyzeRequestV1*,std::string*);std::vector<std::uint8_t>EncodeSblrTableAnalyzeDescriptorV1(const SblrTableAnalyzeDescriptorV1&,bool);bool DecodeSblrTableAnalyzeDescriptorV1(const std::uint8_t*,std::size_t,SblrTableAnalyzeDescriptorV1*,std::string*,bool);std::vector<std::uint8_t>EncodeSblrTableAnalyzeResultV1(const SblrTableAnalyzeResultV1&);bool DecodeSblrTableAnalyzeResultV1(const std::uint8_t*,std::size_t,SblrTableAnalyzeResultV1*,std::string*);}
