#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDdlDropDictionaryRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence=0; std::uint32_t dictionary_occurrence=0; };
struct SblrDdlDropDictionaryDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability=0; };
struct SblrDdlDropDictionaryResultV1 { std::array<std::uint8_t,248> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability=0; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlDropDictionaryRequestV1(const SblrDdlDropDictionaryRequestV1&); bool DecodeSblrDdlDropDictionaryRequestV1(const std::uint8_t*,std::size_t,SblrDdlDropDictionaryRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlDropDictionaryDescriptorV1(const SblrDdlDropDictionaryDescriptorV1&,bool); bool DecodeSblrDdlDropDictionaryDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlDropDictionaryDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlDropDictionaryResultV1(const SblrDdlDropDictionaryResultV1&); bool DecodeSblrDdlDropDictionaryResultV1(const std::uint8_t*,std::size_t,SblrDdlDropDictionaryResultV1*,std::string*);
}
