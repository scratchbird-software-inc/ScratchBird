#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDmlAsyncInsertStatusRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence{}; std::uint64_t status_occurrence{}; };
struct SblrDmlAsyncInsertStatusDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; };
struct SblrDmlAsyncInsertStatusResultV1 { std::array<std::uint8_t,240> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDmlAsyncInsertStatusRequestV1(const SblrDmlAsyncInsertStatusRequestV1&);
bool DecodeSblrDmlAsyncInsertStatusRequestV1(const std::uint8_t*,std::size_t,SblrDmlAsyncInsertStatusRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDmlAsyncInsertStatusDescriptorV1(const SblrDmlAsyncInsertStatusDescriptorV1&,bool);
bool DecodeSblrDmlAsyncInsertStatusDescriptorV1(const std::uint8_t*,std::size_t,SblrDmlAsyncInsertStatusDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDmlAsyncInsertStatusResultV1(const SblrDmlAsyncInsertStatusResultV1&);
bool DecodeSblrDmlAsyncInsertStatusResultV1(const std::uint8_t*,std::size_t,SblrDmlAsyncInsertStatusResultV1*,std::string*);
}
