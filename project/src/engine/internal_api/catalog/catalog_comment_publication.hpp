// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "ddl/comment_api.hpp"
#include <functional>

namespace scratchbird::engine::internal_api {
class CatalogCommentPublicationCustody final {
 public:
  CatalogCommentPublicationCustody(const CatalogCommentPublicationCustody&) = delete;
  CatalogCommentPublicationCustody& operator=(const CatalogCommentPublicationCustody&) = delete;
  EngineApiDiagnostic Readmit() const { return readmit_(); }
 private:
  explicit CatalogCommentPublicationCustody(std::function<EngineApiDiagnostic()> readmit)
      : readmit_(std::move(readmit)) {}
  friend EngineCommentOnObjectResult EngineCatalogPublishComment(const EngineCommentOnObjectRequest&);
  const std::function<EngineApiDiagnostic()> readmit_;
};
} // namespace scratchbird::engine::internal_api
