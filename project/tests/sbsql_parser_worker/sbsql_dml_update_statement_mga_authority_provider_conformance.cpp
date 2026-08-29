// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_statement_mga_authority_provider.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace engine_api = scratchbird::engine::internal_api;

constexpr std::string_view kDatabaseUuid =
    "019d5200-0000-7000-8000-000000000001";
constexpr std::string_view kTransactionUuid =
    "019d5200-0000-7000-8000-000000000002";
constexpr std::string_view kOtherTransactionUuid =
    "019d5200-0000-7000-8000-000000000003";
constexpr std::string_view kReceiptUuid =
    "019d5200-0000-7000-8000-000000000004";
constexpr std::string_view kOtherReceiptUuid =
    "019d5200-0000-7000-8000-000000000005";
constexpr std::string_view kOperationUuid =
    "019d5200-0000-7000-8000-000000000006";
constexpr std::string_view kOtherOperationUuid =
    "019d5200-0000-7000-8000-000000000007";
constexpr std::string_view kDescriptorUuid =
    "019d5200-0000-7000-8000-000000000008";
constexpr std::string_view kOtherDescriptorUuid =
    "019d5200-0000-7000-8000-000000000009";
constexpr std::string_view kRecoveryUuid =
    "019d5200-0000-7000-8000-00000000000a";
constexpr std::string_view kOtherRecoveryUuid =
    "019d5200-0000-7000-8000-00000000000b";
constexpr std::string_view kReservedBarrierUuid =
    "019d5200-0000-7000-8000-00000000000e";
constexpr std::string_view kOtherReservedBarrierUuid =
    "019d5200-0000-7000-8000-00000000000f";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

void RequireDiagnostic(const engine_api::EngineApiDiagnostic& diagnostic,
                       std::string_view code, std::string_view message) {
  if (!diagnostic.error || diagnostic.code != code) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
    Fail(message);
  }
}

bool Nonzero(const engine_api::MgaDmlUpdateStatementAuthoritySha256V1& sha) {
  return std::any_of(sha.begin(), sha.end(),
                     [](std::uint8_t value) { return value != 0; });
}

bool ExactNonzeroUuid(std::string_view text) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  return parsed.ok() &&
         !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == text;
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/sb_update_statement_mga_authority.XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* made = ::mkdtemp(writable.data());
    Require(made != nullptr,
            "mkdtemp failed for UPDATE MGA authority test");
    path_ = made;
  }

  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

engine_api::EngineDmlUpdateStatementMgaAuthorityOpenRequestV1 Request(
    const std::filesystem::path& database_path) {
  engine_api::EngineDmlUpdateStatementMgaAuthorityOpenRequestV1 request;
  request.context.trust_mode =
      engine_api::EngineTrustMode::server_isolated;
  request.context.database_path = database_path.string();
  request.context.database_uuid.canonical = std::string(kDatabaseUuid);
  request.context.transaction_uuid.canonical = std::string(kTransactionUuid);
  request.context.local_transaction_id = 901;
  request.context.statement_receipt_uuid.canonical =
      std::string(kReceiptUuid);
  request.context.statement_snapshot_uuid.canonical =
      "019d5200-0000-7000-8000-00000000000c";
  request.context.statement_metadata_snapshot_engine_owned = true;
  request.context.statement_metadata_snapshot_uuid.canonical =
      "019d5200-0000-7000-8000-00000000000d";
  request.context.security_context_present = true;
  request.context.authorization_context.present = true;
  request.context.authorization_context.authority_uuid.canonical =
      std::string(kDatabaseUuid);
  request.context.authorization_context.security_context_generation = 1;
  request.context.trace_tags.push_back("private_dml_update_rows_consumer");
  request.authenticated_statement_receipt_uuid = std::string(kReceiptUuid);
  request.operation_uuid = std::string(kOperationUuid);
  request.descriptor_uuid = std::string(kDescriptorUuid);
  request.descriptor_generation = 17;
  request.recovery_token_uuid = std::string(kRecoveryUuid);
  request.recovery_generation = 23;
  request.reserved_publication_barrier_uuid =
      std::string(kReservedBarrierUuid);
  request.reserved_publication_barrier_generation = 1;
  return request;
}

