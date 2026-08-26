#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDdlDropTimeseriesValueCacheRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence{}; std::uint64_t cache_occurrence{}; };
struct SblrDdlDropTimeseriesValueCacheDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; };
struct SblrDdlDropTimeseriesValueCacheResultV1 { std::array<std::uint8_t,240> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlDropTimeseriesValueCacheRequestV1(const SblrDdlDropTimeseriesValueCacheRequestV1&);
bool DecodeSblrDdlDropTimeseriesValueCacheRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropTimeseriesValueCacheRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropTimeseriesValueCacheDescriptorV1(const SblrDdlDropTimeseriesValueCacheDescriptorV1&,bool);
bool DecodeSblrDdlDropTimeseriesValueCacheDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropTimeseriesValueCacheDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropTimeseriesValueCacheResultV1(const SblrDdlDropTimeseriesValueCacheResultV1&);
bool DecodeSblrDdlDropTimeseriesValueCacheResultV1(const std::uint8_t*,std::size_t,SblrDdlDropTimeseriesValueCacheResultV1*,std::string*);
}

