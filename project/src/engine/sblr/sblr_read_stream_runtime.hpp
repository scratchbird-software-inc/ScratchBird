#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using StreamUuid=std::array<std::uint8_t,16>;using StreamSha=std::array<std::uint8_t,32>;
struct SblrReadStreamRequestV1{StreamUuid receipt{};std::uint64_t occurrence=0;std::uint32_t stream_occurrence=0;};
struct SblrReadStreamDescriptorV1{StreamUuid descriptor{},relation{},schema{},mga{},security{},row_shape{},policy{};StreamSha evidence{};std::uint64_t descriptor_generation=0,relation_generation=0,schema_generation=0,mga_generation=0,security_generation=0,row_shape_generation=0,policy_generation=0,availability_generation=0;std::uint32_t maximum_rows=0,maximum_bytes=0;std::uint8_t mode=0,redaction=0;};
struct SblrReadStreamHandleV1{StreamUuid descriptor{},stream{},relation{},row_shape{};StreamSha continuation{},evidence{};std::uint64_t descriptor_generation=0,stream_generation=0,availability_generation=0;std::uint8_t state=0;};
std::vector<std::uint8_t>EncodeSblrReadStreamRequestV1(const SblrReadStreamRequestV1&);bool DecodeSblrReadStreamRequestV1(const std::uint8_t*,std::size_t,SblrReadStreamRequestV1*,std::string*);
std::vector<std::uint8_t>EncodeSblrReadStreamDescriptorV1(const SblrReadStreamDescriptorV1&,bool);bool DecodeSblrReadStreamDescriptorV1(const std::uint8_t*,std::size_t,SblrReadStreamDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t>EncodeSblrReadStreamHandleV1(const SblrReadStreamHandleV1&);bool DecodeSblrReadStreamHandleV1(const std::uint8_t*,std::size_t,SblrReadStreamHandleV1*,std::string*);}
