// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// BindStatementTableTruncateTargetV1 refusal contract.
//
// SBSQL-TRUNCATE-TABLE-V1 section 6.1 requires the receipt to resolve to the
// authenticated preliminary statement and its admitted truncate occurrence,
// and says UUID shape alone is not that authority. Every case below is a
// refusal that must happen BEFORE any catalog resolution, so none of them
// needs a database, a table or a live receipt: a bind that reached resolution
// with a bad receipt or an unadmitted clause would already have leaked the
// decision.
//
// SCOPE, stated so this gate is not mistaken for more than it is: this proves
// the fail-closed half only. The resolve-and-publish half -- that a valid bind
// publishes occurrence -> persisted descriptor onto the receipt view -- needs a
// live receipt over a real table and is NOT covered here.

#include "server_engine_bridge/statement_context.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
namespace bridge = scratchbird::server_engine_bridge;
namespace api = scratchbird::engine::internal_api;

int checks = 0;
void Check(bool pass, const char* what) {
  ++checks;
  if (!pass) {
    std::fprintf(stderr, "FAIL table truncate bind target: %s\n", what);
    std::exit(EXIT_FAILURE);
  }
}

api::EngineUuid Id(unsigned tail) {
  api::EngineUuid value{};
  value.bytes[0] = 1;
  value.bytes[6] = 0x70;
  value.bytes[8] = 0x80;
  value.bytes[14] = static_cast<std::uint8_t>(tail >> 8);
  value.bytes[15] = static_cast<std::uint8_t>(tail);
  return value;
}

bridge::StatementTableTruncateBindRequestV1 Request() {
  bridge::StatementTableTruncateBindRequestV1 request;
  request.authenticated_receipt_uuid = Id(0x701);
  request.structural_occurrence = 1;
  request.truncate_occurrence = 1;
  request.target_name_atoms.push_back({"orders", false});
  request.identity_action = 1;   // CONTINUE IDENTITY, the section 2 default
  request.dependency_action = 1;  // RESTRICT, the section 2 default
  return request;
}

// A handle whose opaque id names no live receipt. Registry lookup must refuse
// it; the id is never reused, so this can never collide with a real receipt.
bridge::StatementContextReceiptHandle AbsentReceipt() {
  bridge::StatementContextReceiptHandle handle;
  handle.opaque_id = 0xfffffffffffffffeull;
  return handle;
}

struct Outcome {
  sb_engine_status_t status = SB_ENGINE_STATUS_OK;
  bridge::StatementTableTruncateBindAckV1 ack;
};

Outcome Bind(bridge::StatementContextReceiptHandle handle,
             const bridge::StatementTableTruncateBindRequestV1* request) {
  Outcome out;
  sb_engine_result_t result{};
  out.status = bridge::BindStatementTableTruncateTargetV1(
      handle, request, &out.ack, &result);
  return out;
}

// A refusal must publish nothing: no resolved identity may appear on the ack,
// and the ack must carry its cause.
void CheckRefused(const Outcome& out, sb_engine_status_t expected,
                  const char* expected_key, const char* what) {
  Check(out.status == expected, what);
  Check(out.ack.resolved_table_uuid == api::EngineUuid{},
        "a refused bind published a resolved table identity");
  Check(out.ack.persisted_descriptor_uuid == api::EngineUuid{},
        "a refused bind published a persisted descriptor identity");
  Check(out.ack.persisted_descriptor_generation == 0,
        "a refused bind published a descriptor generation");
  Check(!out.ack.failure_code.empty(), "a refused bind carries no failure code");
  Check(out.ack.failure_message_key == expected_key,
        "a refused bind reported an unexpected message key");
}
}  // namespace

