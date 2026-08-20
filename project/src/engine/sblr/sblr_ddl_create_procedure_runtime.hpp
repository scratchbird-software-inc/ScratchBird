#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using DdlCreateProcedureUuid=std::array<std::uint8_t,16>; using DdlCreateProcedureSha=std::array<std::uint8_t,32>;
struct SblrDdlCreateProcedureRequestV1 { DdlCreateProcedureUuid receipt{}; std::uint64_t occurrence=0; std::uint32_t procedure_occurrence=0; };
struct SblrDdlCreateProcedureDescriptorV1 { std::array<std::uint8_t,400> body{}; DdlCreateProcedureSha evidence{}; std::uint64_t availability=0; };
struct SblrDdlCreateProcedureResultV1 { std::array<std::uint8_t,240> body{}; DdlCreateProcedureSha evidence{}; std::uint64_t availability=0; DdlCreateProcedureUuid publication_barrier{}; };
std::vector<std::uint8_t> EncodeSblrDdlCreateProcedureRequestV1(const SblrDdlCreateProcedureRequestV1&);
bool DecodeSblrDdlCreateProcedureRequestV1(const std::uint8_t*,std::size_t,SblrDdlCreateProcedureRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrDdlCreateProcedureDescriptorV1(const SblrDdlCreateProcedureDescriptorV1&,bool);
bool DecodeSblrDdlCreateProcedureDescriptorV1(const std::uint8_t*,std::size_t,SblrDdlCreateProcedureDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrDdlCreateProcedureResultV1(const SblrDdlCreateProcedureResultV1&);
bool DecodeSblrDdlCreateProcedureResultV1(const std::uint8_t*,std::size_t,SblrDdlCreateProcedureResultV1*,std::string*);
}
