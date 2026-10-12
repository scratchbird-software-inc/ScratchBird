// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_diagnostics.hpp"

namespace scratchbird::engine::internal_api {

// A runtime observation must not publish a savepoint watermark over an
// unpublished event range. Same-thread inventory ownership is recursive, so
// it cannot itself exclude this reentry. This lexical guard is allocation-free
// and nested scopes retain all affected database owners.
class MgaPublicationCallbackScope final {
 public:
  explicit MgaPublicationCallbackScope(const EngineRequestContext& context) noexcept
      : database_uuid_(context.database_uuid), database_path_(context.database_path), prior_(current_) { current_ = this; }
  ~MgaPublicationCallbackScope() { current_ = prior_; }
  MgaPublicationCallbackScope(const MgaPublicationCallbackScope&) = delete;
  MgaPublicationCallbackScope& operator=(const MgaPublicationCallbackScope&) = delete;
  bool savepoint_mutation_refused() const noexcept { return refused_; }
  static bool RefuseSavepointMutation(const EngineRequestContext& context) noexcept {
    return RefuseSavepointMutation(context.database_uuid, context.database_path);
  }
  static bool RefuseSavepointMutation(const EngineUuid& database_uuid,
                                     const std::string& database_path) noexcept {
    bool blocked = false;
    for (auto* scope = current_; scope; scope = scope->prior_) {
      if ((!scope->database_uuid_.is_nil() && scope->database_uuid_ == database_uuid) ||
          scope->database_path_ == database_path) {
        scope->refused_ = true;
        blocked = true;
      }
    }
    return blocked;
  }

 private:
  const EngineUuid database_uuid_;
  const std::string& database_path_;
  MgaPublicationCallbackScope* prior_;
  bool refused_ = false;
  inline static thread_local MgaPublicationCallbackScope* current_ = nullptr;
};

inline EngineApiDiagnostic CheckMgaPublicationCallback(
    const EngineRequestContext& context, const std::string& cancellation_key) {
  if (context.query_cancellation_requested) {
    MgaPublicationCallbackScope scope(context);
    const bool cancelled = context.query_cancellation_requested();
    if (scope.savepoint_mutation_refused())
      return MakeInvalidRequestDiagnostic("mga.publication.callback",
          "savepoint_mutation_during_publication_callback");
    if (cancelled)
      return MakeEngineApiDiagnostic("PROCESS.CANCELLED", cancellation_key,
          "cancellation was observed before publication", true);
  }
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", {}, false);
}

} // namespace scratchbird::engine::internal_api