int main() {
  // 1. Argument contract.
  {
    const auto request = Request();
    sb_engine_result_t result{};
    Check(bridge::BindStatementTableTruncateTargetV1(
              AbsentReceipt(), &request, nullptr, &result) ==
              SB_ENGINE_STATUS_INVALID_ARGUMENT,
          "a null ack was accepted");
    bridge::StatementTableTruncateBindAckV1 ack;
    Check(bridge::BindStatementTableTruncateTargetV1(
              AbsentReceipt(), nullptr, &ack, &result) ==
              SB_ENGINE_STATUS_INVALID_ARGUMENT,
          "a null request was accepted");
    bridge::StatementContextReceiptHandle unset;
    Check(bridge::BindStatementTableTruncateTargetV1(
              unset, &request, &ack, &result) ==
              SB_ENGINE_STATUS_INVALID_ARGUMENT,
          "an unset receipt handle was accepted");
  }

  // 2. The occurrences the later carrier resolves against must be present.
  //    Zero is the "absent" value in every truncate carrier field, so a zero
  //    occurrence cannot be bound.
  {
    auto zero_structural = Request();
    zero_structural.structural_occurrence = 0;
    CheckRefused(Bind(AbsentReceipt(), &zero_structural),
                 SB_ENGINE_STATUS_INVALID_ARGUMENT,
                 "sblr.table_truncate.bind_request_invalid",
                 "a zero structural occurrence was bound");

    auto zero_truncate = Request();
    zero_truncate.truncate_occurrence = 0;
    CheckRefused(Bind(AbsentReceipt(), &zero_truncate),
                 SB_ENGINE_STATUS_INVALID_ARGUMENT,
                 "sblr.table_truncate.bind_request_invalid",
                 "a zero truncate occurrence was bound");

    auto no_target = Request();
    no_target.target_name_atoms.clear();
    CheckRefused(Bind(AbsentReceipt(), &no_target),
                 SB_ENGINE_STATUS_INVALID_ARGUMENT,
                 "sblr.table_truncate.bind_request_invalid",
                 "an empty target list was bound");

    auto empty_atom = Request();
    empty_atom.target_name_atoms = {{"", false}};
    CheckRefused(Bind(AbsentReceipt(), &empty_atom),
                 SB_ENGINE_STATUS_INVALID_ARGUMENT,
                 "sblr.table_truncate.bind_request_invalid",
                 "an empty target name atom was bound");
  }

  // 3. Clauses the grammar admits but this version does not resolve refuse
  //    explicitly. Section 5 forbids a partial target set, so resolving one
  //    target of a CASCADE closure is not an option; refusing is.
  {
    auto restart = Request();
    restart.identity_action = 2;  // RESTART IDENTITY
    CheckRefused(Bind(AbsentReceipt(), &restart), SB_ENGINE_STATUS_UNSUPPORTED,
                 "sblr.table_truncate.bind_clause_not_available",
                 "RESTART IDENTITY was silently bound as CONTINUE");

    auto cascade = Request();
    cascade.dependency_action = 2;  // CASCADE
    CheckRefused(Bind(AbsentReceipt(), &cascade), SB_ENGINE_STATUS_UNSUPPORTED,
                 "sblr.table_truncate.bind_clause_not_available",
                 "CASCADE was silently bound as RESTRICT");

    for (const std::uint8_t action : {0, 3, 255}) {
      auto identity = Request();
      identity.identity_action = action;
      CheckRefused(Bind(AbsentReceipt(), &identity),
                   SB_ENGINE_STATUS_UNSUPPORTED,
                   "sblr.table_truncate.bind_clause_not_available",
                   "an out-of-range identity action was bound");
      auto dependency = Request();
      dependency.dependency_action = action;
      CheckRefused(Bind(AbsentReceipt(), &dependency),
                   SB_ENGINE_STATUS_UNSUPPORTED,
                   "sblr.table_truncate.bind_clause_not_available",
                   "an out-of-range dependency action was bound");
    }
  }

  // 4. A well-formed request against a receipt that does not exist is hidden,
  //    not "not found": the refusal must not distinguish an absent receipt
  //    from one belonging to another session.
  {
    const auto request = Request();
    CheckRefused(Bind(AbsentReceipt(), &request),
                 SB_ENGINE_STATUS_SECURITY_DENIED,
                 "sblr.table_truncate.bind_hidden",
                 "an absent receipt was not refused as hidden");
  }

  std::printf("PASS table truncate bind target checks=%d\n", checks);
  return 0;
}
