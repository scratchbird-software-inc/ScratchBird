#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using AccessFetchUuid=std::array<std::uint8_t,16>;using AccessFetchSha=std::array<std::uint8_t,32>;
struct SblrAccessCursorFetchRequestV1{AccessFetchUuid receipt{};std::uint64_t occurrence=0;std::uint32_t fetch_occurrence=0;};
struct SblrAccessCursorFetchDescriptorV1{AccessFetchUuid descriptor{},cursor{},relation{},index{},session{},transaction{},catalog{},mga{},security{};AccessFetchSha prior_position_token{},evidence{};std::uint64_t descriptor_generation=0,cursor_generation=0,prior_position_generation=0,availability_generation=0;std::uint32_t maximum_rows=0;std::uint8_t direction=0;};
struct SblrAccessCursorFetchResultV1{AccessFetchUuid descriptor{},cursor{},row_batch{};AccessFetchSha row_batch_sha{},refreshed_position_token{},evidence{};std::uint64_t descriptor_generation=0,cursor_generation=0,prior_position_generation=0,resulting_position_generation=0,availability_generation=0;std::uint32_t returned_rows=0;std::uint8_t eof=0,direction=0;};
std::vector<std::uint8_t>EncodeSblrAccessCursorFetchRequestV1(const SblrAccessCursorFetchRequestV1&);bool DecodeSblrAccessCursorFetchRequestV1(const std::uint8_t*,std::size_t,SblrAccessCursorFetchRequestV1*,std::string*);
std::vector<std::uint8_t>EncodeSblrAccessCursorFetchDescriptorV1(const SblrAccessCursorFetchDescriptorV1&,bool);bool DecodeSblrAccessCursorFetchDescriptorV1(const std::uint8_t*,std::size_t,SblrAccessCursorFetchDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t>EncodeSblrAccessCursorFetchResultV1(const SblrAccessCursorFetchResultV1&);bool DecodeSblrAccessCursorFetchResultV1(const std::uint8_t*,std::size_t,SblrAccessCursorFetchResultV1*,std::string*);}
