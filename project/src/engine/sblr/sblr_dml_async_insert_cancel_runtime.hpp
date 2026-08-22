#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDmlAsyncInsertCancelRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence{}; std::uint64_t cancel_occurrence{}; };
struct SblrDmlAsyncInsertCancelDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; };
struct SblrDmlAsyncInsertCancelResultV1 { std::array<std::uint8_t,240> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDmlAsyncInsertCancelRequestV1(const SblrDmlAsyncInsertCancelRequestV1&);
bool DecodeSblrDmlAsyncInsertCancelRequestV1(const std::uint8_t*,std::size_t,SblrDmlAsyncInsertCancelRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDmlAsyncInsertCancelDescriptorV1(const SblrDmlAsyncInsertCancelDescriptorV1&,bool);
bool DecodeSblrDmlAsyncInsertCancelDescriptorV1(const std::uint8_t*,std::size_t,SblrDmlAsyncInsertCancelDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDmlAsyncInsertCancelResultV1(const SblrDmlAsyncInsertCancelResultV1&);
bool DecodeSblrDmlAsyncInsertCancelResultV1(const std::uint8_t*,std::size_t,SblrDmlAsyncInsertCancelResultV1*,std::string*);
}
