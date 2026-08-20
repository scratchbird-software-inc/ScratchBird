#pragma once
#include "api_types.hpp"
#include <cstdint>
#include <string>
namespace scratchbird::engine::internal_api {
struct SblrTransactionBeginAuthorityV1{std::string isolation_profile_uuid,transaction_policy_snapshot_uuid;std::uint64_t isolation_profile_generation=0,transaction_policy_generation=0,deadline_monotonic_ns=0;std::uint8_t read_mode=1,authority_scope=1,wait_policy=1;};
struct SblrTransactionBeginAuthorityResultV1{bool ok=false;EngineApiDiagnostic diagnostic;SblrTransactionBeginAuthorityV1 authority;};
SblrTransactionBeginAuthorityResultV1 LoadSblrTransactionBeginAuthorityV1(const EngineRequestContext&);
}
