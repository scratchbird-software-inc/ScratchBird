#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrDdlAlterDictionaryRequestV1 { std::array<std::uint8_t,16> receipt{}; std::uint64_t occurrence=0; std::uint32_t dictionary_occurrence=0; };
struct SblrDdlAlterDictionaryDescriptorV1 { std::array<std::uint8_t,400> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability=0; };
struct SblrDdlAlterDictionaryResultV1 { std::array<std::uint8_t,248> body{}; std::array<std::uint8_t,32> evidence{}; std::uint64_t availability=0; std::array<std::uint8_t,16> publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlAlterDictionaryRequestV1(const SblrDdlAlterDictionaryRequestV1&); bool DecodeSblrDdlAlterDictionaryRequestV1(const std::uint8_t*,std::size_t,SblrDdlAlterDictionaryRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlAlterDictionaryDescriptorV1(const SblrDdlAlterDictionaryDescriptorV1&,bool); bool DecodeSblrDdlAlterDictionaryDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlAlterDictionaryDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlAlterDictionaryResultV1(const SblrDdlAlterDictionaryResultV1&); bool DecodeSblrDdlAlterDictionaryResultV1(const std::uint8_t*,std::size_t,SblrDdlAlterDictionaryResultV1*,std::string*);
}
