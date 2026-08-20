#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

enum class SblrSavepointState : std::uint8_t { reserved=1, active=2, revoked=3, released=4 };

struct SblrSavepointSnapshot {
  std::string descriptor_uuid;
  std::uint64_t descriptor_generation{0};
  std::string savepoint_uuid;
  std::uint64_t savepoint_generation{0};
  std::string transaction_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t transaction_ordinal{0};
  std::string statement_receipt_uuid;
  std::uint64_t symbol_occurrence_id{0};
  std::string canonical_symbol_sha256;
  std::string transaction_handle_evidence_sha256;
  std::string descriptor_evidence_sha256;
  std::uint64_t stack_generation{0};
  SblrSavepointState state{SblrSavepointState::revoked};
};

struct SblrSavepointCoordinatorResult {
  bool ok{false};
  EngineApiDiagnostic diagnostic;
  SblrSavepointSnapshot snapshot;
  std::vector<EngineEvidenceReference> evidence;
};

SblrSavepointCoordinatorResult ReserveSblrSavepoint(
    const EngineRequestContext&, const std::string& statement_receipt_uuid,
    const std::string& transaction_handle_evidence_sha256,
    std::uint64_t symbol_occurrence_id,
    const std::string& canonical_symbol_sha256);
SblrSavepointCoordinatorResult LookupSblrSavepoint(
    const EngineRequestContext&, const std::string& statement_receipt_uuid,
    const std::string& descriptor_uuid, std::uint64_t descriptor_generation,
    const std::string& descriptor_evidence_sha256);
SblrSavepointCoordinatorResult ActivateSblrSavepoint(
    const EngineRequestContext&, const std::string& statement_receipt_uuid,
    const std::string& descriptor_uuid, std::uint64_t descriptor_generation,
    const std::string& descriptor_evidence_sha256,
    std::uint64_t executor_availability_generation);
SblrSavepointCoordinatorResult ReleaseSblrSavepoint(
    const EngineRequestContext&, const std::string& savepoint_uuid,
    std::uint64_t savepoint_generation, std::uint64_t transaction_ordinal,
    std::uint64_t admitted_stack_generation,
    const std::string& admitted_savepoint_evidence_sha256,
    std::uint64_t executor_availability_generation);
SblrSavepointCoordinatorResult RollbackToSblrSavepoint(
    const EngineRequestContext&, const std::string& savepoint_uuid,
    std::uint64_t savepoint_generation, std::uint64_t transaction_ordinal,
    std::uint64_t admitted_stack_generation,
    const std::string& admitted_savepoint_evidence_sha256,
    std::uint64_t executor_availability_generation);
EngineApiDiagnostic RecoverSblrSavepointCoordinator(const EngineRequestContext&);

}  // namespace scratchbird::engine::internal_api
