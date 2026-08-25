#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
struct SblrCreateTableAsQueryRequestV1 { std::array<std::uint8_t,16> receipt{}, occurrence{}; std::uint32_t occurrence_ordinal{}; std::uint32_t flags{}; std::uint64_t catalog_epoch{}; std::uint64_t authority_generation{}; };
struct SblrCreateTableAsQueryDescriptorV1 { std::uint16_t opcode{}; std::uint16_t flags{}; std::array<std::uint8_t,16> table_uuid{}, query_plan_uuid{}; std::uint64_t catalog_epoch{}; std::vector<std::uint8_t> query_plan; std::vector<std::uint8_t> columns; std::vector<std::uint8_t> authority_proof; };
struct SblrCreateTableAsQueryResultV1 { std::uint8_t status{}; std::uint8_t materialization{}; std::uint16_t flags{}; std::array<std::uint8_t,16> table_uuid{}, publication_uuid{}; std::uint64_t catalog_epoch{}, row_count{}; std::array<std::uint8_t,32> result_hash{}, diagnostic_hash{}; };
std::vector<std::uint8_t> EncodeSblrCreateTableAsQueryRequestV1(const SblrCreateTableAsQueryRequestV1&);
bool DecodeSblrCreateTableAsQueryRequestV1(const std::uint8_t*,std::size_t,SblrCreateTableAsQueryRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrCreateTableAsQueryDescriptorV1(const SblrCreateTableAsQueryDescriptorV1&);
bool DecodeSblrCreateTableAsQueryDescriptorV1(const std::uint8_t*,std::size_t,SblrCreateTableAsQueryDescriptorV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrCreateTableAsQueryResultV1(const SblrCreateTableAsQueryResultV1&);
bool DecodeSblrCreateTableAsQueryResultV1(const std::uint8_t*,std::size_t,SblrCreateTableAsQueryResultV1*,std::string*);
}
