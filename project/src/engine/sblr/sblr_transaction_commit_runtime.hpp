#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::sblr {
using SblrTxnCommitUuidV1 = std::array<std::uint8_t, 16>;
using SblrTxnCommitShaV1 = std::array<std::uint8_t, 32>;
struct SblrTransactionCommitOptionsV1 {
  SblrTxnCommitUuidV1 transaction_uuid{};
  std::uint64_t local_transaction_id = 0;
  SblrTxnCommitShaV1 admitted_handle_evidence_sha256{};
  std::uint8_t commit_mode = 1, authority_scope = 1, wait_policy = 1;
  std::uint64_t deadline_monotonic_ns = 0;
  SblrTxnCommitShaV1 options_sha256{};
};
struct SblrTransactionCommitResultV1 {
  SblrTxnCommitUuidV1 transaction_uuid{}, commit_policy_snapshot_uuid{};
  std::uint64_t local_transaction_id = 0, commit_sequence = 0;
  std::uint64_t commit_policy_generation = 0;
  std::uint8_t lifecycle_state = 2, authority_scope = 1;
  SblrTxnCommitShaV1 commit_evidence_sha256{};
  std::uint64_t executor_availability_generation = 0;
};
std::vector<std::uint8_t> EncodeSblrTransactionCommitOptionsV1(
    SblrTransactionCommitOptionsV1*);
bool DecodeSblrTransactionCommitOptionsV1(const std::uint8_t*, std::size_t,
                                          SblrTransactionCommitOptionsV1*,
                                          std::string*);
std::vector<std::uint8_t> EncodeSblrTransactionCommitResultV1(
    const SblrTransactionCommitResultV1&);
bool DecodeSblrTransactionCommitResultV1(const std::uint8_t*, std::size_t,
                                         SblrTransactionCommitResultV1*,
                                         std::string*);
}
