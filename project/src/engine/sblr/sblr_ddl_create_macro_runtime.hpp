#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDdlCreateMacroRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence=0; std::uint32_t macro_occurrence=0; };
struct SblrDdlCreateMacroDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateMacroResultV1 { std::array<std::uint8_t,248> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability=0; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateMacroRequestV1(const SblrDdlCreateMacroRequestV1&); bool DecodeSblrDdlCreateMacroRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateMacroRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateMacroDescriptorV1(const SblrDdlCreateMacroDescriptorV1&,bool); bool DecodeSblrDdlCreateMacroDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateMacroDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateMacroResultV1(const SblrDdlCreateMacroResultV1&); bool DecodeSblrDdlCreateMacroResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateMacroResultV1*,std::string*);
}
