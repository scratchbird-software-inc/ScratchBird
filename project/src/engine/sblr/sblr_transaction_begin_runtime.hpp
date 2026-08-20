#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using SblrTxnUuidV1=std::array<std::uint8_t,16>;using SblrTxnShaV1=std::array<std::uint8_t,32>;
struct SblrTransactionBeginOptionsV1{SblrTxnUuidV1 isolation_profile_uuid{},transaction_policy_snapshot_uuid{};std::uint64_t isolation_profile_generation=0,transaction_policy_generation=0,deadline_monotonic_ns=0;std::uint8_t read_mode=0,authority_scope=0,wait_policy=0;SblrTxnShaV1 options_sha256{};};
struct SblrTransactionHandleV1{SblrTxnUuidV1 transaction_uuid{},snapshot_uuid{},isolation_profile_uuid{},transaction_policy_snapshot_uuid{};std::uint64_t local_transaction_id=0,isolation_profile_generation=0,transaction_policy_generation=0,executor_availability_generation=0;std::uint8_t read_mode=0,lifecycle_state=1,authority_scope=0;SblrTxnShaV1 handle_evidence_sha256{};};
std::vector<std::uint8_t> EncodeSblrTransactionBeginOptionsV1(SblrTransactionBeginOptionsV1*);
bool DecodeSblrTransactionBeginOptionsV1(const std::uint8_t*,std::size_t,SblrTransactionBeginOptionsV1*,std::string*);
std::vector<std::uint8_t> EncodeSblrTransactionHandleV1(const SblrTransactionHandleV1&);
bool DecodeSblrTransactionHandleV1(const std::uint8_t*,std::size_t,SblrTransactionHandleV1*,std::string*);
}
