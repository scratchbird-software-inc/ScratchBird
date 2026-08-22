#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDmlAsyncInsertSubmitRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence{}; std::uint64_t submission_occurrence{}; };
struct SblrDmlAsyncInsertSubmitDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; };
struct SblrDmlAsyncInsertSubmitResultV1 { std::array<std::uint8_t,240> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability{}; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDmlAsyncInsertSubmitRequestV1(const SblrDmlAsyncInsertSubmitRequestV1&);
bool DecodeSblrDmlAsyncInsertSubmitRequestV1(const std::uint8_t*,std::size_t,SblrDmlAsyncInsertSubmitRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDmlAsyncInsertSubmitDescriptorV1(const SblrDmlAsyncInsertSubmitDescriptorV1&,bool);
bool DecodeSblrDmlAsyncInsertSubmitDescriptorV1(const std::uint8_t*,std::size_t,SblrDmlAsyncInsertSubmitDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDmlAsyncInsertSubmitResultV1(const SblrDmlAsyncInsertSubmitResultV1&);
bool DecodeSblrDmlAsyncInsertSubmitResultV1(const std::uint8_t*,std::size_t,SblrDmlAsyncInsertSubmitResultV1*,std::string*);
}