engine_api::EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1
Transition(
    const engine_api::EngineDmlUpdateStatementMgaAuthorityOpenRequestV1&
        current,
    const engine_api::MgaDmlUpdateStatementSavepointAuthorityV1& admitted) {
  engine_api::EngineDmlUpdateStatementMgaAuthorityTransitionRequestV1 result;
  result.current = current;
  result.admitted = admitted;
  return result;
}

engine_api::EngineDmlUpdateStatementMgaAuthorityRecoverRequestV1 Recover(
    const engine_api::EngineDmlUpdateStatementMgaAuthorityOpenRequestV1&
        current,
    const engine_api::MgaDmlUpdateStatementSavepointAuthorityV1& admitted) {
  engine_api::EngineDmlUpdateStatementMgaAuthorityRecoverRequestV1 result;
  result.current = current;
  result.current.context.trace_tags.clear();
  result.current.context.trace_tags.push_back(
      "private_dml_update_rows_recovery");
  result.savepoint_uuid = admitted.savepoint_uuid;
  result.savepoint_generation = admitted.savepoint_generation;
  return result;
}

std::string PrivateMarker(std::string_view uuid) {
  std::string marker = "__sblr_dml_update_rows_";
  for (const char value : uuid) {
    if (value != '-') marker.push_back(value);
  }
  return marker;
}

std::string HexEncode(std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(text.size() * 2);
  for (const unsigned char value : text) {
    encoded.push_back(kHex[value >> 4]);
    encoded.push_back(kHex[value & 0x0f]);
  }
  return encoded;
}

void AppendContradictoryActiveMarker(
    const std::filesystem::path& database_path,
    const engine_api::MgaDmlUpdateStatementSavepointAuthorityV1& released) {
  std::ofstream out(database_path.string() + ".sb.mga_savepoints",
                    std::ios::app | std::ios::binary);
  Require(static_cast<bool>(out),
          "could not open MGA savepoint journal for contradiction fixture");
  out << "SBMGA1\tSAVEPOINT\t"
      << released.binding.owning_local_transaction_id << '\t'
      << HexEncode(PrivateMarker(released.savepoint_uuid))
      << "\t0\t0\t0\n";
  out.flush();
  Require(static_cast<bool>(out),
          "could not append contradictory active savepoint fixture");
}

