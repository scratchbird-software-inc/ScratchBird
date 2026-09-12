// SPDX-License-Identifier: MPL-2.0
// Actual private admission predicate, not a substitute executor.
#include "../../src/engine/executor/filter_executor.cpp"
#include <stdexcept>

void CheckFilterDefaultBinaryAuthority() {
  namespace ex = scratchbird::engine::executor;
  const auto check = [](bool ok) {
    if (!ok) throw std::runtime_error("filter exact-default authority boundary changed");
  };
  ex::CanonicalExecutionMgaAuthority authority;
  check(ex::CanonicalExecutionMgaAuthorityCarrierIsExactDefault(authority));
  for (const auto member : {&ex::PhysicalMgaStatementContext::statement_uuid,
                           &ex::PhysicalMgaStatementContext::owning_transaction_uuid,
                           &ex::PhysicalMgaStatementContext::statement_snapshot_uuid,
                           &ex::PhysicalMgaStatementContext::statement_metadata_snapshot_uuid}) {
    for (unsigned bit = 0; bit != 128; ++bit) {
      authority = {};
      (authority.statement_context.*member).bytes[bit / 8] = 1u << (bit % 8);
      check(!ex::CanonicalExecutionMgaAuthorityCarrierIsExactDefault(authority));
    }
  }
  authority = {};
  authority.statement_context.snapshot_kind.reserve(1024);
  check(!ex::CanonicalExecutionMgaAuthorityCarrierIsExactDefault(authority));
  authority = {};
  authority.statement_context.active_excluded_local_transaction_ids.reserve(3);
  check(!ex::CanonicalExecutionMgaAuthorityCarrierIsExactDefault(authority));
  authority = {};
  authority.statement_context.in_doubt_excluded_local_transaction_ids.reserve(3);
  check(!ex::CanonicalExecutionMgaAuthorityCarrierIsExactDefault(authority));
}
