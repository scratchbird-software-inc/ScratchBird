#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using StatementBatchUuid=std::array<std::uint8_t,16>;using StatementBatchSha=std::array<std::uint8_t,32>;
struct SblrStatementBatchRequestV1{StatementBatchUuid receipt{};std::uint64_t occurrence=0;std::uint32_t batch_occurrence=0;};
struct SblrStatementBatchDescriptorV1{std::array<std::uint8_t,360> canonical_body{};StatementBatchSha evidence{};std::uint64_t availability_generation=0;};
struct SblrStatementBatchResultRecordV1{std::array<std::uint8_t,96> bytes{};};
struct SblrStatementBatchResultV1{StatementBatchUuid batch_uuid{};std::uint64_t batch_generation=0;StatementBatchUuid transaction_uuid{};StatementBatchSha committed_effect_set{};std::array<std::uint8_t,24> executor_evidence{};std::uint64_t availability_generation=0;std::vector<SblrStatementBatchResultRecordV1> records;};
std::vector<std::uint8_t> EncodeSblrStatementBatchRequestV1(const SblrStatementBatchRequestV1&);bool DecodeSblrStatementBatchRequestV1(const std::uint8_t*,std::size_t,SblrStatementBatchRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrStatementBatchDescriptorV1(const SblrStatementBatchDescriptorV1&,bool);bool DecodeSblrStatementBatchDescriptorV1(const std::uint8_t*,std::size_t,SblrStatementBatchDescriptorV1*,std::string*,bool);
std::vector<std::uint8_t> EncodeSblrStatementBatchResultV1(const SblrStatementBatchResultV1&);bool DecodeSblrStatementBatchResultV1(const std::uint8_t*,std::size_t,SblrStatementBatchResultV1*,std::string*);
}