void TestFreshIdentityAndStaleRefusals(
    const std::filesystem::path& database_path) {
  const auto request = Request(database_path);
  const auto first = engine_api::OpenDmlUpdateStatementMgaAuthorityV1(request);
  auto second_request = request;
  second_request.operation_uuid = std::string(kOtherOperationUuid);
  second_request.descriptor_uuid = std::string(kOtherDescriptorUuid);
  second_request.recovery_token_uuid = std::string(kOtherRecoveryUuid);
  second_request.reserved_publication_barrier_uuid =
      std::string(kOtherReservedBarrierUuid);
  const auto second =
      engine_api::OpenDmlUpdateStatementMgaAuthorityV1(second_request);
  Require(first.ok && second.ok,
          "fresh UPDATE MGA savepoint creation was refused");
  Require(ExactNonzeroUuid(first.authority.savepoint_uuid) &&
              first.authority.savepoint_generation == 1 &&
              first.authority.lifecycle ==
                  engine_api::MgaDmlUpdateStatementSavepointLifecycleV1::active &&
              ExactNonzeroUuid(
                  first.authority.publication_barrier_uuid) &&
              first.authority.publication_barrier_generation == 1 &&
              !first.authority.publication_barrier_present &&
              first.authority.publication_barrier_uuid ==
                  request.reserved_publication_barrier_uuid &&
              first.authority.publication_barrier_uuid !=
                  first.authority.savepoint_uuid &&
              Nonzero(first.authority.durable_presence_sha256),
          "fresh savepoint/reserved-barrier identity or durable proof is invalid");
  Require(first.authority.savepoint_uuid != second.authority.savepoint_uuid &&
              first.authority.publication_barrier_uuid !=
                  second.authority.publication_barrier_uuid &&
              second.authority.publication_barrier_uuid ==
                  second_request.reserved_publication_barrier_uuid,
          "MGA reused a statement savepoint or reserved barrier UUID");
  Require(engine_api::RevalidateDmlUpdateStatementMgaAuthorityV1(
              Transition(request, first.authority))
              .ok,
          "unchanged active MGA authority failed revalidation");

  auto cross_receipt = request;
  cross_receipt.authenticated_statement_receipt_uuid =
      std::string(kOtherReceiptUuid);
  cross_receipt.context.statement_receipt_uuid.canonical =
      std::string(kOtherReceiptUuid);
  const auto receipt_refused =
      engine_api::RevalidateDmlUpdateStatementMgaAuthorityV1(
          Transition(cross_receipt, first.authority));
  RequireDiagnostic(receipt_refused.diagnostic,
                    engine_api::kDmlUpdateStatementMgaDiagnosticStale,
                    "cross-receipt MGA savepoint was accepted");

  auto cross_transaction = request;
  cross_transaction.context.transaction_uuid.canonical =
      std::string(kOtherTransactionUuid);
  cross_transaction.context.local_transaction_id = 902;
  const auto transaction_refused =
      engine_api::RevalidateDmlUpdateStatementMgaAuthorityV1(
          Transition(cross_transaction, first.authority));
  RequireDiagnostic(transaction_refused.diagnostic,
                    engine_api::kDmlUpdateStatementMgaDiagnosticStale,
                    "cross-transaction MGA savepoint was accepted");

  auto cross_operation = request;
  cross_operation.operation_uuid = std::string(kOtherOperationUuid);
  const auto operation_refused =
      engine_api::RevalidateDmlUpdateStatementMgaAuthorityV1(
          Transition(cross_operation, first.authority));
  RequireDiagnostic(operation_refused.diagnostic,
                    engine_api::kDmlUpdateStatementMgaDiagnosticStale,
                    "cross-operation MGA savepoint was accepted");

  auto cross_descriptor = request;
  cross_descriptor.descriptor_uuid = std::string(kOtherDescriptorUuid);
  const auto descriptor_refused =
      engine_api::RevalidateDmlUpdateStatementMgaAuthorityV1(
          Transition(cross_descriptor, first.authority));
  RequireDiagnostic(descriptor_refused.diagnostic,
                    engine_api::kDmlUpdateStatementMgaDiagnosticStale,
                    "cross-descriptor MGA savepoint was accepted");

  auto cross_recovery = request;
  cross_recovery.recovery_token_uuid = std::string(kOtherRecoveryUuid);
  const auto recovery_refused =
      engine_api::RevalidateDmlUpdateStatementMgaAuthorityV1(
          Transition(cross_recovery, first.authority));
  RequireDiagnostic(recovery_refused.diagnostic,
                    engine_api::kDmlUpdateStatementMgaDiagnosticStale,
                    "cross-recovery MGA savepoint was accepted");

  auto cross_barrier = request;
  cross_barrier.reserved_publication_barrier_uuid =
      std::string(kOtherReservedBarrierUuid);
  const auto barrier_refused =
      engine_api::RevalidateDmlUpdateStatementMgaAuthorityV1(
          Transition(cross_barrier, first.authority));
  RequireDiagnostic(barrier_refused.diagnostic,
                    engine_api::kDmlUpdateStatementMgaDiagnosticStale,
                    "cross-reservation MGA barrier was accepted");

  auto forged_generation = first.authority;
  forged_generation.savepoint_generation = 2;
  const auto generation_refused =
      engine_api::RevalidateDmlUpdateStatementMgaAuthorityV1(
          Transition(request, forged_generation));
  RequireDiagnostic(generation_refused.diagnostic,
                    engine_api::kDmlUpdateStatementMgaDiagnosticStale,
                    "forged savepoint generation was accepted");

  auto forged_proof = first.authority;
  forged_proof.durable_presence_sha256[0] ^= 0xff;
  const auto proof_refused =
      engine_api::RevalidateDmlUpdateStatementMgaAuthorityV1(
          Transition(request, forged_proof));
  RequireDiagnostic(proof_refused.diagnostic,
                    engine_api::kDmlUpdateStatementMgaDiagnosticStale,
                    "forged durable presence proof was accepted");
}

