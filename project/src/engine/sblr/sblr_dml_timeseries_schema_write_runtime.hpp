#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDmlTimeseriesSchemaWriteRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence{}; std::uint64_t write_occurrence{}; };
struct SblrDmlTimeseriesSchemaWriteDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; };
struct SblrDmlTimeseriesSchemaWriteResultV1 { std::array<std::uint8_t,240> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDmlTimeseriesSchemaWriteRequestV1(const SblrDmlTimeseriesSchemaWriteRequestV1&);
bool DecodeSblrDmlTimeseriesSchemaWriteRequestV1(const std::uint8_t*,std::size_t,SblrDmlTimeseriesSchemaWriteRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDmlTimeseriesSchemaWriteDescriptorV1(const SblrDmlTimeseriesSchemaWriteDescriptorV1&,bool);
bool DecodeSblrDmlTimeseriesSchemaWriteDescriptorV1(const std::uint8_t*,std::size_t,SblrDmlTimeseriesSchemaWriteDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDmlTimeseriesSchemaWriteResultV1(const SblrDmlTimeseriesSchemaWriteResultV1&);
bool DecodeSblrDmlTimeseriesSchemaWriteResultV1(const std::uint8_t*,std::size_t,SblrDmlTimeseriesSchemaWriteResultV1*,std::string*);
}
