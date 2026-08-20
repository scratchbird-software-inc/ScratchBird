#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {using SpUuid=std::array<std::uint8_t,16>;using SpSha=std::array<std::uint8_t,32>;
struct SblrSavepointCoordinationRequestV1{SpUuid preliminary_receipt_uuid{},transaction_uuid{};std::uint64_t local_transaction_id=0;SpSha transaction_handle_evidence_sha256{};std::uint64_t symbol_occurrence_id=0;SpSha canonical_symbol_sha256{};};
struct SblrSavepointCoordinationResultV1{SpUuid preliminary_receipt_uuid{},descriptor_uuid{},savepoint_uuid{};std::uint64_t descriptor_generation=0,savepoint_generation=0,transaction_ordinal=0,symbol_occurrence_id=0;SpSha canonical_symbol_sha256{},descriptor_evidence_sha256{};};
struct SblrSavepointDescriptorV1{SpUuid descriptor_uuid{},savepoint_uuid{},transaction_uuid{};std::uint64_t descriptor_generation=0,savepoint_generation=0,local_transaction_id=0,transaction_ordinal=0;SpSha descriptor_evidence_sha256{};};
struct SblrSavepointHandleV1{SpUuid savepoint_uuid{},transaction_uuid{};std::uint64_t savepoint_generation=0,local_transaction_id=0,transaction_ordinal=0,stack_generation=0;std::uint8_t lifecycle_state=1;SpSha savepoint_evidence_sha256{};std::uint64_t executor_availability_generation=0;};
struct SblrSavepointReleaseOperandV1{SpUuid savepoint_uuid{},transaction_uuid{};std::uint64_t savepoint_generation=0,local_transaction_id=0,transaction_ordinal=0,admitted_stack_generation=0;SpSha admitted_savepoint_evidence_sha256{};std::uint64_t executor_availability_generation=0;};
struct SblrSavepointReleaseResultV1{SpUuid transaction_uuid{},released_savepoint_uuid{};std::uint64_t local_transaction_id=0,released_savepoint_generation=0,transaction_ordinal=0,resulting_stack_generation=0;SpSha release_evidence_sha256{};std::uint64_t executor_availability_generation=0;};
struct SblrSavepointRollbackOperandV1{SpUuid savepoint_uuid{},transaction_uuid{};std::uint64_t savepoint_generation=0,local_transaction_id=0,transaction_ordinal=0,admitted_stack_generation=0;SpSha admitted_savepoint_evidence_sha256{};std::uint64_t executor_availability_generation=0;};
struct SblrSavepointRollbackResultV1{SpUuid transaction_uuid{},target_savepoint_uuid{};std::uint64_t local_transaction_id=0,target_savepoint_generation=0,transaction_ordinal=0,resulting_stack_generation=0,rollback_sequence=0;std::uint8_t target_lifecycle_state=1;SpSha refreshed_savepoint_evidence_sha256{},rollback_evidence_sha256{};std::uint64_t executor_availability_generation=0;};
std::vector<std::uint8_t> EncodeSblrSavepointDescriptorV1(const SblrSavepointDescriptorV1&);bool DecodeSblrSavepointDescriptorV1(const std::uint8_t*,std::size_t,SblrSavepointDescriptorV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSavepointHandleV1(const SblrSavepointHandleV1&);bool DecodeSblrSavepointHandleV1(const std::uint8_t*,std::size_t,SblrSavepointHandleV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSavepointReleaseOperandV1(const SblrSavepointReleaseOperandV1&);bool DecodeSblrSavepointReleaseOperandV1(const std::uint8_t*,std::size_t,SblrSavepointReleaseOperandV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSavepointReleaseResultV1(const SblrSavepointReleaseResultV1&);bool DecodeSblrSavepointReleaseResultV1(const std::uint8_t*,std::size_t,SblrSavepointReleaseResultV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSavepointRollbackOperandV1(const SblrSavepointRollbackOperandV1&);bool DecodeSblrSavepointRollbackOperandV1(const std::uint8_t*,std::size_t,SblrSavepointRollbackOperandV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSavepointRollbackResultV1(const SblrSavepointRollbackResultV1&);bool DecodeSblrSavepointRollbackResultV1(const std::uint8_t*,std::size_t,SblrSavepointRollbackResultV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSavepointCoordinationRequestV1(const SblrSavepointCoordinationRequestV1&);bool DecodeSblrSavepointCoordinationRequestV1(const std::uint8_t*,std::size_t,SblrSavepointCoordinationRequestV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrSavepointCoordinationResultV1(const SblrSavepointCoordinationResultV1&);bool DecodeSblrSavepointCoordinationResultV1(const std::uint8_t*,std::size_t,SblrSavepointCoordinationResultV1*,std::string*);
}
