#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_bulk_export_stream_runtime.hpp"
namespace scratchbird::engine::internal_api {struct SblrBulkExportStreamCoordinationResult{bool ok=false;scratchbird::engine::sblr::SblrBulkExportStreamDescriptorV1 descriptor{};EngineApiDiagnostic diagnostic;};SblrBulkExportStreamCoordinationResult CompileSblrBulkExportStreamDescriptor(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint32_t,std::uint64_t);SblrBulkExportStreamCoordinationResult ConsumeSblrBulkExportStreamDescriptor(const EngineRequestContext&,const scratchbird::engine::sblr::SblrBulkExportStreamDescriptorV1&);}
