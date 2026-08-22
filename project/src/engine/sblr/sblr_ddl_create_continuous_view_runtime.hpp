#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDdlCreateContinuousViewRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence=0; std::uint64_t view_occurrence=0; };
struct SblrDdlCreateContinuousViewDescriptorV1 { std::array<std::uint8_t,488> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateContinuousViewResultV1 { std::array<std::uint8_t,240> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability=0; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateContinuousViewRequestV1(const SblrDdlCreateContinuousViewRequestV1&);
bool DecodeSblrDdlCreateContinuousViewRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateContinuousViewRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateContinuousViewDescriptorV1(const SblrDdlCreateContinuousViewDescriptorV1&,bool operand);
bool DecodeSblrDdlCreateContinuousViewDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateContinuousViewDescriptorV1*,std::string*,bool operand);
std::vector<std::uint8_t> EncodeSblrDdlCreateContinuousViewResultV1(const SblrDdlCreateContinuousViewResultV1&);
bool DecodeSblrDdlCreateContinuousViewResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateContinuousViewResultV1*,std::string*);
}
