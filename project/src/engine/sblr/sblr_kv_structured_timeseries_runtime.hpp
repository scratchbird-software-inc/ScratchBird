#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using KvStructuredTimeseriesUuid=std::array<std::uint8_t,16>; using KvStructuredTimeseriesSha=std::array<std::uint8_t,32>;
struct SblrKvStructuredTimeseriesRequestV1 { KvStructuredTimeseriesUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t timeseries_occurrence=0; };
struct SblrKvStructuredTimeseriesDescriptorV1 { std::array<std::uint8_t,400> body{}; KvStructuredTimeseriesSha evidence{}; std::uint64_t availability=0; };
struct SblrKvStructuredTimeseriesResultV1 { std::array<std::uint8_t,240> body{}; KvStructuredTimeseriesSha evidence{}; std::uint64_t availability=0; KvStructuredTimeseriesUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrKvStructuredTimeseriesRequestV1(const SblrKvStructuredTimeseriesRequestV1&);
bool DecodeSblrKvStructuredTimeseriesRequestV1(const std::uint8_t*,std::size_t,SblrKvStructuredTimeseriesRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrKvStructuredTimeseriesDescriptorV1(const SblrKvStructuredTimeseriesDescriptorV1&,bool);
bool DecodeSblrKvStructuredTimeseriesDescriptorV1(const std::uint8_t*,std::size_t,SblrKvStructuredTimeseriesDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrKvStructuredTimeseriesResultV1(const SblrKvStructuredTimeseriesResultV1&);
bool DecodeSblrKvStructuredTimeseriesResultV1(const std::uint8_t*,std::size_t,SblrKvStructuredTimeseriesResultV1*,std::string*);
}