void TestRollbackAndRecovery(const std::filesystem::path& database_path) {
  const auto request = Request(database_path);
  const auto opened = engine_api::OpenDmlUpdateStatementMgaAuthorityV1(request);
  Require(opened.ok, "rollback savepoint creation failed");
  const auto rolled_back = engine_api::RollbackDmlUpdateStatementMgaAuthorityV1(
      Transition(request, opened.authority));
  Require(rolled_back.ok &&
              rolled_back.authority.lifecycle ==
                  engine_api::MgaDmlUpdateStatementSavepointLifecycleV1::rolled_back &&
              rolled_back.authority.publication_barrier_uuid ==
                  opened.authority.publication_barrier_uuid &&
              rolled_back.authority.publication_barrier_generation == 1 &&
              !rolled_back.authority.publication_barrier_present &&
              rolled_back.authority.durable_presence_sha256 !=
                  opened.authority.durable_presence_sha256 &&
              Nonzero(rolled_back.authority.durable_presence_sha256),
          "rollback did not publish exact durable terminal authority");
  const auto active = engine_api::ActiveMgaSavepointNames(request.context);
  Require(std::find(active.begin(), active.end(),
                    PrivateMarker(opened.authority.savepoint_uuid)) ==
              active.end(),
          "rolled-back statement savepoint remained active");

  const auto reopened = engine_api::RecoverDmlUpdateStatementMgaAuthorityV1(
      Recover(Request(database_path), rolled_back.authority));
  Require(reopened.ok && reopened.authority == rolled_back.authority,
          "durable rollback authority did not recover after reopen");
  const auto duplicate = engine_api::RollbackDmlUpdateStatementMgaAuthorityV1(
      Transition(request, rolled_back.authority));
  RequireDiagnostic(duplicate.diagnostic,
                    engine_api::kDmlUpdateStatementMgaDiagnosticStale,
                    "terminal rollback was applied twice");
}

void TestReleaseBarrierRecoveryAndContradiction(
    const std::filesystem::path& database_path) {
  const auto request = Request(database_path);
  const auto opened = engine_api::OpenDmlUpdateStatementMgaAuthorityV1(request);
  Require(opened.ok, "release savepoint creation failed");
  const std::string prepared_durs_barrier_uuid =
      opened.authority.publication_barrier_uuid;
  const std::uint64_t prepared_durs_barrier_generation =
      opened.authority.publication_barrier_generation;
  Require(ExactNonzeroUuid(prepared_durs_barrier_uuid) &&
              prepared_durs_barrier_generation == 1 &&
              !opened.authority.publication_barrier_present,
          "prepared DURS could not bind a reserved pre-release barrier");
  const auto released = engine_api::ReleaseDmlUpdateStatementMgaAuthorityV1(
      Transition(request, opened.authority));
  Require(released.ok &&
              released.authority.lifecycle ==
                  engine_api::MgaDmlUpdateStatementSavepointLifecycleV1::released &&
              ExactNonzeroUuid(
                  released.authority.publication_barrier_uuid) &&
              released.authority.publication_barrier_generation == 1 &&
              released.authority.publication_barrier_present &&
              released.authority.publication_barrier_uuid ==
                  prepared_durs_barrier_uuid &&
              released.authority.publication_barrier_generation ==
                  prepared_durs_barrier_generation &&
              released.authority.publication_barrier_uuid !=
                  released.authority.savepoint_uuid &&
              released.authority.durable_presence_sha256 !=
                  opened.authority.durable_presence_sha256 &&
              Nonzero(released.authority.durable_presence_sha256),
          "release did not issue exact durable publication-barrier authority");
  const auto active = engine_api::ActiveMgaSavepointNames(request.context);
  Require(std::find(active.begin(), active.end(),
                    PrivateMarker(opened.authority.savepoint_uuid)) ==
              active.end(),
          "released statement savepoint remained active");

  const auto reopened = engine_api::RecoverDmlUpdateStatementMgaAuthorityV1(
      Recover(Request(database_path), released.authority));
  Require(reopened.ok && reopened.authority == released.authority,
          "durable release/barrier authority did not recover after reopen");

  AppendContradictoryActiveMarker(database_path, released.authority);
  const auto contradictory =
      engine_api::RecoverDmlUpdateStatementMgaAuthorityV1(
          Recover(Request(database_path), released.authority));
  RequireDiagnostic(contradictory.diagnostic,
                    engine_api::kDmlUpdateStatementMgaDiagnosticFailed,
                    "contradictory active savepoint plus barrier was accepted");
}

}  // namespace

int main() {
  TemporaryDirectory temporary;
  TestFreshIdentityAndStaleRefusals(temporary.path() / "fresh.sdb");
  TestRollbackAndRecovery(temporary.path() / "rollback.sdb");
  TestReleaseBarrierRecoveryAndContradiction(
      temporary.path() / "release.sdb");
  std::cout
      << "sbsql_dml_update_statement_mga_authority_provider_conformance: PASS\n";
  return EXIT_SUCCESS;
}
