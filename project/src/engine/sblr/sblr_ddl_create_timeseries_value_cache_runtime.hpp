#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDdlCreateTimeseriesValueCacheRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence{}; std::uint64_t cache_occurrence{}; };
struct SblrDdlCreateTimeseriesValueCacheDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; };
struct SblrDdlCreateTimeseriesValueCacheResultV1 { std::array<std::uint8_t,240> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateTimeseriesValueCacheRequestV1(const SblrDdlCreateTimeseriesValueCacheRequestV1&);
bool DecodeSblrDdlCreateTimeseriesValueCacheRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateTimeseriesValueCacheRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(const SblrDdlCreateTimeseriesValueCacheDescriptorV1&,bool);
bool DecodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateTimeseriesValueCacheDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateTimeseriesValueCacheResultV1(const SblrDdlCreateTimeseriesValueCacheResultV1&);
bool DecodeSblrDdlCreateTimeseriesValueCacheResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateTimeseriesValueCacheResultV1*,std::string*);
}
