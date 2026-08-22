#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::sblr {
struct SblrDdlTimeseriesSeriesCardinalityPolicyRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence{}; std::uint64_t policy_occurrence{}; };
struct SblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; };
struct SblrDdlTimeseriesSeriesCardinalityPolicyResultV1 { std::array<std::uint8_t,240> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlTimeseriesSeriesCardinalityPolicyRequestV1(const SblrDdlTimeseriesSeriesCardinalityPolicyRequestV1&);
bool DecodeSblrDdlTimeseriesSeriesCardinalityPolicyRequestV1(const std::uint8_t*,std::size_t,SblrDdlTimeseriesSeriesCardinalityPolicyRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1(const SblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1&,bool);
bool DecodeSblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlTimeseriesSeriesCardinalityPolicyResultV1(const SblrDdlTimeseriesSeriesCardinalityPolicyResultV1&);
bool DecodeSblrDdlTimeseriesSeriesCardinalityPolicyResultV1(const std::uint8_t*,std::size_t,SblrDdlTimeseriesSeriesCardinalityPolicyResultV1*,std::string*);
}
