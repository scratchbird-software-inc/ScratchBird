#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using SblrTxnRollbackUuidV1=std::array<std::uint8_t,16>; using SblrTxnRollbackShaV1=std::array<std::uint8_t,32>;
struct SblrTransactionRollbackOptionsV1{SblrTxnRollbackUuidV1 transaction_uuid{};std::uint64_t local_transaction_id=0;SblrTxnRollbackShaV1 admitted_handle_evidence_sha256{};std::uint8_t rollback_mode=1,authority_scope=1,wait_policy=1;std::uint64_t deadline_monotonic_ns=0;SblrTxnRollbackShaV1 options_sha256{};};
struct SblrTransactionRollbackResultV1{SblrTxnRollbackUuidV1 transaction_uuid{},rollback_policy_snapshot_uuid{};std::uint64_t local_transaction_id=0,rollback_sequence=0,rollback_policy_generation=0;std::uint8_t lifecycle_state=3,authority_scope=1;SblrTxnRollbackShaV1 rollback_evidence_sha256{};std::uint64_t executor_availability_generation=0;};
std::vector<std::uint8_t> EncodeSblrTransactionRollbackOptionsV1(SblrTransactionRollbackOptionsV1*);
bool DecodeSblrTransactionRollbackOptionsV1(const std::uint8_t*,std::size_t,SblrTransactionRollbackOptionsV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrTransactionRollbackResultV1(const SblrTransactionRollbackResultV1&);
bool DecodeSblrTransactionRollbackResultV1(const std::uint8_t*,std::size_t,SblrTransactionRollbackResultV1*,std::string*);
}
