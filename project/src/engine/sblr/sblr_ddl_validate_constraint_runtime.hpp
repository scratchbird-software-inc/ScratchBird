#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlValidateConstraintUuid=std::array<std::uint8_t,16>; using DdlValidateConstraintSha=std::array<std::uint8_t,32>;
struct SblrDdlValidateConstraintRequestV1 { DdlValidateConstraintUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t constraint_occurrence=0; };
struct SblrDdlValidateConstraintDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlValidateConstraintSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlValidateConstraintResultV1 { std::array<std::uint8_t,240> body{}; DdlValidateConstraintSha evidence{}; std::uint64_t availability=0; DdlValidateConstraintUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlValidateConstraintRequestV1(const SblrDdlValidateConstraintRequestV1&);
bool DecodeSblrDdlValidateConstraintRequestV1(const std::uint8_t*,std::size_t,SblrDdlValidateConstraintRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlValidateConstraintDescriptorV1(const SblrDdlValidateConstraintDescriptorV1&,bool);
bool DecodeSblrDdlValidateConstraintDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlValidateConstraintDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlValidateConstraintResultV1(const SblrDdlValidateConstraintResultV1&);
bool DecodeSblrDdlValidateConstraintResultV1(const std::uint8_t*,std::size_t,SblrDdlValidateConstraintResultV1*,std::string*);
}
