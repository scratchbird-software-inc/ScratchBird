#pragma once

#include "api_types.hpp"

#include <cstdint>
#include <string>

namespace scratchbird::engine::internal_api {
struct SblrCursorOpenSnapshot {
  std::string receipt_uuid;
  std::string descriptor_uuid;
  std::string plan_uuid;
  std::string plan_evidence_sha256;
  std::string row_shape_uuid;
  std::string transaction_uuid;
  std::string session_uuid;
  std::string security_uuid;
  std::string descriptor_evidence_sha256;
  std::string cursor_uuid;
  std::string cursor_evidence_sha256;
  std::uint64_t occurrence = 0;
  std::uint64_t descriptor_generation = 0;
  std::uint64_t plan_generation = 0;
  std::uint64_t row_shape_generation = 0;
  std::uint64_t catalog_generation = 0;
  std::uint64_t availability_generation = 0;
  std::uint64_t cursor_generation = 0;
  std::uint64_t position_generation = 0;
  std::uint8_t mode = 0;
  std::uint8_t hold = 0;
  std::uint32_t fetch_size = 0;
};

struct SblrCursorOpenResult {
  bool ok = false;
  SblrCursorOpenSnapshot snapshot;
  EngineApiDiagnostic diagnostic;
};

SblrCursorOpenResult CompileAndPublishSblrExecutablePlanReceipt(
    const EngineRequestContext&, const std::string&, std::uint64_t,
    std::uint8_t, std::uint8_t, std::uint32_t, std::uint64_t);
SblrCursorOpenResult OpenSblrCursor(const EngineRequestContext&,
                                    const std::string&, std::uint64_t,
                                    const std::string&, std::uint64_t);
SblrCursorOpenResult FetchSblrCursor(const EngineRequestContext&,
    const std::string&, std::uint64_t, std::uint64_t, const std::string&,
    std::uint64_t, std::uint32_t);
SblrCursorOpenResult CloseSblrCursor(const EngineRequestContext&,
    const std::string&, std::uint64_t, std::uint64_t, const std::string&,
    std::uint64_t, std::uint8_t);
EngineApiDiagnostic RecoverSblrOpenCursors(const EngineRequestContext&);
}  // namespace scratchbird::engine::internal_api
