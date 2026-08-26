#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDdlAlterTimeseriesValueCacheRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence{}; std::uint64_t cache_occurrence{}; };
struct SblrDdlAlterTimeseriesValueCacheDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; };
struct SblrDdlAlterTimeseriesValueCacheResultV1 { std::array<std::uint8_t,240> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlAlterTimeseriesValueCacheRequestV1(const SblrDdlAlterTimeseriesValueCacheRequestV1&);
bool DecodeSblrDdlAlterTimeseriesValueCacheRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterTimeseriesValueCacheRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterTimeseriesValueCacheDescriptorV1(const SblrDdlAlterTimeseriesValueCacheDescriptorV1&,bool);
bool DecodeSblrDdlAlterTimeseriesValueCacheDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterTimeseriesValueCacheDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlAlterTimeseriesValueCacheResultV1(const SblrDdlAlterTimeseriesValueCacheResultV1&);
bool DecodeSblrDdlAlterTimeseriesValueCacheResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterTimeseriesValueCacheResultV1*,std::string*);
}
